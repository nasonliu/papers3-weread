// img_render.cpp — 图片渲染模块实现
//
// 设计要点：
// - 下载：自实现精简 HTTPS GET（esp_http_client + esp_crt_bundle_attach，参考 weread_client.cpp），
//   响应体收进 PSRAM，上限 512KB，超限主动中断并判定失败；不带 Cookie（图片 CDN 无需鉴权，也避免泄漏 wr_skey）
// - 缓存：SD /weread/img/<url的md5>.<jpg|png>，扩展名由 magic bytes 决定（FF D8=jpeg，89 50 4E 47=png）
// - 解码：JPEG 用 TJpg_Decoder（内存数组输入，JPG_SCALE 1/2/4/8 档）；PNG 用 PNGdec（行回调）
//   缩放策略：先选“解码后不小于目标”的最大解码档（避免上采样），再在像素回调里最近邻抽样到目标尺寸
// - 灰度：回调里逐像素 RGB565 → BT.601 亮度 → color565(v,v,v)，直接画进 canvas，不整帧缓存
// - 内存：文件缓冲 / PNG 对象 / PNG 行缓冲一律 PSRAM；DRAM 侧只有 TJpgDec 自带工作区（约 3.5KB bss）
//
// 已知限制（均来自解码库本身，调用方按失败处理即可）：
//   TJpgDec 不支持渐进式 JPEG；PNGdec 不支持隔行（Adam7）PNG；
//   PNGdec 1.1.6 行缓冲固定 2562 字节（存当前行+上一行），iPitch>1265 的宽 PNG 无法安全解码，本模块直接拒绝
// 注意：本模块不可重入（解码回调靠全局上下文传参），请单线程逐张调用

#include "img_render.h"
#include "storage.h"        // storage_sd_ok() / SD_WEREAD_DIR
#include "weread_crypto.h"  // weread::md5_hex()
#include "weread_client.h"  // WR.cookieHeader()（需鉴权图源重试用）
#include <WiFi.h>           // WiFi.status()（网络断开时跳过下载）
#include <M5Unified.h>      // M5.Display（缩略图生成用小画布）
#include <SD.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <set>
#include <TJpg_Decoder.h>
#include <PNGdec.h>
#include <AnimatedGIF.h>
#include <new>              // placement new

// ---- 常量 ----
#define IMG_CACHE_DIR   "/weread/img"   // 老扁平图片缓存（只读回退 + 懒迁移源）
#define IMG_CACHE_DIR2  "/weread/img2"  // 新分目录缓存（md5 前两位分片；FAT 大目录 open 要线性扫，分片才快）
static const size_t   IMG_MAX_BYTES  = 512 * 1024;  // 下载/解码文件上限（超出丢弃）
static const uint32_t IMG_HTTP_TIMEOUT = 8000;      // ms（缩短单图超时，卡死出口更快）
// 普通浏览器 UA（CDN 按 UA 防风类比按 cookie 多）
static const char IMG_UA[] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36";
// PNGdec 内部行缓冲 PNG_MAX_BUFFERED_PIXELS=2562 需容纳 当前行+上一行+对齐：
// 2*(iPitch+1)+32 <= 2562 → iPitch <= 1265（见 png.inl DecodePNG 的 pCurr/pPrev 计算）
static const int PNG_SAFE_PITCH = 1265;

// ---- 绘制上下文（C 风格静态解码回调借此传参）----
struct DrawCtx {
    M5Canvas* cv = nullptr;
    int dw = 0, dh = 0;          // 解码输出尺寸（采样源）
    int tw = 0, th = 0;          // 目标尺寸（等比适配后）
    int ox = 0, oy = 0;          // 画布上的绘制原点（区域内居中）
    uint16_t* line = nullptr;    // PNG 用：整行 RGB565 缓冲（PSRAM）
};
static DrawCtx g_dc;
static PNG* g_png = nullptr;     // 当前解码中的 PNG 对象（PSRAM）
static std::set<String> g_fail_set; // 会话级负缓存（本次开机内失败的 URL；重启清空给第二次机会）
static std::set<String> g_no_cache; // 会话级"无文件"负缓存（cached miss 一次后不再反复扫目录）

// ---- 缩略图（书架等小图场景的提速关键）----
// 小目标绘制若每次全尺寸解码（600x900 的封面 TJpgDec 全 MCU 走一遍 ~0.7s/张），
// 书架一页 5 张就是 ~4s，翻页必卡。策略：下载时一次性解码生成 128x192 以内的
// RGB565 位图落 SD（<缓存路径>.thb），小目标绘制直接块搬（~0.1s）。
#define THUMB_W 128
#define THUMB_H 192
static bool make_thumb(const String& img_path);   // 定义在文件尾部 draw_full 之后

// 绘制互斥：解码回调靠全局上下文 g_dc 传参（模块不可重入），
// 主任务画封面与后台 netjob 生成缩略图都会用，必须串行
static SemaphoreHandle_t g_draw_mtx = nullptr;
#define DRAW_LOCK()   do { if (g_draw_mtx) xSemaphoreTakeRecursive(g_draw_mtx, portMAX_DELAY); } while (0)
#define DRAW_UNLOCK() do { if (g_draw_mtx) xSemaphoreGiveRecursive(g_draw_mtx); } while (0)

// ==================== 下载 ====================

// esp_http_client 事件回调：响应体累积进 PSRAM 缓冲，超限置标志并中断请求
struct ImgDlBuf {
    uint8_t* buf;
    size_t   len;
    size_t   cap;
    bool     overflow;
};

// 当前请求的接收缓冲（keep-alive 会话复用 client 时 user_data 不便改，用全局指针传）
static ImgDlBuf* g_cur_dl = nullptr;

static esp_err_t img_http_event(esp_http_client_event_t* evt) {
    ImgDlBuf* ib = g_cur_dl;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ib && evt->data_len > 0) {
        if (ib->len + (size_t)evt->data_len > ib->cap) {
            ib->overflow = true;
            return ESP_FAIL;    // 超上限：主动中断
        }
        memcpy(ib->buf + ib->len, evt->data, evt->data_len);
        ib->len += evt->data_len;
    }
    return ESP_OK;
}

// keep-alive 会话：同 host 的图片请求复用一条 TLS 连接（省每图 1-2s 握手）
static esp_http_client_handle_t g_img_client = nullptr;
static String g_img_host;
static char g_img_host_buf[96]; // cfg.host 用持久缓冲（esp_http_client 不复制该指针，局部 String 会悬垂！）
static unsigned long g_img_last_use = 0; // 最近使用时间（闲置关闭用）
static volatile int g_img_in_use = 0;    // 在途计数（housekeeping 只在 0 时关，否则关掉在途句柄必崩）
static int g_consec_fail = 0;            // 连续失败计数（熔断用）
static unsigned long g_breaker_until = 0; // 熔断截止时间：连续失败 3 次后 60s 内不再尝试（防重试风暴卡死主循环）

static void img_client_drop() {
    if (g_img_client) {
        esp_http_client_close(g_img_client);
        esp_http_client_cleanup(g_img_client);
        g_img_client = nullptr;
    }
}

// 精简 HTTPS GET：成功返回接收字节数，失败/超限/非 200 返回 0
// with_cookie=true 时带 weread cookie（res.weread.qq.com 等需鉴权的图源用）
// fail_kind_out（可空）：1=连接级失败（DNS/建连），2=HTTP 层失败（4xx/5xx）
static size_t https_get(const String& url, uint8_t* buf, size_t cap, bool with_cookie, int* fail_kind_out = nullptr) {
    if (fail_kind_out) *fail_kind_out = 0;
    // 从 URL 提取 host（keep-alive 按 host 复用会话）
    int hs = url.indexOf("://");
    int he = url.indexOf('/', hs + 3);
    String host = (hs >= 0) ? url.substring(hs + 3, he > 0 ? he : (int)url.length()) : String("");
    if (!host.length()) return 0;

    ImgDlBuf ib = { buf, 0, cap, false };
    struct UseGuard { // RAII：在途计数，housekeeping 据此不关句柄
        volatile int& c; UseGuard(volatile int& c_) : c(c_) { c++; } ~UseGuard() { c--; }
    } guard(g_img_in_use);
    // 节流：图片请求间隔 ≥300ms（连发会触发风控/服务器 RST）
    static unsigned long last_req = 0;
    unsigned long now = millis();
    if (last_req && now - last_req < 300) delay(300 - (now - last_req));
    last_req = millis();
    for (int attempt = 0; attempt < 2; attempt++) {
        // 会话不存在/host 变了/上次失败被丢弃 → 重建
        if (!g_img_client || g_img_host != host) {
            img_client_drop();
            strlcpy(g_img_host_buf, host.c_str(), sizeof(g_img_host_buf)); // 持久化，防悬垂
            esp_http_client_config_t cfg = {};
            cfg.host = g_img_host_buf;
            cfg.path = "/"; // init 校验要求 host+path 成对出现（后续 set_url 会覆盖）
            cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;  // 内置 CA 证书包
            cfg.event_handler = img_http_event;
            cfg.timeout_ms = IMG_HTTP_TIMEOUT;
            cfg.buffer_size = 4096;
            cfg.keep_alive_enable = true;
            g_img_client = esp_http_client_init(&cfg);
            g_img_host = host;
            if (!g_img_client) return 0;
        }
        g_cur_dl = &ib;
        esp_http_client_set_url(g_img_client, url.c_str());
        esp_http_client_set_header(g_img_client, "User-Agent", IMG_UA);
        esp_http_client_set_header(g_img_client, "Accept", "image/png,image/jpeg,image/gif,image/*,*/*");
        if (with_cookie) {
            String ck = WR.cookieHeader();
            if (ck.length()) esp_http_client_set_header(g_img_client, "Cookie", ck.c_str());
        } else {
            esp_http_client_set_header(g_img_client, "Cookie", nullptr); // 清掉上次可能带的 cookie
        }
        esp_err_t err = esp_http_client_perform(g_img_client);
        int status = esp_http_client_get_status_code(g_img_client);
        if (err != ESP_OK) {
            // 连接级失败（含空闲被服务端断开）：丢弃会话重建重试一次
            if (fail_kind_out) *fail_kind_out = 1;
            Serial.printf("[img] 连接失败，重建重试 err=%s url=%.60s\n", esp_err_to_name(err), url.c_str());
            img_client_drop();
            continue;
        }
        if (ib.overflow || status != 200 || ib.len == 0) {
            if (fail_kind_out) *fail_kind_out = 2;
            Serial.printf("[img] 下载失败 status=%d len=%u%s url=%.80s\n",
                          status, (unsigned)ib.len, ib.overflow ? " (>上限)" : "", url.c_str());
            return 0; // HTTP 层失败是会话正常应答，不重建
        }
        g_img_last_use = millis();
        return ib.len;
    }
    return 0;
}

// ==================== SD 缓存 ====================

// 缓存命中检查（要求存在且非空；直接 open 一次，比 exists+open 少一次目录扫描）
static bool cache_hit(const String& path) {
    File f = SD.open(path.c_str(), "r");
    if (!f) return false;
    size_t s = f.size();
    f.close();
    return s > 0;
}

// 缓存基路径：新布局 /weread/img2/<md5前两位>/<md5>（分片小目录，open 只要几毫秒）。
// 老布局 /weread/img/<md5>（扁平大目录，open 扫目录 ~200ms）只读回退，懒迁移走。
static String cache_base(const String& url, bool sharded) {
    String h = weread::md5_hex(url);
    if (sharded) return String(IMG_CACHE_DIR2) + "/" + h.substring(0, 2) + "/" + h;
    return String(IMG_CACHE_DIR) + "/" + h;
}

// 读整个 SD 文件到 PSRAM（最多 cap 字节）；成功返回缓冲（调用方负责 heap_caps_free），*out_len 为长度，失败 nullptr
static uint8_t* read_sd_psram(const String& path, size_t cap, size_t* out_len) {
    *out_len = 0;
    File f = SD.open(path.c_str(), "r");
    if (!f) return nullptr;
    size_t fsz = f.size();
    if (fsz == 0) { f.close(); return nullptr; }
    size_t rd = fsz < cap ? fsz : cap;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(rd, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return nullptr; }
    size_t got = f.read(buf, rd);
    f.close();
    if (got != rd) { heap_caps_free(buf); return nullptr; }
    *out_len = rd;
    return buf;
}

// magic bytes 判定：0=未知 1=jpeg 2=png 3=gif
static int img_format(const uint8_t* d, size_t n) {
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8) return 1;
    if (n >= 4 && d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47) return 2;
    if (n >= 4 && d[0] == 0x47 && d[1] == 0x49 && d[2] == 0x46 && d[3] == 0x38) return 3; // "GIF8"
    return 0;
}

namespace img_render {

// 会话闲置关闭（主循环周期调用）：keep-alive TLS 常驻 ~50KB DRAM，30s 不用就关。
// 有在途请求时绝不关（关掉在途句柄 = 崩溃）
void housekeeping() {
    if (g_img_client && g_img_in_use == 0 && millis() - g_img_last_use > 30000) {
        Serial.println("[img] 会话闲置 30s，关闭释放内存");
        img_client_drop();
    }
}

void begin() {
    if (!g_draw_mtx) g_draw_mtx = xSemaphoreCreateRecursiveMutex();
    if (!storage_sd_ok()) {
        Serial.println("[img] SD 不可用，图片缓存关闭");
        return;
    }
    SD.mkdir(SD_WEREAD_DIR);    // 逐级建目录（已存在时 mkdir 返回失败，忽略）
    SD.mkdir(IMG_CACHE_DIR);
    SD.mkdir(IMG_CACHE_DIR2);
    Serial.printf("[img] 缓存目录就绪 %s（新布局 %s）\n", IMG_CACHE_DIR, IMG_CACHE_DIR2);
}

String fetch(const String& url) {
    if (!storage_sd_ok() || url.isEmpty()) return "";

    // 1) 查缓存（jpg/png/gif 三种扩展名都试，分目录/扁平两种布局都找）
    String base = cache_base(url, true);   // 新缓存写分目录
    String nkey = weread::md5_hex(url);    // 负缓存键（与布局无关）
    g_no_cache.erase(url);                 // 下载意图明确，忽略"无文件"负缓存（cached 会真实查一遍）
    String hit = cached(url);
    if (hit.length()) return hit;
    // 会话级负缓存：本次开机失败过的不再反复试（重启即清空，给瞬时失败第二次机会）
    if (g_fail_set.count(nkey)) return "";
    // 熔断：连续失败 3 次后 60s 内不再尝试下载（网络挂时防重试风暴卡死主循环）
    if (millis() < g_breaker_until) return "";
    // 网络都没连上就别下了（DNS 必败，省 15 秒/张）
    if (WiFi.status() != WL_CONNECTED) { g_fail_set.insert(nkey); return ""; }

    // 2) 下载到 PSRAM（裸取失败用 weread cookie 重试：res.weread.qq.com 插图需鉴权；连接级失败不重试）
    uint8_t* buf = (uint8_t*)heap_caps_malloc(IMG_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { Serial.println("[img] PSRAM 分配失败（下载缓冲）"); return ""; }
    int kind = 0;
    size_t n = https_get(url, buf, IMG_MAX_BYTES, false, &kind);
    if (n == 0 && kind == 2) n = https_get(url, buf, IMG_MAX_BYTES, true, &kind); // 仅 HTTP 层失败才用 cookie 重试
    if (n < 8) {  // 过短不可能是有效图片（也保证下面读前 4 字节安全）
        heap_caps_free(buf);
        g_fail_set.insert(nkey); // 记负缓存（仅本次开机有效）
        // 熔断计数：连续 3 次失败熔断 60s
        if (++g_consec_fail >= 3) {
            g_consec_fail = 0;
            g_breaker_until = millis() + 60000;
            Serial.println("[img] 连续失败 3 次，熔断 60s 暂停下载（防卡死）");
        }
        return "";
    }
    g_consec_fail = 0; // 成功一次就解除熔断计数

    // 3) magic bytes 定扩展名；非 jpeg/png 丢弃
    int fmt = img_format(buf, n);
    if (fmt == 0) {
        Serial.printf("[img] 非 jpeg/png（magic=%02X %02X %02X %02X），丢弃 %s\n",
                      buf[0], buf[1], buf[2], buf[3], url.c_str());
        heap_caps_free(buf);
        g_fail_set.insert(nkey);
        return "";
    }
    String path = base + (fmt == 1 ? ".jpg" : fmt == 2 ? ".png" : ".gif");

    // 4) 写 SD 缓存（分目录子目录按需建）
    SD.mkdir(SD_WEREAD_DIR);    // begin() 未跑时兜底
    SD.mkdir(IMG_CACHE_DIR2);
    String sub = String(IMG_CACHE_DIR2) + "/" + weread::md5_hex(url).substring(0, 2);
    SD.mkdir(sub.c_str());
    File f = SD.open(path.c_str(), "w");
    if (!f) { heap_caps_free(buf); Serial.println("[img] 缓存写打开失败"); return ""; }
    size_t wn = f.write(buf, n);
    f.close();
    heap_caps_free(buf);
    if (wn != n) { SD.remove(path.c_str()); Serial.println("[img] 缓存写不完整，已删除"); return ""; }
    g_fail_set.erase(nkey); // 成功则清除负缓存记录
    Serial.printf("[img] 已缓存 %s（%u B）\n", path.c_str(), (unsigned)n);
    make_thumb(path); // 顺手生成缩略图（一次性全解码；书架等小图绘制之后走快路径）
    return path;
}

// 老缓存（扁平路径）懒迁移到分目录：rename 是元数据操作，首次访问花一次目录扫描，
// 之后永久走小目录（否则老用户的几百个扁平缓存文件永远享受不到提速）
static std::set<String> g_shard_dirs; // 本机已建过的分目录（防每次 mkdir 重复扫目录）
static String migrate_if_legacy(const String& legacy_path) {
    int sl = legacy_path.lastIndexOf('/');
    if (sl < 0) return legacy_path;
    String dir = legacy_path.substring(0, sl);
    if (dir != IMG_CACHE_DIR) return legacy_path;   // 已在新布局
    String name = legacy_path.substring(sl + 1);    // <md5>.<ext>
    String sub = String(IMG_CACHE_DIR2) + "/" + name.substring(0, 2);
    if (!g_shard_dirs.count(sub)) {
        SD.mkdir(IMG_CACHE_DIR2);
        SD.mkdir(sub.c_str());
        g_shard_dirs.insert(sub);
    }
    String target = sub + "/" + name;
    if (SD.rename(legacy_path.c_str(), target.c_str())) {
        String thb = legacy_path + ".thb";
        if (SD.exists(thb.c_str())) SD.rename(thb.c_str(), (target + ".thb").c_str());
        return target;
    }
    return legacy_path;
}

// 只查缓存不下载（分页估算用）；先找分目录（新布局），再回退扁平（老缓存，顺手迁移）
String cached(const String& url) {
    if (!storage_sd_ok() || url.isEmpty()) return "";
    if (g_no_cache.count(url)) return "";
    String found;
    for (int s = 1; s >= 0; s--) {
        String base = cache_base(url, s == 1);
        String pj = base + ".jpg";
        String pp = base + ".png";
        String pg = base + ".gif";
        if (cache_hit(pj)) { found = pj; break; }
        if (cache_hit(pp)) { found = pp; break; }
        if (cache_hit(pg)) { found = pg; break; }
    }
    if (found.length()) return migrate_if_legacy(found);
    g_no_cache.insert(url);
    return "";
}

// 下载任意内容到指定 SD 路径（tar 包等资源；无 magic 校验，上限 4MB）
String fetch_raw(const String& url, const String& out_path) {
    if (!storage_sd_ok() || url.isEmpty()) return "";
    if (cache_hit(out_path)) return out_path; // 已下载过

    const size_t CAP = 4 * 1024 * 1024; // tar 包可达 MB 级
    uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { Serial.println("[img] PSRAM 分配失败（raw 下载缓冲）"); return ""; }
    size_t n = https_get(url, buf, CAP, false);
    Serial.printf("[img] raw 裸取 n=%u\n", (unsigned)n);
    if (n == 0) {
        n = https_get(url, buf, CAP, true); // 鉴权重试
        Serial.printf("[img] raw 带 cookie n=%u\n", (unsigned)n);
    }
    if (n == 0) { heap_caps_free(buf); return ""; }

    // 确保父目录存在（取最后一个 / 之前部分逐级建）
    int slash = out_path.lastIndexOf('/');
    if (slash > 0) {
        String dir = out_path.substring(0, slash);
        String acc;
        for (int i = 1; i < (int)dir.length(); i++) {
            if (dir[i] == '/') { SD.mkdir(acc.length() ? acc : "/"); }
            acc += dir[i];
        }
        SD.mkdir(dir.c_str());
    }
    File f = SD.open(out_path.c_str(), "w");
    if (!f) { heap_caps_free(buf); Serial.println("[img] raw 写打开失败"); return ""; }
    size_t wn = f.write(buf, n);
    f.close();
    heap_caps_free(buf);
    if (wn != n) { SD.remove(out_path.c_str()); Serial.println("[img] raw 写不完整，已删除"); return ""; }
    Serial.printf("[img] 已下载 %s（%u B）\n", out_path.c_str(), (unsigned)n);
    return out_path;
}

// ==================== 尺寸解析（只读文件头，不全解码）====================

bool size(const String& path, int& w, int& h) {
    w = h = 0;
    // 先读 26 字节判格式（PNG 的 IHDR 宽高就在其中）
    File f = SD.open(path.c_str(), "r");
    if (!f) return false;
    uint8_t head[26];
    size_t hn = f.read(head, sizeof(head));
    f.close();
    int fmt = img_format(head, hn);

    if (fmt == 2) {
        // PNG：8 字节签名 + 4 长度 + "IHDR" + 宽(4,大端) + 高(4,大端)
        if (hn < 26 || memcmp(head + 12, "IHDR", 4) != 0) return false;
        uint32_t w32 = ((uint32_t)head[16] << 24) | ((uint32_t)head[17] << 16) | ((uint32_t)head[18] << 8) | head[19];
        uint32_t h32 = ((uint32_t)head[20] << 24) | ((uint32_t)head[21] << 16) | ((uint32_t)head[22] << 8) | head[23];
        if (w32 == 0 || h32 == 0 || w32 > 0x7FFFFFFF || h32 > 0x7FFFFFFF) return false;
        w = (int)w32; h = (int)h32;
        return true;
    }
    if (fmt == 3) {
        // GIF：6 字节签名后紧跟 宽(2,小端) + 高(2,小端)
        if (hn < 10) return false;
        w = head[6] | (head[7] << 8);
        h = head[8] | (head[9] << 8);
        return w > 0 && h > 0;
    }
    if (fmt == 1) {
        // JPEG：SOF 位置不定（前面可能有 EXIF 等 APPn 段），
        // 交给 TJpgDec 的 jd_prepare 解析——它只读头部到 SOS 为止，不做全解码
        size_t n = 0;
        uint8_t* buf = read_sd_psram(path, IMG_MAX_BYTES, &n);
        if (!buf) return false;
        uint16_t jw = 0, jh = 0;
        JRESULT jr = TJpgDec.getJpgSize(&jw, &jh, buf, (uint32_t)n);
        heap_caps_free(buf);
        if (jr != JDR_OK || jw == 0 || jh == 0) return false;
        w = jw; h = jh;
        return true;
    }
    return false;
}

// ==================== 绘制 ====================

} // namespace img_render

// RGB565 → BT.601 亮度灰（v=v=v 写回 565，与墨水屏灰度显示一致）
static inline uint16_t to_gray565(M5Canvas* cv, uint16_t c) {
    uint32_t r5 = (c >> 11) & 0x1F, g6 = (c >> 5) & 0x3F, b5 = c & 0x1F;
    uint32_t r8 = (r5 << 3) | (r5 >> 2);    // 5→8 位扩展
    uint32_t g8 = (g6 << 2) | (g6 >> 4);    // 6→8 位扩展
    uint32_t b8 = (b5 << 3) | (b5 >> 2);
    uint8_t v = (uint8_t)((r8 * 77 + g8 * 150 + b8 * 29) >> 8);  // 77+150+29=256，满白得 255
    return cv->color565(v, v, v);
}

// 等比适配进 max_w×max_h（保宽高比），结果至少 1×1；允许小图放大
static void fit_box(int sw, int sh, int max_w, int max_h, int& tw, int& th) {
    if ((int64_t)sw * max_h > (int64_t)max_w * sh) {
        tw = max_w;
        th = (int)((int64_t)sh * max_w / sw);
    } else {
        th = max_h;
        tw = (int)((int64_t)sw * max_h / sh);
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
}

// ---- JPEG：TJpgDec 块回调（每块 8/16px 见方，数据为原生 RGB565——库默认 _swap=false，ESP32 小端直接读）----
// 最近邻抽样：输出像素 (ox,oy) ← 源像素 (ox*dw/tw, oy*dh/th)；按块覆盖范围反推输出行列区间
static bool jpg_block_cb(int16_t bx, int16_t by, uint16_t bw, uint16_t bh, uint16_t* data) {
    DrawCtx& d = g_dc;
    // 本块覆盖的输出行 [oy0,oy1)：oy*dh/th ∈ [by, by+bh)
    int oy0 = (int)(((int32_t)by * d.th + d.dh - 1) / d.dh);
    int oy1 = (int)(((int32_t)(by + bh) * d.th + d.dh - 1) / d.dh);
    if (oy1 > d.th) oy1 = d.th;
    // 本块覆盖的输出列 [ox0,ox1)
    int ox0 = (int)(((int32_t)bx * d.tw + d.dw - 1) / d.dw);
    int ox1 = (int)(((int32_t)(bx + bw) * d.tw + d.dw - 1) / d.dw);
    if (ox1 > d.tw) ox1 = d.tw;
    for (int oy = oy0; oy < oy1; oy++) {
        int sy = (int)((int32_t)oy * d.dh / d.th) - by;   // 块内行（数学上必落在 [0,bh)）
        if (sy < 0 || sy >= (int)bh) continue;            // 防御取整边界
        const uint16_t* srow = data + sy * bw;
        int py = d.oy + oy;
        for (int ox = ox0; ox < ox1; ox++) {
            int sx = (int)((int32_t)ox * d.dw / d.tw) - bx;
            if (sx < 0 || sx >= (int)bw) continue;
            d.cv->drawPixel(d.ox + ox, py, to_gray565(d.cv, srow[sx]));
        }
    }
    return true;    // true=继续解码
}

static int draw_jpg(M5Canvas* cv, const uint8_t* buf, size_t n, int x, int y, int max_w, int max_h) {
    uint16_t w0 = 0, h0 = 0;
    if (TJpgDec.getJpgSize(&w0, &h0, buf, (uint32_t)n) != JDR_OK || w0 == 0 || h0 == 0) {
        Serial.println("[img] JPEG 头解析失败（可能是不支持的渐进式 JPEG）");
        return 0;
    }
    // 先按原尺寸适配，用于选解码缩放档
    int tw0, th0;
    fit_box(w0, h0, max_w, max_h, tw0, th0);
    // 选“解码后仍不小于目标”的最大档位 8/4/2/1（省 CPU 且不上采样）
    int scale = 1;
    for (int s = 8; s >= 2; s >>= 1) {
        if ((int)(w0 / s) >= tw0 && (int)(h0 / s) >= th0) { scale = s; break; }
    }
    int dw = w0 / scale, dh = h0 / scale;   // 与 tjpgd 逐 MCU 截断取整的总输出一致
    if (dw < 1 || dh < 1) return 0;
    // 按解码后的真实尺寸重新适配（消除取整误差），并在区域内居中
    fit_box(dw, dh, max_w, max_h, g_dc.tw, g_dc.th);
    g_dc.cv = cv;
    g_dc.dw = dw; g_dc.dh = dh;
    g_dc.ox = x + (max_w - g_dc.tw) / 2;
    g_dc.oy = y + (max_h - g_dc.th) / 2;
    g_dc.line = nullptr;    // JPEG 路径不用行缓冲，清掉可能遗留的已释放指针

    TJpgDec.setJpgScale((uint8_t)scale);    // 1/2/4/8（库内部映射 0..3）
    TJpgDec.setCallback(jpg_block_cb);
    JRESULT jr = TJpgDec.drawJpg(0, 0, buf, (uint32_t)n);
    if (jr != JDR_OK) {
        Serial.printf("[img] JPEG 解码失败 jresult=%d\n", (int)jr);
        return 0;
    }
    return g_dc.th;
}

// ---- PNG：行回调（pPixels 为原始格式行，getLineAsRGB565 统一转 565，透明混白底）----
static int png_line_cb(PNGDRAW* p) {
    DrawCtx& d = g_dc;
    int y = p->y;   // 源行号（0 起）
    if (y < 0 || y >= d.dh || !d.line) return 1;
    // 转整行 RGB565（小端=ESP32 原生 uint16_t）；0x00BBGGRR 白底混合 alpha
    g_png->getLineAsRGB565(p, d.line, PNG_RGB565_LITTLE_ENDIAN, 0x00FFFFFF);
    // 本行覆盖的输出行 [oy0,oy1)：oy*dh/th == y
    int oy0 = (int)(((int32_t)y * d.th + d.dh - 1) / d.dh);
    int oy1 = (int)(((int32_t)(y + 1) * d.th + d.dh - 1) / d.dh);
    if (oy1 > d.th) oy1 = d.th;
    for (int oy = oy0; oy < oy1; oy++) {
        int py = d.oy + oy;
        for (int ox = 0; ox < d.tw; ox++) {
            int sx = (int)((int32_t)ox * d.dw / d.tw);
            d.cv->drawPixel(d.ox + ox, py, to_gray565(d.cv, d.line[sx]));
        }
    }
    return 1;   // 非 0=继续解码（返回 0 会让库报 PNG_QUIT_EARLY）
}

// 与 PNGdec PNGParseInfo 相同的行字节数计算（用于安全上限判断）
static int png_pitch(int w, int pixel_type, int bpp) {
    switch (pixel_type) {
        case PNG_PIXEL_GRAYSCALE:
        case PNG_PIXEL_INDEXED:          return (w * bpp + 7) / 8;
        case PNG_PIXEL_TRUECOLOR:        return ((3 * bpp) * w + 7) / 8;
        case PNG_PIXEL_GRAY_ALPHA:       return ((2 * bpp) * w + 7) / 8;
        case PNG_PIXEL_TRUECOLOR_ALPHA:  return ((4 * bpp) * w + 7) / 8;
    }
    return 0;
}

static int draw_png(M5Canvas* cv, uint8_t* buf, size_t n, int x, int y, int max_w, int max_h) {
    // PNG 对象约 38KB（内含 32KB zlib 状态 + 调色板 + 行缓冲），放 PSRAM
    void* mem = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM);
    if (!mem) { Serial.println("[img] PSRAM 分配失败（PNG 对象）"); return 0; }
    PNG* png = new (mem) PNG;   // openRAM 内部会 memset 初始化；PNG 平凡析构，用毕直接 free
    int orc = png->openRAM(buf, (int)n, png_line_cb);
    if (orc != PNG_SUCCESS) {
        Serial.printf("[img] PNG 头解析失败 rc=%d\n", orc);
        heap_caps_free(mem);
        return 0;
    }
    int w0 = png->getWidth(), h0 = png->getHeight();
    int pitch = png_pitch(w0, png->getPixelType(), png->getBpp());
    if (w0 <= 0 || h0 <= 0 || pitch <= 0 || pitch > PNG_SAFE_PITCH) {
        Serial.printf("[img] PNG 放弃：w=%d h=%d pitch=%d（库行缓冲上限 %d）\n",
                      w0, h0, pitch, PNG_SAFE_PITCH);
        heap_caps_free(mem);
        return 0;
    }
    // 行 RGB565 缓冲（getLineAsRGB565 要求 >= width*2 字节），PSRAM
    uint16_t* line = (uint16_t*)heap_caps_malloc((size_t)w0 * 2 + 16, MALLOC_CAP_SPIRAM);
    if (!line) { heap_caps_free(mem); Serial.println("[img] PSRAM 分配失败（PNG 行缓冲）"); return 0; }

    fit_box(w0, h0, max_w, max_h, g_dc.tw, g_dc.th);
    g_dc.cv = cv;
    g_dc.dw = w0; g_dc.dh = h0;
    g_dc.ox = x + (max_w - g_dc.tw) / 2;
    g_dc.oy = y + (max_h - g_dc.th) / 2;
    g_dc.line = line;
    g_png = png;

    int rc = png->decode(nullptr, 0);   // iOptions=0：不做 zlib CRC 校验，快 10~30%
    g_png = nullptr;
    heap_caps_free(line);
    heap_caps_free(mem);
    if (rc != PNG_SUCCESS) {   // 隔行 PNG 会在此报 PNG_UNSUPPORTED_FEATURE
        Serial.printf("[img] PNG 解码失败 err=%d\n", rc);
        return 0;
    }
    return g_dc.th;
}

// ---- GIF：行回调（pPixels 为 8 位调色板索引，pPalette 为 RGB565 小端；只播第一帧）----
static void gif_line_cb(GIFDRAW* p) {
    DrawCtx& d = g_dc;
    int cy = p->iY + p->y;   // 帧行 → 逻辑屏行
    if (cy < 0 || cy >= d.dh) return;
    // 本行覆盖的输出行 [oy0,oy1)
    int oy0 = (int)(((int32_t)cy * d.th + d.dh - 1) / d.dh);
    int oy1 = (int)(((int32_t)(cy + 1) * d.th + d.dh - 1) / d.dh);
    if (oy1 > d.th) oy1 = d.th;
    for (int oy = oy0; oy < oy1; oy++) {
        int py = d.oy + oy;
        for (int ox = 0; ox < d.tw; ox++) {
            int cx = (int)((int32_t)ox * d.dw / d.tw); // 逻辑屏列
            int fx = cx - p->iX;                        // 帧内列
            if (fx < 0 || fx >= p->iWidth) continue;
            uint8_t idx = p->pPixels[fx];
            if (p->ucHasTransparency && idx == p->ucTransparent) continue; // 透明=留白
            d.cv->drawPixel(d.ox + ox, py, to_gray565(d.cv, p->pPalette[idx]));
        }
    }
}

static int draw_gif(M5Canvas* cv, uint8_t* buf, size_t n, int x, int y, int max_w, int max_h) {
    // 逻辑屏尺寸从头部读（6 字节签名后 LE u16 宽/高）
    if (n < 10) return 0;
    int w0 = buf[6] | (buf[7] << 8), h0 = buf[8] | (buf[9] << 8);
    if (w0 <= 0 || h0 <= 0) return 0;

    // GIFIMAGE 对象较大（~30KB+），放 PSRAM
    void* mem = heap_caps_malloc(sizeof(AnimatedGIF), MALLOC_CAP_SPIRAM);
    if (!mem) { Serial.println("[img] PSRAM 分配失败（GIF 对象）"); return 0; }
    AnimatedGIF* gif = new (mem) AnimatedGIF;
    gif->begin(GIF_PALETTE_RGB565_LE);   // 调色板输出原生 RGB565 小端
    if (gif->open(buf, (int)n, gif_line_cb) == 0) {
        Serial.println("[img] GIF 打开失败");
        heap_caps_free(mem);
        return 0;
    }

    fit_box(w0, h0, max_w, max_h, g_dc.tw, g_dc.th);
    g_dc.cv = cv;
    g_dc.dw = w0; g_dc.dh = h0;
    g_dc.ox = x + (max_w - g_dc.tw) / 2;
    g_dc.oy = y + (max_h - g_dc.th) / 2;
    g_dc.line = nullptr;

    int rc = gif->playFrame(false, nullptr, nullptr); // 只画第一帧
    gif->close();
    heap_caps_free(mem);
    if (rc < 0) { Serial.printf("[img] GIF 解码失败 rc=%d\n", rc); return 0; }
    return g_dc.th;
}

// ---- 全尺寸解码绘制（原 draw 本体）----
static int draw_full(M5Canvas* cv, const String& path, int x, int y, int max_w, int max_h) {
    size_t n = 0;
    uint8_t* buf = read_sd_psram(path, IMG_MAX_BYTES, &n);
    if (!buf) { Serial.printf("[img] 读文件失败 %s\n", path.c_str()); return 0; }
    int fmt = img_format(buf, n);
    int rc = 0;
    if (fmt == 1)      rc = draw_jpg(cv, buf, n, x, y, max_w, max_h);
    else if (fmt == 2) rc = draw_png(cv, buf, n, x, y, max_w, max_h);
    else if (fmt == 3) rc = draw_gif(cv, buf, n, x, y, max_w, max_h);
    else Serial.printf("[img] 未知图片格式 %s\n", path.c_str());
    heap_caps_free(buf);
    return rc;
}

// ---- 缩略图 ----
static String thumb_path(const String& img_path) { return img_path + ".thb"; }

// 生成缩略图：全尺寸解码一次到小画布，RGB565 位图落 SD（4 字节 w/h 头 + w*h*2 字节）
static bool make_thumb(const String& img_path) {
    if (!storage_sd_ok()) return false;
    int w0 = 0, h0 = 0;
    if (!img_render::size(img_path, w0, h0)) { Serial.println("[thb] size 解析失败"); return false; }
    int tw, th;
    fit_box(w0, h0, THUMB_W, THUMB_H, tw, th);
    if (tw < 8 || th < 8) { Serial.println("[thb] 图太小"); return false; }
    M5Canvas c(&M5.Display);
    c.setColorDepth(16);
    if (!c.createSprite(tw, th)) { Serial.println("[thb] createSprite 失败"); return false; }
    c.fillSprite(TFT_WHITE);
    int drawn = draw_full(&c, img_path, 0, 0, tw, th); // 直接走全尺寸版，不走缩略图（防递归）
    if (drawn <= 0) { c.deleteSprite(); Serial.println("[thb] 全尺寸解码失败"); return false; }
    File f = SD.open(thumb_path(img_path).c_str(), "w");
    if (!f) { c.deleteSprite(); Serial.println("[thb] 写打开失败"); return false; }
    uint8_t hdr[4] = { (uint8_t)(tw & 0xFF), (uint8_t)(tw >> 8), (uint8_t)(th & 0xFF), (uint8_t)(th >> 8) };
    f.write(hdr, 4);
    size_t want = (size_t)tw * th * 2;
    size_t wn = f.write((const uint8_t*)c.getBuffer(), want);
    f.close();
    c.deleteSprite();
    if (wn != want) { SD.remove(thumb_path(img_path).c_str()); Serial.println("[thb] 写不完整"); return false; }
    return true;
}

// 缩略图直绘：位图读进 PSRAM 后原位下采样、单次 pushImage 块搬
static int draw_thumb(M5Canvas* cv, const String& img_path, int x, int y, int max_w, int max_h) {
    String tp = thumb_path(img_path);
    if (!SD.exists(tp.c_str())) return 0; // 先查存在，避免 SD.open 失败刷屏
    File f = SD.open(tp.c_str(), "r");
    if (!f) return 0;
    uint8_t hdr[4];
    if (f.read(hdr, 4) != 4) { f.close(); return 0; }
    int tw = hdr[0] | (hdr[1] << 8), th = hdr[2] | (hdr[3] << 8);
    if (tw <= 0 || th <= 0 || tw > THUMB_W || th > THUMB_H) { f.close(); return 0; }
    size_t want = (size_t)tw * th * 2;
    uint16_t* buf = (uint16_t*)heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return 0; }
    size_t got = f.read((uint8_t*)buf, want);
    f.close();
    if (got != want) { heap_caps_free(buf); return 0; }
    int dw, dh;
    fit_box(tw, th, max_w, max_h, dw, dh); // 缩略图已按比例，通常 1:1 或略缩
    int ox = x + (max_w - dw) / 2, oy = y + (max_h - dh) / 2;
    if (dw != tw || dh != th) {
        // 原位下采样（读指针始终领先写指针，安全覆盖），避免逐行 pushImage 的调用开销
        for (int yy = 0; yy < dh; yy++) {
            const uint16_t* src = buf + (int)((int32_t)yy * th / dh) * tw;
            uint16_t* dst = buf + yy * dw;
            for (int xx = 0; xx < dw; xx++) dst[xx] = src[(int)((int32_t)xx * tw / dw)];
        }
    }
    cv->pushImage(ox, oy, dw, dh, buf); // 单次整块搬（memcpy 级）
    heap_caps_free(buf);
    return dh;
}

namespace img_render {

int draw(M5Canvas* cv, const String& path, int x, int y, int max_w, int max_h) {
    if (!cv || max_w <= 0 || max_h <= 0) return 0;
    DRAW_LOCK();
    int rc;
    // 小目标优先缩略图快路径（书架一页 5 张，全尺寸解码 ~0.7s/张扛不住）；
    // 无缩略图（老缓存/下载时生成失败）现场补生成一次，之后都走快路径
    if (max_w <= THUMB_W && max_h <= THUMB_H) {
        rc = draw_thumb(cv, path, x, y, max_w, max_h);
        if (rc > 0) { DRAW_UNLOCK(); return rc; }
        if (make_thumb(path)) {
            rc = draw_thumb(cv, path, x, y, max_w, max_h);
            if (rc > 0) { DRAW_UNLOCK(); return rc; }
        }
    }
    rc = draw_full(cv, path, x, y, max_w, max_h);
    DRAW_UNLOCK();
    return rc;
}

void ensure_thumb(const String& path) {
    if (!storage_sd_ok() || path.isEmpty()) return;
    if (SD.exists(thumb_path(path).c_str())) return;
    DRAW_LOCK();
    make_thumb(path);
    DRAW_UNLOCK();
}

} // namespace img_render
