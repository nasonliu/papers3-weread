// weread_api.cpp — 实现
#include "weread_api.h"
#include "weread_client.h"
#include "weread_crypto.h"
#include "config.h"
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <unzipLIB.h>          // EPUB(zip) 本地解包（platformio.ini lib_deps）
#include <esp_http_client.h>   // EPUB 整本下载（WR 的 512KB 接收上限不够）
#include <esp_crt_bundle.h>

// PSRAM 分配器：大 JSON 树放 PSRAM(8MB)，不占 DRAM(320KB)
struct PsramAllocator {
    void* allocate(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
using PsramJsonDocument = BasicJsonDocument<PsramAllocator>;

namespace weread_api {

// ---- 书架 ----
// 正确接口：GET https://weread.qq.com/web/shelf/sync（cookie 明文 JSON）
// books[] 含 bookId/title/author/cover/format
bool getBookshelf(std::vector<BookEntry>& out, String& err) {
    out.clear();
    HttpResponse r = WR.get(String(WEREAD_HOST_WEB) + "/web/shelf/sync");
    if (!r.ok()) { err = "shelf/sync HTTP " + String(r.status) + " " + r.error; return false; }

    // 调试：打印实际接收长度（PSRAM）、开头、结尾（确认 JSON 完整闭合）
    Serial.printf("[shelf] 接收 len=%d (psram=%s) head=%.120s\n",
                  (int)r.length(), r.psbody ? "是" : "否", r.data());
    if (r.length() > 100) {
        Serial.printf("[shelf] tail=%.100s\n", r.data() + r.length() - 100);
    }

    // 用 PSRAM JsonDocument + filter 只留需要字段
    JsonDocument filter;
    filter["books"][0]["bookId"] = true;
    filter["books"][0]["title"] = true;
    filter["books"][0]["author"] = true;
    filter["books"][0]["cover"] = true;
    // 阅读进度（bookProgress[] 与 books[] 并列，按 bookId 关联）
    filter["bookProgress"][0]["bookId"] = true;
    filter["bookProgress"][0]["progress"] = true;
    filter["bookProgress"][0]["chapterUid"] = true;
    filter["bookProgress"][0]["updateTime"] = true;
    filter["errcode"] = true;   // -2012 检查用；服务器两种字段名都有
    filter["errCode"] = true;

    PsramJsonDocument doc(64 * 1024); // PSRAM 分配 64KB（filter 后 115 本书足够）
    DeserializationError derr = deserializeJson(doc, r.data(), r.length(),
                                                DeserializationOption::Filter(filter));
    if (derr) { err = String("shelf JSON 解析失败: ") + derr.c_str(); return false; }
    Serial.printf("[shelf] 解析成功 已用内存=%d\n", doc.memoryUsage());

    // -2012 登录超时检查（注意服务器两种字段名都有：errcode / errCode）
    int ec = 0;
    if (!doc["errcode"].isNull()) ec = doc["errcode"].as<int>();
    else if (!doc["errCode"].isNull()) ec = doc["errCode"].as<int>();
    if (ec == -2012) { err = "cookie 过期(-2012)"; return false; }

    JsonArray books = doc["books"].as<JsonArray>();
    Serial.printf("[shelf] books 数组大小=%d 已用内存=%d\n", books.size(), doc.memoryUsage());
    for (JsonObject b : books) {
        BookEntry be;
        be.bookId = b["bookId"].as<String>();
        be.title  = b["title"].as<String>();
        be.author = b["author"].as<String>();
        be.cover  = b["cover"].as<String>();
        if (be.bookId.length()) out.push_back(be);
    }
    if (out.empty()) { err = "书架为空或解析无 books"; return false; }

    // 合并 bookProgress[] 的阅读进度（实测 chapterUid 为数字，兼容字符串）
    for (JsonObject p : doc["bookProgress"].as<JsonArray>()) {
        String bid = p["bookId"].as<String>();
        if (!bid.length()) continue;
        for (auto& be : out) {
            if (be.bookId != bid) continue;
            be.progress = p["progress"] | 0;
            if (p["chapterUid"].is<const char*>()) be.progressChapterUid = p["chapterUid"].as<String>();
            else be.progressChapterUid = String(p["chapterUid"].as<long>());
            be.readUpdateTime = p["updateTime"].as<uint32_t>();
            break;
        }
    }
    return true;
}

// ---- 书籍信息 ----
bool getBookInfo(const String& bookId, BookEntry& out, String& err) {
    HttpResponse r = WR.get(String(WEREAD_HOST_I) + "/book/info?bookId=" + bookId);
    if (!r.ok()) { err = "info HTTP " + String(r.status); return false; }
    JsonDocument doc;
    if (deserializeJson(doc, r.data(), r.length())) { err = "info JSON 解析失败"; return false; }
    out.bookId = bookId;
    out.title  = doc["title"].as<String>();
    out.author = doc["author"].as<String>();
    out.cover  = doc["cover"].as<String>();
    return true;
}

// ---- 目录 ----
bool getChapterInfos(const String& bookId, std::vector<ChapterEntry>& out, String& format_out, String& err) {
    out.clear();
    String body = "{\"bookIds\":[\"" + bookId + "\"]}";
    HttpResponse r = WR.postJson(String(WEREAD_HOST_WEB) + "/web/book/chapterInfos", body);
    if (!r.ok()) { err = "chapterInfos HTTP " + String(r.status); return false; }

    // 大书章节多 JSON 可达数百 KB，放 PSRAM；数据在 r.data()/r.length()（非 r.body）
    PsramJsonDocument doc(64 * 1024);
    if (deserializeJson(doc, r.data(), r.length())) { err = "chapterInfos JSON 解析失败"; return false; }

    // 兼容 data[] / 顶层
    JsonArray data;
    if (doc["data"].is<JsonArray>()) data = doc["data"].as<JsonArray>();
    else { err = "chapterInfos 无 data[]"; return false; }

    for (JsonObject item : data) {
        String bid = item["bookId"].as<String>();
        if (bid != bookId && item["book"]["bookId"].as<String>() != bookId) continue;
        format_out = item["format"].as<String>(); // 可能为空（txt）
        JsonArray updated = item["updated"].as<JsonArray>();
        if (updated.isNull()) updated = item["chapterInfos"].as<JsonArray>();
        for (JsonObject ch : updated) {
            ChapterEntry ce;
            ce.chapterUid = ch["chapterUid"].as<String>();
            ce.title      = ch["title"].as<String>();
            ce.chapterIdx = ch["chapterIdx"] | 0;
            ce.wordCount  = ch["wordCount"] | 0;
            ce.paid       = ch["paid"] | false;
            ce.tar        = ch["tar"].as<String>(); // 资源 tar 包（插图等），可能为空
            if (ce.chapterUid.length()) out.push_back(ce);
        }
        return true;
    }
    err = "chapterInfos 未找到 bookId=" + bookId;
    return false;
}

// ---- psvts 提取（字符串搜索，不需 DOM）----
String extractPsvts(const char* html, size_t len) {
    // window.__INITIAL_STATE__ = {...}; 中找 "psvts":"..."
    // 直接在缓冲上扫描（PSRAM 大 HTML 不复制成 String，要求 html 有 \0 结尾）
    if (!html || !len) return "";
    const char* idx = strstr(html, "\"psvts\"");
    if (!idx) return "";
    const char* colon = strchr(idx, ':');
    if (!colon) return "";
    const char* q1 = strchr(colon, '"');
    if (!q1) return "";
    const char* q2 = strchr(q1 + 1, '"');
    if (!q2) return "";
    return String(q1 + 1, (unsigned int)(q2 - q1 - 1));
}

String extractPsvts(const String& html) {
    return extractPsvts(html.c_str(), html.length());
}

// reader URL
static String reader_url(const String& bookId, const String& chapterUid) {
    String u = String(WEREAD_HOST_WEB) + "/web/reader/" + weread::e(bookId);
    if (chapterUid.length()) u += "k" + weread::e(chapterUid);
    return u;
}

// 从 params 列表构造 JSON body
static String params_to_json(const std::vector<std::pair<String,String>>& p) {
    String j = "{";
    bool first = true;
    for (auto& kv : p) {
        if (!first) j += ",";
        // 数字字段不加引号：ct, sc, st, r
        bool numeric = (kv.first == "ct" || kv.first == "sc" || kv.first == "st" || kv.first == "r");
        j += "\"" + kv.first + "\":";
        if (numeric) j += kv.second;
        else { j += "\""; j += kv.second; j += "\""; }
        first = false;
    }
    j += "}";
    return j;
}

// ---- XHTML 去标签（指针版，直接解析 PSRAM 大数据，避开 String 64KB 损坏 bug）----

// 文本块最大字节数：ContentBlock.text 是 Arduino String，超 ~64KB 会损坏，按 48KB 留余量
static const size_t TEXT_BLOCK_MAX = 48 * 1024;

// 在 [data,len) 内从 from 起找子串，返回下标或 -1（限长 memcmp 搜索，不要求 \0 结尾）
static int find_sub(const char* data, size_t len, size_t from, const char* needle, size_t nlen) {
    if (!nlen || from + nlen > len) return -1;
    for (size_t i = from; i + nlen <= len; i++)
        if (memcmp(data + i, needle, nlen) == 0) return (int)i;
    return -1;
}

// UTF-8 安全切点：p[end] 若落在多字节字符中间（10xxxxxx），回退到字符边界
static size_t utf8_safe_cut(const char* p, size_t end) {
    while (end > 0 && (p[end] & 0xC0) == 0x80) end--;
    return end;
}

// 去标签并流式产出文本块：逐字符解析 [p,n)，文本累积到 t；
// t 达 48KB 就在最近换行/UTF-8 边界切出一块交给 emit（保证单个 String 永不超 48KB）。
// 解析规则与原 String 版 strip_xhtml 一致：段落/换行标签转换行、常见 HTML 实体展开、连续空行折叠、各块 trim。
template <typename Emit>
static void strip_and_emit(const char* p, size_t n, Emit emit) {
    String t;
    int nl = 0;        // 连续换行计数（空行折叠）
    bool in_tag = false;
    // 追加一个字符（带空行折叠）
    auto putc = [&](char c) {
        if (c == '\n') { nl++; if (nl <= 1) t += '\n'; }
        else { nl = 0; t += c; }
    };
    // 切出一块：优先最近换行处，否则 48KB 硬切（UTF-8 边界对齐），trim 后非空才产出
    auto flush = [&]() {
        size_t cut = t.length();
        int br = t.lastIndexOf('\n');
        if (br > (int)(TEXT_BLOCK_MAX / 2)) cut = br + 1;
        else cut = utf8_safe_cut(t.c_str(), TEXT_BLOCK_MAX);
        if (!cut) cut = 1; // 防御：保证前进不死循环
        String piece = t.substring(0, cut);
        piece.trim();
        if (piece.length()) emit(piece);
        t = t.substring(cut);
        nl = 0;
    };
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '<') {
            // 段落/换行标签转换为换行（规则同原 strip_xhtml：<p <br <h1 </p> </h1>）
            size_t r = n - i;
            if ((r >= 2 && p[i+1] == 'p') ||
                (r >= 3 && p[i+1] == 'b' && p[i+2] == 'r') ||
                (r >= 3 && p[i+1] == 'h' && p[i+2] == '1') ||
                (r >= 4 && p[i+1] == '/' && p[i+2] == 'p' && p[i+3] == '>') ||
                (r >= 5 && p[i+1] == '/' && p[i+2] == 'h' && p[i+3] == '1' && p[i+4] == '>')) {
                putc('\n');
            }
            in_tag = true;
            continue;
        }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) {
            // HTML 实体（常见几个；窗口 8 字节内找 ';'）
            if (c == '&') {
                int semi = find_sub(p, n, i, ";", 1);
                if (semi > 0 && semi - (int)i < 8) {
                    String ent(p + i, (unsigned int)(semi - i + 1));
                    if (ent == "&amp;") putc('&');
                    else if (ent == "&lt;") putc('<');
                    else if (ent == "&gt;") putc('>');
                    else if (ent == "&quot;") putc('"');
                    else if (ent == "&nbsp;") putc(' ');
                    else if (ent == "&apos;") putc('\'');
                    else { for (size_t k = i; k <= (size_t)semi; k++) putc(p[k]); } // 未知实体原样保留
                    i = semi;
                    if (t.length() >= TEXT_BLOCK_MAX) flush();
                    continue;
                }
            }
            putc(c);
        }
        if (t.length() >= TEXT_BLOCK_MAX) flush();
    }
    t.trim();
    if (t.length()) emit(t); // 尾部
}

// 指针版 strip_xhtml（getChapterText 用）
// 注意：返回 String，结果超 ~64KB 的章会触发 ESP32 String bug——大章请用 getChapterBlocks
static String strip_xhtml(const char* p, size_t n) {
    String out;
    strip_and_emit(p, n, [&](const String& piece) {
        if (out.length()) out += '\n'; // 切块边界补换行
        out += piece;
    });
    out.trim();
    return out;
}

// ---- 章节原始内容下载（reader HTML → psvts → 分片解码）----
// getChapterText / getChapterBlocks 共用

// 章节内容形态
enum ChapterKind {
    CHAPTER_TXT,       // TXT 纯文本（data）
    CHAPTER_XHTML,     // 常规 EPUB 书：分片解码出的单章 XHTML（data）
    CHAPTER_EPUB_ZIP,  // 少数 EPUB 书：正文接口直接下发整本 EPUB zip（data）
};

// 章节原始内容：PSRAM 持有（Arduino String 超 ~64KB 会损坏，大章必须避开）
struct ChapterRaw {
    ChapterKind kind = CHAPTER_XHTML;
    char* data = nullptr;  // PSRAM 缓冲（heap_caps_malloc，已补 \0），用毕调 release()
    size_t len = 0;
    void release() { if (data) { heap_caps_free(data); data = nullptr; len = 0; } }
};

// 前向声明：EPUB 解包取章节 XHTML（实现在下文"章节图文分块"区）
// xhtml_out 为 PSRAM 缓冲（所有权转给调用方，负责 heap_caps_free）
static bool epub_extract_chapter_xhtml(const uint8_t* zip_data, size_t zip_len,
                                       int chapterIdx, char*& xhtml_out, size_t& xhtml_len_out,
                                       String& epub_dir_out, String& err);

// EPUB 整本下载：独立 esp_http_client + 3MB PSRAM 缓冲
// （WereadClient 的接收上限是 512KB，MB 级 zip 会被截断，故在此自实现）
// 成功返回 PSRAM 缓冲（调用方负责 heap_caps_free），失败返回 nullptr
static uint8_t* download_epub_post(const String& url, const String& body, const String& referer,
                                   size_t& out_len, String& err) {
    const size_t CAP = 3 * 1024 * 1024; // EPUB 一般 1-3MB
    uint8_t* buf = (uint8_t*)heap_caps_malloc(CAP, MALLOC_CAP_SPIRAM);
    if (!buf) { err = "EPUB 接收缓冲分配失败"; return nullptr; }

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 60000; // 大文件放宽超时
    cfg.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { heap_caps_free(buf); err = "EPUB 下载初始化失败"; return nullptr; }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
    esp_http_client_set_header(client, "Referer", referer.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json;charset=UTF-8");
    String cookie = WR.cookieHeader();
    if (cookie.length()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
    esp_http_client_set_post_field(client, body.c_str(), body.length());

    esp_err_t e = esp_http_client_perform(client);
    int status = e == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    size_t total = 0;
    if (status >= 200 && status < 300) {
        // 流式读满缓冲（chunked 由 esp 层解码）
        for (;;) {
            int r = esp_http_client_read(client, (char*)buf + total, CAP - total);
            if (r <= 0) break;
            total += r;
            if (total >= CAP) break;
        }
    }
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        heap_caps_free(buf);
        err = "EPUB 下载 HTTP " + String(status);
        return nullptr;
    }
    if (total >= CAP) { // zip 超 3MB 缓冲上限，放弃
        heap_caps_free(buf);
        err = "EPUB 超过 3MB 缓冲上限";
        return nullptr;
    }
    Serial.printf("[epub] 整本下载完成 %d 字节\n", (int)total);
    out_len = total;
    return buf;
}

// ---- 正文下载专用 keep-alive 会话 ----
// 一章的 4 次请求（reader HTML + 3 分片，同 host weread.qq.com）共用一条 TLS 连接：
// 握手从 4 次/章降到 1 次/章（独立请求时每请求握手 1-2s，整书下载提速数倍）。
// 与 WereadClient 的普通请求路径完全隔离（书架/详情等仍走 WR，不受影响）。
// 会话跨章静态保留（整书下载全程复用）；perform 失败即丢弃重建重试一次，仍失败则报错（不卡死）。

// 会话响应缓冲（PSRAM；分片单片实测 ~116KB，768KB 留足余量）
struct ShardRespBuf {
    char* psbuf = nullptr;
    size_t pslen = 0;
    static const size_t CAP = 768 * 1024;
};

// esp_http_client 事件回调：响应体累积到 PSRAM 缓冲（超上限丢弃，语义同 WereadClient）
static esp_err_t shard_http_event(esp_http_client_event_t* evt) {
    ShardRespBuf* sr = (ShardRespBuf*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sr->psbuf && evt->data_len > 0) {
        if (sr->pslen + (size_t)evt->data_len <= ShardRespBuf::CAP) {
            memcpy(sr->psbuf + sr->pslen, evt->data, evt->data_len);
            sr->pslen += evt->data_len;
        }
    }
    return ESP_OK;
}

struct ShardSession {
    esp_http_client_handle_t client = nullptr;
    ShardRespBuf rb;
    unsigned long last_use = 0; // 最近使用时间（闲置关闭用：TLS 常驻 ~50KB DRAM，久持会挤爆后续连接）
    unsigned long last_req = 0; // 上次请求发起时间（请求节流用）
    volatile int in_use = 0;    // 在途请求计数（housekeeping 只在 0 时才允许关闭，否则关掉在途句柄必崩）

    // 请求节流：两次请求至少隔 400ms，避免连发触发 weread 风控（实测连发会被 RST）
    void pace() {
        unsigned long now = millis();
        if (last_req && now - last_req < 400) delay(400 - (now - last_req));
        last_req = millis();
    }

    // 建立连接（幂等）；host 固定 weread.qq.com，后续 set_url 换路径复用同一连接
    bool open() {
        if (client) return true;
        esp_http_client_config_t cfg = {};
        cfg.host = "weread.qq.com";
        cfg.path = "/";
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = HTTP_TIMEOUT_MS;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 2048;
        cfg.keep_alive_enable = true;   // 关键：HTTP/1.1 keep-alive
        cfg.event_handler = shard_http_event;
        cfg.user_data = &rb;
        client = esp_http_client_init(&cfg);
        if (!client && rb.psbuf) { heap_caps_free(rb.psbuf); rb.psbuf = nullptr; }
        return client != nullptr;
    }

    // 丢弃连接（perform 失败 / 空闲被服务端断开后的恢复手段）
    void close() {
        if (client) { esp_http_client_cleanup(client); client = nullptr; }
    }

    // 单次请求（不做重试）。成功返回 PSRAM 数据（所有权转给调用方，heap_caps_free 释放），
    // 传输失败（perform error）返回 nullptr 且 status=-1；HTTP 非 2xx 仍返回数据（上层判断）
    char* request(bool post, const String& path, const String* body, const String& referer,
                  size_t& out_len, int& status) {
        out_len = 0;
        status = -1;
        struct UseGuard { // RAII：在途计数，housekeeping 据此不关句柄
            volatile int& c; UseGuard(volatile int& c_) : c(c_) { c++; } ~UseGuard() { c--; }
        } guard(in_use);
        pace(); // 节流：请求间隔 ≥400ms（防风控）
        if (!open()) return nullptr;
        if (!rb.psbuf) {
            rb.psbuf = (char*)heap_caps_malloc(ShardRespBuf::CAP, MALLOC_CAP_SPIRAM);
            if (!rb.psbuf) return nullptr;
        }
        rb.pslen = 0;
        esp_http_client_set_url(client, path.c_str());
        esp_http_client_set_method(client, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        // headers 每次请求重设（Referer 每章不同；cookie 可能被 renewal 轮换）
        esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
        esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
        esp_http_client_set_header(client, "Referer", referer.c_str());
        String cookie = WR.cookieHeader();
        if (cookie.length()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
        if (post) {
            esp_http_client_set_header(client, "Content-Type", "application/json;charset=UTF-8");
            esp_http_client_set_post_field(client, body->c_str(), (int)body->length());
        } else {
            esp_http_client_set_post_field(client, nullptr, 0); // 清掉上次 POST body
        }
        esp_err_t e = esp_http_client_perform(client);
        if (e != ESP_OK) return nullptr; // status 保持 -1：连接级失败
        status = esp_http_client_get_status_code(client);
        last_use = millis();
        // 转移数据所有权（缓冲已补 \0 便于字符串扫描）
        char* out = rb.psbuf;
        rb.psbuf = nullptr;
        out_len = rb.pslen;
        if (out_len < ShardRespBuf::CAP) out[out_len] = 0;
        return out;
    }

    // 带一次重建重试的请求：perform 失败（连接被断开等）→ 丢弃旧连接重建再试一次
    char* request_retry(bool post, const String& path, const String* body, const String& referer,
                        size_t& out_len, int& status) {
        char* d = request(post, path, body, referer, out_len, status);
        if (d) return d;
        close(); // 连接已坏，丢弃重建（不卡死：最多两次尝试）
        Serial.println("[shard] 连接失败，丢弃重建重试");
        return request(post, path, body, referer, out_len, status);
    }
};

// 跨章静态会话：整书下载全程复用一条 TLS 连接（空闲被服务端断开后由 request_retry 自动重建）
static ShardSession g_shard_sess;

// 会话闲置关闭（主循环周期调用）：TLS 连接常驻 ~50KB DRAM，超过 30s 不用就关掉，
// 否则两三条 keep-alive 长连接会把后续请求挤出内存（mbedtls_ssl_setup -0x7F00）。
// 有在途请求时绝不关（关掉在途句柄 = 崩溃，实测：伊朗五百年预取循环重启的根因）
void housekeeping() {
    if (g_shard_sess.client && g_shard_sess.in_use == 0 && millis() - g_shard_sess.last_use > 30000) {
        Serial.println("[shard] 会话闲置 30s，关闭释放内存");
        g_shard_sess.close();
    }
}

// psvts 缓存（按 bookId）：整书下载时避免每章都拉 reader HTML。
// 跨章复用的协议兼容性未经实测（cookie 过期未能验证），采用"缓存 + 失败自动重取"自适应策略：
// e_0 请求失败/返回 {} 时清缓存重取重试一次，最坏退化为原行为（多一次请求），不会更坏。
static String g_psvts_book;  // 缓存对应的 bookId
static String g_psvts;       // 缓存的 psvts

// 章节拉取进度回调（main.cpp 注入显示进度），默认为空不调用
void (*chapter_stage_cb)(const char* stage, int cur, int total) = nullptr;
static inline void stage_tick(const char* s, int c, int t) { if (chapter_stage_cb) chapter_stage_cb(s, c, t); }

// 成功返回 true：raw.kind 指示形态，raw.data/len 为 PSRAM 中的内容（TXT 纯文本 /
// 单章 XHTML / 整本 EPUB zip），解码全程 PSRAM（避开 String 64KB 损坏）。
// 本章所有请求（reader HTML + 分片）走同一条 keep-alive 连接（见 ShardSession 注释）
static bool fetch_chapter_raw(const String& bookId, const ChapterEntry& ch, ChapterRaw& raw, String& err) {
    String rurl = reader_url(bookId, ch.chapterUid); // 完整 URL，作 Referer
    // 会话内用相对路径（host 固定 weread.qq.com）
    String rpath = "/web/reader/" + weread::e(bookId);
    if (ch.chapterUid.length()) rpath += "k" + weread::e(ch.chapterUid);
    int status = 0;

    // 1. psvts：按 bookId 缓存（整书下载只拉一次 reader HTML）
    String psvts;
    bool from_cache = (g_psvts_book == bookId && g_psvts.length());
    if (from_cache) psvts = g_psvts;
    if (!psvts.length()) {
        stage_tick("reader", 1, 4);
        size_t hlen = 0;
        char* html = g_shard_sess.request_retry(false, rpath, nullptr, rurl, hlen, status);
        if (!html) { err = "reader 请求失败"; return false; }
        if (status < 200 || status >= 300) { heap_caps_free(html); err = "reader HTTP " + String(status); return false; }
        psvts = extractPsvts(html, hlen);
        heap_caps_free(html);
        if (!psvts.length()) { err = "未提取到 psvts"; return false; }
        g_psvts_book = bookId;
        g_psvts = psvts;
    }

    // 2. e_0（同一连接；缓存 psvts 失效时自动重取并重试一次）
    auto shard_body = [&]() -> String {
        auto params = weread::make_content_params(bookId, ch.chapterUid, psvts, false, 1);
        return params_to_json(params);
    };
    String e0_body = shard_body();
    const String e0_path = "/web/book/chapter/e_0";
    char* d0 = nullptr;
    size_t n0 = 0;
    auto post_e0 = [&]() {
        stage_tick("e_0", 2, 4);
        d0 = g_shard_sess.request_retry(true, e0_path, &e0_body, rurl, n0, status);
    };
    post_e0();
    if (!d0) { err = "e_0 请求失败"; return false; }
    // 缓存 psvts 失效的典型表现：HTTP 错误或返回 {}
    auto e0_failed = [&]() {
        return status < 200 || status >= 300 || (n0 == 2 && d0[0] == '{' && d0[1] == '}');
    };
    if (e0_failed() && from_cache) {
        heap_caps_free(d0);
        d0 = nullptr;
        g_psvts = "";
        g_psvts_book = "";
        Serial.println("[shard] 缓存 psvts 失效，重取重试");
        size_t hlen = 0;
        char* html = g_shard_sess.request_retry(false, rpath, nullptr, rurl, hlen, status);
        if (!html || status < 200 || status >= 300) {
            if (html) heap_caps_free(html);
            err = "reader 重取失败 HTTP " + String(status);
            return false;
        }
        psvts = extractPsvts(html, hlen);
        heap_caps_free(html);
        if (!psvts.length()) { err = "重取 psvts 失败"; return false; }
        g_psvts_book = bookId;
        g_psvts = psvts;
        e0_body = shard_body(); // psvts 已变，重新签名
        post_e0();
        if (!d0) { err = "e_0 重试失败"; return false; }
    }
    if (e0_failed()) {
        err = "e_0 HTTP " + String(status);
        heap_caps_free(d0);
        return false;
    }

    // 形态 B：少数 EPUB 书直接下发整本 EPUB zip（明文 PK\x03\x04 开头，非加密分片）
    if (n0 >= 4 && d0[0] == 'P' && d0[1] == 'K' && d0[2] == 3 && d0[3] == 4) {
        // 会话缓冲上限 768KB 可能截断，用独立大缓冲重下完整 zip
        heap_caps_free(d0);
        raw.data = (char*)download_epub_post(String(WEREAD_HOST_WEB) + e0_path, e0_body, rurl, raw.len, err);
        if (!raw.data) return false;
        raw.kind = CHAPTER_EPUB_ZIP;
        return true;
    }

    // 形态 A：返回 JSON 说明是 TXT 书（e_0 对 TXT 书回章节元信息）
    // 限长扫描 "bookId"（响应填满缓冲时无 \0 结尾，不能用 strstr）
    bool has_bookid = false;
    if (n0 >= 8) {
        for (size_t i = 0; i + 8 <= n0; i++)
            if (memcmp(d0 + i, "\"bookId\"", 8) == 0) { has_bookid = true; break; }
    }
    bool is_txt = (n0 && d0[0] == '{' && has_bookid) || n0 == 0;

    // 分片 POST（同一连接；shard_body() 每次生成新签名 ts/rn，与原行为一致）
    auto post_shard = [&](const char* path, char*& out, size_t& out_len) {
        String body = shard_body(); // 命名变量（临时量不能取地址）
        out = g_shard_sess.request_retry(true, String(path), &body, rurl, out_len, status);
    };

    if (is_txt) {
        char* t0 = nullptr; size_t tn0 = 0;
        char* t1 = nullptr; size_t tn1 = 0;
        stage_tick("t_0", 3, 4);
        post_shard("/web/book/chapter/t_0", t0, tn0);
        stage_tick("t_1", 4, 4);
        post_shard("/web/book/chapter/t_1", t1, tn1);
        heap_caps_free(d0); // e_0 元信息已无用（t 分支不再需要）
        d0 = nullptr;
        if (tn0 == 2 && t0[0] == '{' && t0[1] == '}') { // tn0==2 保证 t0 非空
            heap_caps_free(t0);
            if (t1) heap_caps_free(t1);
            err = "t_0 返回 {}（可能无权限/风控）";
            return false;
        }
        raw.data = weread::decode_content_shards_psram(t0, tn0, t1, tn1, nullptr, 0, &raw.len);
        if (t0) heap_caps_free(t0);
        if (t1) heap_caps_free(t1);
        if (!raw.data) { err = "TXT 解码失败"; return false; }
        raw.kind = CHAPTER_TXT;
        return true;
    }

    // 形态 C：常规 EPUB 分片 e_0+e_1+e_3 → 单章 XHTML（全 PSRAM，e_0 响应零复制复用）
    char* e1 = nullptr; size_t en1 = 0;
    char* e3 = nullptr; size_t en3 = 0;
    stage_tick("e_1", 3, 4);
    post_shard("/web/book/chapter/e_1", e1, en1);
    stage_tick("e_3", 4, 4);
    post_shard("/web/book/chapter/e_3", e3, en3);
    raw.data = weread::decode_content_shards_psram(d0, n0, e1, en1, e3, en3, &raw.len);
    heap_caps_free(d0);
    if (e1) heap_caps_free(e1);
    if (e3) heap_caps_free(e3);
    if (!raw.data) { err = "EPUB 解码失败"; return false; }
    // 防御：解码后才是 zip 的情况（未见实例，同样走解包）
    if (raw.len >= 4 && raw.data[0] == 'P' && raw.data[1] == 'K' && raw.data[2] == 3 && raw.data[3] == 4)
        raw.kind = CHAPTER_EPUB_ZIP;
    else
        raw.kind = CHAPTER_XHTML;
    return true;
}

// ---- 拉取单章纯文本 ----
// 注意：text_out 是 Arduino String，正文超 ~64KB 的章会触发 ESP32 String 损坏 bug
// ——大章请改用 getChapterBlocks（其文本块按 ≤48KB 切分，不受影响）
bool getChapterText(const String& bookId, const ChapterEntry& ch, String& text_out, String& err) {
    ChapterRaw raw;
    if (!fetch_chapter_raw(bookId, ch, raw, err)) return false;
    if (raw.kind == CHAPTER_EPUB_ZIP) {
        // 整本 EPUB：解包取本章 XHTML 再去标签
        char* xhtml = nullptr;
        size_t xlen = 0;
        String edir;
        bool ok = epub_extract_chapter_xhtml((const uint8_t*)raw.data, raw.len,
                                             ch.chapterIdx, xhtml, xlen, edir, err);
        raw.release();
        if (!ok) return false;
        text_out = strip_xhtml(xhtml, xlen);
        heap_caps_free(xhtml);
        return true;
    }
    if (raw.kind == CHAPTER_TXT) text_out = String(raw.data, (unsigned int)raw.len); // txt 直出
    else text_out = strip_xhtml(raw.data, raw.len); // epub 去标签
    raw.release();
    return true;
}

// ---- 上传阅读进度 ----
// 实测：POST weread.qq.com/web/book/read（cookie 明文 + web 签名 payload）返回 {"succ":1}；
// i.weread.qq.com 同类接口需额外签名返回 401，不可用。
// payload 字段与 weread.koplugin lib/weread.lua make_read_payload 一致：
// appId/b/c/ci/co/sm/pr/rt/ts/rn/sg/ct/ps/pc，再对排序 query 签出 s

// 进度 payload 的固定 reader token（同 koplugin DEFAULT_READER_TOKEN）
static const char* READER_TOKEN = "3c5c8717f3daf09iop3423zafeqoi";

// SHA256 hex（小写 64 字符），payload 的 sg 字段用
static String sha256_hex(const String& s) {
    uint8_t out[32];
    mbedtls_sha256((const uint8_t*)s.c_str(), s.length(), out, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[64] = 0;
    return String(hex);
}

// 由 UA 推导 appId（移植 koplugin web_app_id：UA 前 12 个词长 %10 作前缀 + UA 哈希）
static String web_app_id() {
    String ua = WEREAD_USER_AGENT;
    String prefix;
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= ua.length() && count < 12; i++) {
        if (i == ua.length() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) { prefix += String((i - start) % 10); count++; }
            start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (size_t i = 0; i < ua.length(); i++)
        hash = (uint32_t)((0x83ULL * hash + (uint8_t)ua[i]) & 0x7fffffff);
    return "wb" + prefix + "h" + String(hash);
}

bool uploadProgress(const String& bookId, const String& chapterUid, int chapterIdx, int progressPercent, String& err) {
    // 1. 拿 reader HTML 提取 psvts（payload 的 ps 字段，与正文请求同源）
    String rurl = reader_url(bookId, chapterUid);
    HttpResponse rh = WR.get(rurl, rurl);
    if (!rh.ok()) { err = "reader HTTP " + String(rh.status); return false; }
    String psvts = extractPsvts(rh.data(), rh.length());
    if (!psvts.length()) { err = "未提取到 psvts"; return false; }

    // 2. 构造签名 payload
    long ct = (long)time(nullptr);
    char ts_buf[24]; // 毫秒时间戳 = 秒*1000 + 随机毫秒（同 Lua）
    snprintf(ts_buf, sizeof(ts_buf), "%llu", (unsigned long long)ct * 1000 + random(0, 1000));
    String ts = ts_buf;
    String rn = String(random(0, 1000));

    std::vector<std::pair<String,String>> p;
    p.push_back({"appId", web_app_id()});
    p.push_back({"b", weread::e(bookId)});
    p.push_back({"c", weread::e(chapterUid)});
    p.push_back({"ci", String(chapterIdx)});
    p.push_back({"co", "0"});                   // 章节内偏移，按章节粒度同步置 0
    p.push_back({"sm", ""});                    // 章节摘要，可空
    p.push_back({"pr", String(progressPercent)});
    p.push_back({"rt", "0"});                   // 本次阅读时长秒，暂不统计
    p.push_back({"ts", ts});
    p.push_back({"rn", rn});
    p.push_back({"sg", sha256_hex(ts + rn + READER_TOKEN)});
    p.push_back({"ct", String(ct)});
    p.push_back({"ps", psvts});
    p.push_back({"pc", weread::e(String(ct))});
    String q;
    weread::sorted_query(p, q);
    p.push_back({"s", weread::sign(q)});

    // 3. 序列化 JSON body（ci/co/pr/rt/ts/rn/ct 为数值，其余字符串）
    auto is_num = [](const String& k) {
        return k == "ci" || k == "co" || k == "pr" || k == "rt" || k == "ts" || k == "rn" || k == "ct";
    };
    String body = "{";
    bool first = true;
    for (auto& kv : p) {
        if (!first) body += ",";
        body += "\"" + kv.first + "\":";
        if (is_num(kv.first)) body += kv.second;
        else { body += "\""; body += kv.second; body += "\""; }
        first = false;
    }
    body += "}";

    HttpResponse r = WR.postJson(String(WEREAD_HOST_WEB) + "/web/book/read", body, rurl);
    if (!r.ok()) { err = "read HTTP " + String(r.status); return false; }
    JsonDocument doc;
    if (deserializeJson(doc, r.data(), r.length())) { err = "read JSON 解析失败"; return false; }
    if ((doc["succ"] | 0) != 1) {
        err = "进度上传被拒: " + String(r.data(), (unsigned int)min(r.length(), (size_t)120));
        return false;
    }
    return true;
}

// ---- 书籍详情 ----
// 实测：web/book/info 返回 intro/publisher/isbn/category 等（cookie 明文）；
// i.weread/book/info 需签名返回 401；web 接口无作者简介字段，authorIntro 恒空
bool getBookDetail(const String& bookId, BookDetail& out, String& err) {
    HttpResponse r = WR.get(String(WEREAD_HOST_WEB) + "/web/book/info?bookId=" + bookId);
    if (!r.ok()) { err = "book/info HTTP " + String(r.status); return false; }

    // 响应键多达 90+，filter 只留需要字段
    JsonDocument filter;
    filter["title"] = true;
    filter["author"] = true;
    filter["intro"] = true;
    filter["cover"] = true;
    filter["publisher"] = true;
    filter["errcode"] = true; // -2012 检查用
    filter["errCode"] = true;

    PsramJsonDocument doc(16 * 1024);
    DeserializationError derr = deserializeJson(doc, r.data(), r.length(),
                                                DeserializationOption::Filter(filter));
    if (derr) { err = String("book/info JSON 解析失败: ") + derr.c_str(); return false; }
    int ec = 0;
    if (!doc["errcode"].isNull()) ec = doc["errcode"].as<int>();
    else if (!doc["errCode"].isNull()) ec = doc["errCode"].as<int>();
    if (ec == -2012) { err = "cookie 过期(-2012)"; return false; }
    if (doc["title"].isNull()) { err = "book/info 无 title（bookId 无效?）"; return false; }

    out.title       = doc["title"].as<String>();
    out.author      = doc["author"].as<String>();
    out.intro       = doc["intro"].as<String>();
    out.cover       = doc["cover"].as<String>();
    out.publisher   = doc["publisher"].as<String>();
    out.authorIntro = ""; // web/book/info 不返回作者简介
    return true;
}

// ---- 热门评论 ----
// 实测：web/review/list?listType=3 返回本书推荐（热门）长评（cookie 明文）；
// listType=1 是好友动态流（不限本书）、2 是最新、4 返回空；i.weread/review/list 401。
// web 接口不返回点赞数，likes 恒 0

// 取 UTF-8 字符串前 max_chars 个字符（按字符数截，不截断多字节序列）
static String utf8_prefix(const String& s, size_t max_chars) {
    size_t i = 0, n = 0;
    while (i < s.length() && n < max_chars) {
        uint8_t c = (uint8_t)s[i];
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        i += len;
        n++;
    }
    return s.substring(0, i);
}

bool getHotReviews(const String& bookId, std::vector<Review>& out, int count, String& err) {
    out.clear();
    if (count <= 0) count = 3;
    String url = String(WEREAD_HOST_WEB) + "/web/review/list?bookId=" + bookId +
                 "&listType=3&maxIdx=0&count=" + String(count);
    HttpResponse r = WR.get(url);
    if (!r.ok()) { err = "review/list HTTP " + String(r.status); return false; }

    // 只留昵称和正文（author 里还有 avatar 等大字段，过滤掉省内存）
    JsonDocument filter;
    filter["reviews"][0]["review"]["content"] = true;
    filter["reviews"][0]["review"]["author"]["name"] = true;
    filter["errCode"] = true;

    PsramJsonDocument doc(32 * 1024);
    DeserializationError derr = deserializeJson(doc, r.data(), r.length(),
                                                DeserializationOption::Filter(filter));
    if (derr) { err = String("review/list JSON 解析失败: ") + derr.c_str(); return false; }
    int ec = doc["errCode"] | 0; // 成功时无 errCode 键（默认 0）；参数错误时为 -2003
    if (ec != 0) { err = "review/list errCode=" + String(ec); return false; }

    for (JsonObject rv : doc["reviews"].as<JsonArray>()) {
        JsonObject inner = rv["review"].as<JsonObject>();
        if (inner.isNull()) continue;
        Review rev;
        rev.nick    = inner["author"]["name"].as<String>();
        rev.content = utf8_prefix(inner["content"].as<String>(), 200);
        rev.likes   = 0; // web 接口无点赞数
        if (rev.content.length()) out.push_back(rev);
    }
    if (out.empty()) { err = "本书暂无热门评论"; return false; }
    return true;
}

// ---- 章节图文分块 ----

// 图片 URL 归一化为 https 绝对 URL。
// 实测正文插图已是 CDN 绝对 URL（https://res.weread.qq.com/wrepub/...）；
// 少量装饰图是相对路径（如 ../Images/x.jpg），拼 web 域名仅作 best-effort
static String normalize_image_url(const String& src) {
    if (src.startsWith("http://") || src.startsWith("https://")) return src;
    if (src.startsWith("//")) return "https:" + src;
    if (src.startsWith("/")) return String(WEREAD_HOST_WEB) + src;
    return String(WEREAD_HOST_WEB) + "/" + src;
}

// 从 <img ...> 标签 [p,n) 提取 src 属性值（容忍属性顺序、单双引号、标签内杂字节）
static String extract_img_src(const char* p, size_t n) {
    for (size_t i = 0; i + 3 <= n; i++) {
        if (p[i] != 's' || p[i+1] != 'r' || p[i+2] != 'c') continue;
        size_t j = i + 3;
        while (j < n && (p[j] == ' ' || p[j] == '=')) j++;
        if (j >= n) return "";
        char quo = p[j];
        if (quo != '"' && quo != '\'') return "";
        size_t e = j + 1;
        while (e < n && p[e] != quo) e++;
        if (e >= n) return "";
        return String(p + j + 1, (unsigned int)(e - j - 1));
    }
    return "";
}

// ---- EPUB(zip) 本地解包（unzipLIB）----
// 少数 EPUB 书（公版/原版书）正文接口直接下发整本 EPUB：
// 解包 zip → META-INF/container.xml → content.opf → manifest/spine → 本章 XHTML。
// 章节映射：chapterInfos 的 chapterIdx(1 起) 对应 spine 顺序第 chapterIdx-1 项（clamp 防越界）

// zip 内按名读取整个文件到 PSRAM（成功返回指针，调用方负责 heap_caps_free）
static uint8_t* epub_read_file(UNZIP& zip, const char* path, size_t& out_len) {
    if (zip.locateFile(path) != UNZ_OK) return nullptr;
    unz_file_info fi;
    if (zip.getFileInfo(&fi, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK) return nullptr;
    if (fi.uncompressed_size == 0 || fi.uncompressed_size > 2 * 1024 * 1024) return nullptr;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(fi.uncompressed_size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    if (zip.openCurrentFile() != UNZ_OK) { heap_caps_free(buf); return nullptr; }
    size_t got = 0;
    while (got < fi.uncompressed_size) { // readCurrentFile 一次可能读不满，循环到 EOF
        int r = zip.readCurrentFile(buf + got, fi.uncompressed_size - got);
        if (r <= 0) break;
        got += r;
    }
    zip.closeCurrentFile();
    if (got != fi.uncompressed_size) { heap_caps_free(buf); return nullptr; }
    buf[got] = 0;
    out_len = got;
    return buf;
}

// 从单个 XML 标签文本按属性名提取值（双引号；属性顺序不固定，不能按位置猜）
static String xml_attr(const String& tag, const char* name) {
    String key = String(name) + "=\"";
    int p = tag.indexOf(key);
    if (p < 0) return "";
    int v = p + key.length();
    int e = tag.indexOf('"', v);
    if (e < 0) return "";
    return tag.substring(v, e);
}

// %XX 百分号解码（OPF 的 href 是 URI，可能带编码字符）
static String pct_decode(const String& s) {
    String out;
    out.reserve(s.length());
    for (size_t i = 0; i < s.length(); i++) {
        if (s[i] == '%' && i + 2 < s.length()) {
            auto hexv = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexv(s[i + 1]), lo = hexv(s[i + 2]);
            if (hi >= 0 && lo >= 0) { out += (char)(hi * 16 + lo); i += 2; continue; }
        }
        out += s[i];
    }
    return out;
}

// 包内路径解析：base_dir + 相对路径，规范化 "." 和 ".."（不处理绝对路径，EPUB 内都是相对）
static String epub_resolve_path(const String& base_dir, const String& rel) {
    String full = base_dir.length() ? base_dir + "/" + rel : rel;
    std::vector<String> parts;
    int start = 0;
    while (start <= (int)full.length()) {
        int slash = full.indexOf('/', start);
        if (slash < 0) slash = full.length();
        String seg = full.substring(start, slash);
        if (seg == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (seg != "." && seg.length()) parts.push_back(seg);
        start = slash + 1;
    }
    String out;
    for (auto& s : parts) { if (out.length()) out += '/'; out += s; }
    return out;
}

// OPF 解析结果：xhtml manifest（id→href）+ spine 顺序（idref）+ opf 所在目录
struct EpubOpf {
    String opf_dir;
    std::vector<std::pair<String,String>> manifest;
    std::vector<String> spine;
};

// 解析 content.opf（字符串扫描，不上 XML 库）
static bool epub_parse_opf(const String& opf, const String& opf_path, EpubOpf& out) {
    int slash = opf_path.lastIndexOf('/');
    out.opf_dir = slash > 0 ? opf_path.substring(0, slash) : "";

    // manifest：逐 <item ...> 标签，只收 xhtml 类型（注意排除 <itemref）
    int p = 0;
    for (;;) {
        p = opf.indexOf("<item", p);
        while (p >= 0 && opf.substring(p, p + 8) == "<itemref") p = opf.indexOf("<item", p + 8);
        if (p < 0) break;
        // <item 后须跟空白或 '/'，排除 <items 之类的误匹配
        if (p + 5 < (int)opf.length() && opf[p + 5] != ' ' && opf[p + 5] != '\t' &&
            opf[p + 5] != '\r' && opf[p + 5] != '\n' && opf[p + 5] != '/') {
            p = opf.indexOf("<item", p + 5);
            continue;
        }
        int gt = opf.indexOf('>', p);
        if (gt < 0) break;
        String tag = opf.substring(p, gt + 1);
        String id = xml_attr(tag, "id");
        String href = xml_attr(tag, "href");
        String mt = xml_attr(tag, "media-type");
        if (id.length() && href.length() && mt == "application/xhtml+xml")
            out.manifest.push_back({id, href});
        p = gt + 1;
    }

    // spine：逐 <itemref ...> 取 idref 保序
    p = 0;
    for (;;) {
        p = opf.indexOf("<itemref", p);
        if (p < 0) break;
        int gt = opf.indexOf('>', p);
        if (gt < 0) break;
        String idref = xml_attr(opf.substring(p, gt + 1), "idref");
        if (idref.length()) out.spine.push_back(idref);
        p = gt + 1;
    }
    return !out.manifest.empty() && !out.spine.empty();
}

// 从整本 EPUB(zip) 提取指定章节 XHTML（PSRAM 缓冲，所有权转给调用方）；
// epub_dir_out 返回该章包内目录（图片相对路径解析用）
static bool epub_extract_chapter_xhtml(const uint8_t* zip_data, size_t zip_len,
                                       int chapterIdx, char*& xhtml_out, size_t& xhtml_len_out,
                                       String& epub_dir_out, String& err) {
    xhtml_out = nullptr;
    xhtml_len_out = 0;
    static UNZIP zip; // 对象内含 ~40K inflate 缓冲，static 防栈溢出
    if (zip.openZIP((uint8_t*)zip_data, (uint32_t)zip_len) != 0) {
        err = "EPUB zip 打开失败"; return false;
    }
    size_t n = 0;
    uint8_t* container = epub_read_file(zip, "META-INF/container.xml", n);
    if (!container) { zip.closeZIP(); err = "EPUB 缺 META-INF/container.xml"; return false; }
    String cxml((const char*)container, (unsigned int)n);
    heap_caps_free(container);

    // rootfile 指向 content.opf（找不到用常见路径兜底）
    String opf_path;
    int rp = cxml.indexOf("<rootfile");
    if (rp >= 0) {
        int gt = cxml.indexOf('>', rp);
        if (gt > 0) opf_path = xml_attr(cxml.substring(rp, gt + 1), "full-path");
    }
    if (!opf_path.length()) opf_path = "OEBPS/content.opf";

    uint8_t* opf = epub_read_file(zip, opf_path.c_str(), n);
    if (!opf) { zip.closeZIP(); err = "EPUB 缺 " + opf_path; return false; }
    String opfs((const char*)opf, (unsigned int)n);
    heap_caps_free(opf);

    EpubOpf eo;
    if (!epub_parse_opf(opfs, opf_path, eo)) { zip.closeZIP(); err = "EPUB opf 解析失败"; return false; }

    // chapterIdx(1 起) → spine 第 chapterIdx-1 项（clamp）
    int si = chapterIdx - 1;
    if (si < 0) si = 0;
    if (si >= (int)eo.spine.size()) si = (int)eo.spine.size() - 1;
    const String& idref = eo.spine[si];
    String href;
    for (auto& kv : eo.manifest) if (kv.first == idref) { href = kv.second; break; }
    if (!href.length()) { zip.closeZIP(); err = "EPUB spine 项无 manifest: " + idref; return false; }

    String xhtml_path = epub_resolve_path(eo.opf_dir, pct_decode(href));
    uint8_t* x = epub_read_file(zip, xhtml_path.c_str(), n);
    zip.closeZIP();
    if (!x) { err = "EPUB 缺章节文件 " + xhtml_path; return false; }
    // PSRAM 所有权直接转移给调用方（不复制；章节 xhtml 可能超 64KB，不能转 String）
    xhtml_out = (char*)x;
    xhtml_len_out = n;
    int sl = xhtml_path.lastIndexOf('/');
    epub_dir_out = sl > 0 ? xhtml_path.substring(0, sl) : "";
    Serial.printf("[epub] 章节 chapterIdx=%d → %s（spine 共 %d 项）\n",
                  chapterIdx, xhtml_path.c_str(), (int)eo.spine.size());
    return true;
}

// XHTML → 图文块（getChapterBlocks 的切块核心，按文档顺序；直接解析 PSRAM 数据）
// epub_dir 为空：分片章节，相对 img src 拼 https（best-effort）；
// epub_dir 非空：EPUB 包内章节，相对 img src 转 "epubimg:" + 包内绝对路径标记
// <h1>..<h4> 标题文本标 style=1..4（尽量贴近原书样式）
static void xhtml_to_blocks(const char* raw, size_t rawlen, const String& epub_dir, std::vector<ContentBlock>& out) {
    // 文本段去标签后产出文本块（strip_and_emit 内部按 ≤48KB 流式切分，避开 String 64KB bug）
    auto push_text = [&](const char* p, size_t n, uint8_t style) {
        strip_and_emit(p, n, [&](const String& piece) {
            ContentBlock b;
            b.is_image = false;
            b.style = style;
            b.text = piece;
            out.push_back(b);
        });
    };

    size_t pos = 0;
    for (;;) {
        // 下一个 <img（排除 <image 误匹配）
        int im = find_sub(raw, rawlen, pos, "<img", 4);
        while (im >= 0 && im + 4 < (int)rawlen &&
               raw[im + 4] != ' ' && raw[im + 4] != '\t' && raw[im + 4] != '\r' &&
               raw[im + 4] != '\n' && raw[im + 4] != '/') {
            im = find_sub(raw, rawlen, im + 4, "<img", 4);
        }
        // 下一个 <hN> 或 <hN ...>（N=1..4，取最近的）
        int ht = -1; uint8_t lvl = 0;
        for (uint8_t k = 1; k <= 4; k++) {
            char pat[4]  = {'<', 'h', (char)('0' + k), '>'};
            char pat2[4] = {'<', 'h', (char)('0' + k), ' '};
            int t = find_sub(raw, rawlen, pos, pat, 4);
            if (t < 0) t = find_sub(raw, rawlen, pos, pat2, 4);
            if (t >= 0 && (ht < 0 || t < ht)) { ht = t; lvl = k; }
        }
        if (im < 0 && ht < 0) break;
        int next;
        bool is_img;
        if (im >= 0 && (ht < 0 || im < ht)) { next = im; is_img = true; }
        else { next = ht; is_img = false; }
        push_text(raw + pos, (size_t)next - pos, 0); // 标签前的普通文本

        if (is_img) {
            int gt = find_sub(raw, rawlen, im, ">", 1);
            if (gt < 0) break;
            String src = extract_img_src(raw + im, (size_t)gt + 1 - im);
            if (src.length()) {
                ContentBlock b;
                b.is_image = true;
                bool absolute = src.startsWith("http://") || src.startsWith("https://") || src.startsWith("//");
                if (epub_dir.length() && !absolute) {
                    // EPUB 包内图片：相对本章目录解析成包内绝对路径，加标记前缀（提取渲染由上层处理）
                    b.text = "epubimg:" + epub_resolve_path(epub_dir, pct_decode(src));
                    out.push_back(b);
                } else if (!absolute) {
                    // 分片章节的相对路径图（../Images/note.png 等）是 Web 阅读器 UI 装饰图，
                    // 并非书籍内容（tar 资源包里也没有，实测）→ 不出块，直接跳过
                } else {
                    b.text = normalize_image_url(src);
                    out.push_back(b);
                }
            }
            pos = gt + 1;
        } else {
            // 标题：找 </hN> 闭合，其间文本标 style=lvl
            int gt = find_sub(raw, rawlen, next, ">", 1);
            if (gt < 0) break;
            char close[5] = {'<', '/', 'h', (char)('0' + lvl), '>'};
            int ce = find_sub(raw, rawlen, gt + 1, close, 5);
            if (ce < 0) { pos = gt + 1; continue; } // 无闭合按普通文本继续扫
            push_text(raw + gt + 1, (size_t)ce - (gt + 1), lvl);
            pos = ce + 5;
        }
    }
    push_text(raw + pos, rawlen - pos, 0);
}

bool getChapterBlocks(const String& bookId, const ChapterEntry& ch, std::vector<ContentBlock>& out, String& err) {
    out.clear();
    ChapterRaw raw;
    if (!fetch_chapter_raw(bookId, ch, raw, err)) return false;

    // TXT 书：整章文本按 ≤48KB 切连续文本块（TXT 大章同样会超 64KB）
    if (raw.kind == CHAPTER_TXT) {
        size_t off = 0;
        while (off < raw.len) {
            size_t n = raw.len - off;
            if (n > TEXT_BLOCK_MAX) n = utf8_safe_cut(raw.data + off, TEXT_BLOCK_MAX);
            if (!n) n = 1; // 防御：保证前进不死循环
            ContentBlock b;
            b.is_image = false;
            b.text = String(raw.data + off, (unsigned int)n);
            out.push_back(b);
            off += n;
        }
        raw.release();
        if (out.empty()) { err = "章节为空"; return false; }
        return true;
    }

    // 整本 EPUB(zip)：解包定位本章 XHTML（包内图片转 epubimg: 标记）
    if (raw.kind == CHAPTER_EPUB_ZIP) {
        char* xhtml = nullptr;
        size_t xlen = 0;
        String edir;
        bool ok = epub_extract_chapter_xhtml((const uint8_t*)raw.data, raw.len,
                                             ch.chapterIdx, xhtml, xlen, edir, err);
        raw.release();
        if (!ok) return false;
        xhtml_to_blocks(xhtml, xlen, edir, out);
        heap_caps_free(xhtml);
        if (out.empty()) { err = "EPUB 章节无内容块"; return false; }
        return true;
    }

    // 常规 EPUB 分片：单章 XHTML 直接切块（PSRAM 数据）
    xhtml_to_blocks(raw.data, raw.len, "", out);
    raw.release();
    if (out.empty()) { err = "章节无内容块"; return false; }
    return true;
}

} // namespace weread_api
