// main.cpp — PaperS3 微信读书阅读器
// 流程：初始化 → WiFi(无配置/失败进配网门户) → 拉书架(cookie 过期进扫码登录) → 书架(封面,最近阅读排序)
//       → 书籍详情(封面/简介/热门评论/继续阅读/下载整本) → 图文混排正文
// 交互：阅读页 左半=上页 右半=下页 顶部=回书架；书架 点行=详情 底部=翻页 右上=字体/WiFi/登录；详情/字体页 顶部=返回
// 刷新：翻页 epd_fast 异步推送（不等波形），每 8 次翻页一次 epd_quality 全刷清残影
// 触摸：独立 10ms 高频任务采集，主循环只消费事件
// 存储：配置/进度/章节缓存均存 SD /weread/（配置 SPIFFS 回退）；封面插图缓存 /weread/img/
#include <M5Unified.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <SD.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <algorithm>
#include <time.h>
#include <mbedtls/platform.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "storage.h"
#include "weread_client.h"
#include "weread_api.h"
#include "edcbook_font.h"
#include "img_render.h"
#include "boot_logo.h"
#include "provision.h"
#include "weread_login.h"
#include "network_diag.h"
// qrcodegen 的枚举 Ecc::LOW/HIGH 与 Arduino 的 LOW/HIGH 宏冲突，包含前先 undef
#undef LOW
#undef HIGH
#include "qrcodegen.hpp"

static M5Canvas canvas(&M5.Display);
static std::vector<BookEntry> g_shelf;
static std::vector<ChapterEntry> g_chapters;
static String g_format; // epub / txt
static BookEntry g_cur_book; // 当前书（开书时从书架复制；快速恢复时从 state.json 重建，不依赖书架已加载）

// ---------- 阅读器状态 ----------
enum Screen { SCR_SHELF, SCR_BOOK, SCR_READING, SCR_FONT, SCR_TOC };
static Screen g_screen = SCR_SHELF;
static int g_shelf_page = 0;          // 书架当前页
static int g_book_idx = -1;           // 当前书（g_shelf 下标）
static int g_ch_idx = 0;              // 当前章（g_chapters 下标）
static String g_title;                // 当前章标题
static std::vector<ContentBlock> g_blocks; // 当前章图文块
struct Cursor { int blk; int off; };  // 分页游标：第 blk 块的 off 字节处
static std::vector<Cursor> g_pages;   // 每页起始游标
static int g_page = 0;                // 当前页
static int g_fast_count = 0;          // 快刷计数（每 N 次插一次全刷）
#define FULL_REFRESH_EVERY 8          // 每 N 次快刷做一次 quality 全刷清残影
#define SHELF_PER_PAGE 5              // 书架每页本数（封面行布局）
static unsigned long g_last_progsave_ms = 0; // 进度本地保存节流
static unsigned long g_last_activity = 0;    // 最后操作时间（闲置自动休眠用）
#define AUTO_SLEEP_MS (5UL * 60 * 1000)      // 闲置 5 分钟自动休眠
#define UI_FONT_PATH "/font/霞鹜文楷_大.bin" // 界面/菜单固定字体（霞鹜文楷）
static EdcFont g_ui_font;                    // UI 字体（固定，保证菜单永远可显示）
static EdcFont g_read_font;                  // 正文字体（阅读页内可切换，只影响正文）
static String g_read_font_path = UI_FONT_PATH; // 当前正文字体路径（字体选择页打勾用）
static volatile bool g_stage_ui_ok = false;    // 主任务拉取时允许画进度屏（防后台任务并发画屏）

// 前置声明（定义在后面的函数）
static void paginate_current();
static void render_page(epd_mode_t mode, bool wait);
static void shelf_relogin();
static void sort_shelf_recent();
static bool book_img_off(const String& bookId);
static String resolve_img(const String& ref);
static bool blocks_cache_exists(const String& bookId, const String& chapterUid);
static void blocks_cache_save(const String& bookId, const String& chapterUid, const std::vector<ContentBlock>& blocks);

// ---------- 触摸事件队列（touch_task → loop） ----------
struct TouchPoint { int16_t x, y; };
static QueueHandle_t g_touch_q = nullptr;

// 触摸采集任务：高频 M5.update()，手指落下（wasPressed）即入队，不等抬手
static void touch_task(void*) {
    while (true) {
        M5.update();
        if (M5.Touch.isEnabled()) {
            auto& t = M5.Touch.getDetail();
            if (t.wasPressed()) {
                TouchPoint pt{ t.x, t.y };
                xQueueSend(g_touch_q, &pt, 0); // 队列满则丢弃
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// 等待一次点按，超时返回 false（进入前丢弃旧事件）
static bool wait_tap(unsigned long timeout_ms) {
    if (!g_touch_q) { delay(timeout_ms); return false; }
    xQueueReset(g_touch_q);
    TouchPoint pt;
    return xQueueReceive(g_touch_q, &pt, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// ---------- 屏幕辅助 ----------
// EPD 阻塞刷新：setEpdMode 必须在 pushSprite 之前（写入时按模式抖动位深），pushSprite 自动触发异步刷新
static void epd_flush(epd_mode_t mode) {
    M5.Display.setEpdMode(mode);
    unsigned long t0 = millis();
    canvas.pushSprite(0, 0);
    M5.Display.waitDisplay(); // 等波形物理播放完
    Serial.printf("[刷新] mode=%d 耗时 %lu ms\n", (int)mode, millis() - t0);
}

// EPD 非阻塞刷新：推送后立即返回，波形由后台任务播放 —— 翻页用，点按不会被刷新阻塞/丢弃
static void epd_flush_async(epd_mode_t mode) {
    M5.Display.setEpdMode(mode);
    unsigned long t0 = millis();
    while (M5.Display.displayBusy()) delay(1); // 仅更新队列满才等（极端连翻）
    canvas.pushSprite(0, 0);
    Serial.printf("[刷新-异步] mode=%d 推送耗时 %lu ms\n", (int)mode, millis() - t0);
}

// 用指定 EDC 字体在 (x,y) 绘制一个 UTF-8 字符串，返回结束 x 坐标（scale=1/2 最近邻放大）
static int drawTextFontScaled(EdcFont& font, const String& utf8, int x, int y, int scale) {
    if (!font.loaded()) { canvas.drawString(utf8, x, y); return x + 36; }
    const uint8_t* p = (const uint8_t*)utf8.c_str();
    int cx = x;
    std::vector<uint8_t> pix;
    while (*p) {
        uint32_t cp = EdcFont::nextCodepoint(p);
        if (cp == 0) break;
        int idx = font.findGlyph((uint16_t)cp);
        if (idx < 0) { cx += font.fontHeight() / 2 * scale; continue; }
        int w, h, xo, yo, adv;
        if (!font.decodeGlyph(idx, pix, w, h, xo, yo, adv)) { cx += (adv > 0 ? adv : font.fontHeight() / 2) * scale; continue; }
        // 逐像素画（g: 0=黑..15=白），直接映射灰度，不反转
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                uint8_t g = pix[row * w + col];       // 0..15 (0=黑, 15=白)
                if (g >= 15) continue;                 // 白=背景，跳过
                uint8_t v = g * 255 / 15;              // 0(黑)..238(浅灰)
                uint16_t color = canvas.color565(v, v, v);
                canvas.fillRect(cx + xo * scale + col * scale, y + yo * scale + row * scale,
                                scale, scale, color);
            }
        }
        cx += (adv > 0 ? adv : w) * scale;
    }
    return cx;
}

static int drawTextFont(EdcFont& font, const String& utf8, int x, int y) {
    return drawTextFontScaled(font, utf8, x, y, 1);
}

// 仿粗体：x+1 二次绘制（位图字体没有真粗体）
static int drawTextFontEx(EdcFont& font, const String& utf8, int x, int y, int scale, bool bold) {
    int ex = drawTextFontScaled(font, utf8, x, y, scale);
    if (bold) drawTextFontScaled(font, utf8, x + 1, y, scale);
    return ex;
}

// UI 字体（固定文楷）：菜单/书架/标题/按钮等一切界面文字
static int drawUI(const String& s, int x, int y) { return drawTextFont(g_ui_font, s, x, y); }
// UI 仿粗体（标题栏等强调位置）
static int drawUIBold(const String& s, int x, int y) { return drawTextFontEx(g_ui_font, s, x, y, 1, true); }
// 正文字体（可切换）：仅阅读页正文
static int drawRead(const String& s, int x, int y) { return drawTextFont(g_read_font, s, x, y); }
// 按原书样式画正文行：0=正文，1=h1(2x+仿粗)，2..4=h2..h4(仿粗)
static int drawReadStyle(const String& s, int x, int y, uint8_t style) {
    if (style == 1) return drawTextFontEx(g_read_font, s, x, y, 2, true);
    if (style >= 2 && style <= 4) return drawTextFontEx(g_read_font, s, x, y, 1, true);
    return drawRead(s, x, y);
}

// 测量文本宽度
static int textWidthFont(EdcFont& font, const String& utf8) {
    if (!font.loaded()) return utf8.length() * 8;
    return font.textWidth(utf8);
}
static int textWidthUI(const String& s)   { return textWidthFont(g_ui_font, s); }
static int textWidthRead(const String& s) { return textWidthFont(g_read_font, s); }

// 按像素宽度截断字符串（超宽加 …）
static String truncate_px(const String& s, int max_w) {
    if (textWidthUI(s) <= max_w) return s;
    String out;
    const uint8_t* p = (const uint8_t*)s.c_str();
    while (*p) {
        const uint8_t* q = p;
        uint32_t cp = EdcFont::nextCodepoint(p);
        if (cp == 0) break;
        String ch((const char*)q, (unsigned int)(p - q));
        if (textWidthUI(out + ch + "…") > max_w) break;
        out += ch;
    }
    return out + "…";
}

static void screen_msg(const String& line1, const String& line2 = "", const String& line3 = "") {
    canvas.fillSprite(TFT_WHITE);
    if (g_ui_font.loaded()) { // 中文字体已加载：用 UI 字体画（内置字体没有中文）
        drawUI(line1, 24, 60);
        if (line2.length()) drawUI(line2, 24, 150);
        if (line3.length()) drawUI(line3, 24, 240);
    } else {
        canvas.setTextColor(TFT_BLACK, TFT_WHITE);
        canvas.setTextDatum(TL_DATUM);
        canvas.setTextSize(2);
        canvas.drawString(line1, 20, 30);
        canvas.drawString(line2, 20, 70);
        canvas.drawString(line3, 20, 110);
    }
    epd_flush(epd_mode_t::epd_quality);
}

// 开机首屏：微信读书图标 + 大标题 + 副标题（技术信息一律只走串口）
static void show_splash(const String& subtitle = "") {
    canvas.fillSprite(TFT_WHITE);
    // 图标 160x160（居中偏上，PROGMEM 灰度位图）
    int lx = (SCREEN_W - BOOT_LOGO_W) / 2, ly = 240;
    for (int yy = 0; yy < BOOT_LOGO_H; yy++)
        for (int xx = 0; xx < BOOT_LOGO_W; xx++) {
            uint8_t v = BOOT_LOGO[yy * BOOT_LOGO_W + xx];
            canvas.drawPixel(lx + xx, ly + yy, canvas.color565(v, v, v));
        }
    String t = "微信读书";
    if (g_ui_font.loaded()) { // 字体未就绪时只显示图标（开机第一次调用就是这样）
        drawUI(t, (SCREEN_W - textWidthUI(t)) / 2, ly + BOOT_LOGO_H + 56);
        if (subtitle.length())
            drawUI(subtitle, (SCREEN_W - textWidthUI(subtitle)) / 2, ly + BOOT_LOGO_H + 136);
    }
    epd_flush(epd_mode_t::epd_quality);
}

// 首屏状态行：文字叠加在首屏底部（快刷，不做全屏慢闪；用于 连接WiFi/加载书架 等进度提示）
static void splash_status(const String& text) {
    canvas.fillRect(0, 660, SCREEN_W, 80, TFT_WHITE); // 只清状态区
    drawUI(text, (SCREEN_W - textWidthUI(text)) / 2, 680);
    epd_flush(epd_mode_t::epd_fast);
}

// ---------- 二维码登录页 ----------
// weread_login 模块的 render_qr 回调：把 url 画成二维码 + 状态文字
static void render_qr(const String& url, const String& status) {
    using qrcodegen::QrCode;
    QrCode qr = QrCode::encodeText(url.c_str(), QrCode::Ecc::LOW);
    canvas.fillSprite(TFT_WHITE);
    drawUI("微信扫码登录", 20, 16);

    int size = qr.getSize();                 // 模块数（边长）
    int scale = 400 / size; if (scale < 2) scale = 2;
    int px = scale * size;
    int ox = (SCREEN_W - px) / 2, oy = 180;  // 居中
    // 白色静区 + 边框
    canvas.fillRect(ox - 16, oy - 16, px + 32, px + 32, TFT_WHITE);
    canvas.drawRect(ox - 16, oy - 16, px + 32, px + 32, TFT_BLACK);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (qr.getModule(x, y))
                canvas.fillRect(ox + x * scale, oy + y * scale, scale, scale, TFT_BLACK);

    drawUI(status, 20, oy + px + 40);
    drawUI("登录后自动加载书架", 20, oy + px + 96);
    epd_flush(epd_mode_t::epd_quality);
}

// 扫码登录循环：失败/超时后等点按重试；2 分钟无操作放弃
static bool do_qr_login() {
    while (true) {
        if (weread_login::run(render_qr, screen_msg)) return true;
        Serial.println("[login] 未完成，等待点按重试");
        if (!wait_tap(120000)) return false;
    }
}

// ---------- 配置小助手：改单个字段（保留其它字段） ----------
static void config_set(const String& key, const String& value) {
    JsonDocument doc;
    File f = open_config_read();
    if (f) { deserializeJson(doc, f); f.close(); }
    doc[key] = value;
    File w = open_config_write();
    if (w) { serializeJson(doc, w); w.close(); }
}

// ---------- 阅读进度：本地（SD /weread/progress.json）+ 云端上传 ----------
static void progress_save_local(const String& bookId, int ch_idx, int page) {
    if (!storage_sd_ok()) return;
    if (millis() - g_last_progsave_ms < 3000) return; // 翻页节流 3s
    g_last_progsave_ms = millis();
    JsonDocument doc;
    File f = SD.open("/weread/progress.json", "r");
    if (f) { deserializeJson(doc, f); f.close(); }
    JsonObject o = doc[bookId].to<JsonObject>();
    o["c"] = ch_idx; o["p"] = page;
    File w = SD.open("/weread/progress.json", "w");
    if (w) { serializeJson(doc, w); w.close(); }

    // 同步写"上次阅读位置"标记：任何方式重启（含电源键硬复位）后开机直接恢复到本页，
    // 体验等同"短按休眠、短按唤醒"（其他 PaperS3 项目也是断电+状态恢复的路子）
    JsonDocument st;
    st["bookId"] = bookId;
    st["c"] = ch_idx;
    st["p"] = page;
    st["title"] = g_cur_book.title;   // 快速恢复用（不依赖书架加载）
    st["cover"] = g_cur_book.cover;
    st["format"] = g_format;
    File sf = SD.open("/weread/state.json", "w");
    if (sf) { serializeJson(st, sf); sf.close(); }
}

static bool progress_load_local(const String& bookId, int& ch_idx, int& page) {
    ch_idx = 0; page = 0;
    if (!storage_sd_ok()) return false;
    File f = SD.open("/weread/progress.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e || !doc[bookId].is<JsonObject>()) return false;
    ch_idx = doc[bookId]["c"] | 0;
    page   = doc[bookId]["p"] | 0;
    return ch_idx > 0 || page > 0;
}

// ---------- 后台网络任务（下一章预取 + 进度上传，不占翻页路径） ----------
static SemaphoreHandle_t g_net_mtx = nullptr; // 网络/共享资源互斥（递归锁）
static void net_lock()   { if (g_net_mtx) xSemaphoreTakeRecursive(g_net_mtx, portMAX_DELAY); }
static void net_unlock() { if (g_net_mtx) xSemaphoreGiveRecursive(g_net_mtx); }

struct UploadJob { char bookId[32]; char uid[32]; int cidx; int pct; };
static QueueHandle_t g_upload_q = nullptr;    // UploadJob，进度上传
static QueueHandle_t g_prefetch_q = nullptr;  // PrefetchJob，预取下一章（带 bookId 校验防脏配对）
static QueueHandle_t g_coverpf_q = nullptr;   // int（书架页号），预取相邻页封面
static QueueHandle_t g_imgpre_q = nullptr;    // int（章下标），补下当前章插图

struct PrefetchJob { char bookId[32]; int next1; };

static void net_worker(void*) {
    UploadJob uj;
    PrefetchJob pf;
    for (;;) {
        // 章节预取优先（用户马上要看）；队列空时挂起等待
        if (xQueueReceive(g_prefetch_q, &pf, pdMS_TO_TICKS(2000)) == pdTRUE) {
            net_lock();
            // 上下文仍有效才做（bookId 必须还是当前书，防"书=A、目录=B"脏配对）
            if (g_cur_book.bookId.length() && g_cur_book.bookId == pf.bookId &&
                pf.next1 >= 1 && pf.next1 <= (int)g_chapters.size()) {
                BookEntry& b = g_cur_book;
                ChapterEntry& ch = g_chapters[pf.next1 - 1];
                if (!blocks_cache_exists(b.bookId, ch.chapterUid)) {
                    Serial.printf("[预取] 《%s》%s 开始\n", b.title.c_str(), ch.title.c_str());
                    std::vector<ContentBlock> blocks;
                    String err;
                    if (!weread_api::getChapterBlocks(b.bookId, ch, blocks, err)) {
                        delay(2000); // 偶发连接失败重试一次（与主路径同款）
                        weread_api::getChapterBlocks(b.bookId, ch, blocks, err);
                    }
                    if (!blocks.empty()) {
                        blocks_cache_save(b.bookId, ch.chapterUid, blocks);
                        int imgs = 0;
                        if (!book_img_off(b.bookId)) // 插图关闭的书预取不下载图
                            for (auto& blk : blocks) if (blk.is_image) { resolve_img(blk.text); imgs++; }
                        Serial.printf("[预取] 完成 %d 块 %d 图\n", blocks.size(), imgs);
                    } else {
                        Serial.println("[预取] 失败: " + err);
                    }
                }
            }
            net_unlock();
            delay(800); // 任务间节流：预取完成后歇一会再干下一个（防风控）
            continue;
        }
        // 封面预取（相邻书架页，翻页时封面已在缓存）
        int cp_page;
        if (g_coverpf_q && xQueueReceive(g_coverpf_q, &cp_page, 0) == pdTRUE) {
            net_lock();
            for (int pg = cp_page - 1; pg <= cp_page + 1; pg++) {
                int start = pg * SHELF_PER_PAGE;
                if (pg < 0 || start >= (int)g_shelf.size()) continue;
                int end = min(start + SHELF_PER_PAGE, (int)g_shelf.size());
                for (int i = start; i < end; i++)
                    if (g_shelf[i].cover.length()) resolve_img(g_shelf[i].cover);
            }
            net_unlock();
            continue;
        }
        // 本章插图补下（开章不挡，后台补齐；仍是当前章才做）
        int imgpre_ch;
        if (g_imgpre_q && xQueueReceive(g_imgpre_q, &imgpre_ch, 0) == pdTRUE) {
            net_lock();
            if (g_cur_book.bookId.length() && imgpre_ch == g_ch_idx) {
                int done = 0;
                for (auto& blk : g_blocks) {
                    if (!blk.is_image || img_render::cached(blk.text).length()) continue;
                    resolve_img(blk.text); // 内部有锁（递归互斥）
                    done++;
                }
                if (done) Serial.printf("[预取] 本章插图补下 %d 张\n", done);
            }
            net_unlock();
            continue;
        }
        // 没有预取任务时处理上传
        if (g_upload_q && xQueueReceive(g_upload_q, &uj, 0) == pdTRUE) {
            static unsigned long last_up = 0;
            if (millis() - last_up < 30000) continue; // 上传节流 30s
            last_up = millis();
            net_lock();
            String err;
            if (weread_api::uploadProgress(uj.bookId, uj.uid, uj.cidx, uj.pct, err))
                Serial.printf("[进度] 已上传 %d%%\n", uj.pct);
            else
                Serial.println("[进度] 上传失败: " + err);
            net_unlock();
        }
    }
}

// 上传当前进度（非阻塞：入队给后台任务；force 跳过节流粗筛）
static void maybe_upload_progress(bool force) {
    if (!g_cur_book.bookId.length() || g_ch_idx >= (int)g_chapters.size() || g_pages.empty()) return;
    if (!force) {
        static unsigned long last_enqueue = 0;
        if (millis() - last_enqueue < 30000) return; // 主侧节流粗筛
        last_enqueue = millis();
    }
    if (!g_upload_q) return;
    UploadJob uj;
    BookEntry& b = g_cur_book;
    ChapterEntry& ch = g_chapters[g_ch_idx];
    strlcpy(uj.bookId, b.bookId.c_str(), sizeof(uj.bookId));
    strlcpy(uj.uid, ch.chapterUid.c_str(), sizeof(uj.uid));
    uj.cidx = ch.chapterIdx;
    uj.pct = (int)((long)(g_page + 1) * 100 / g_pages.size());
    xQueueSend(g_upload_q, &uj, 0);
    // 本地书架进度同步更新（显示用）
    b.progress = uj.pct; b.progressChapterUid = ch.chapterUid;
    b.readUpdateTime = (uint32_t)time(nullptr);
}

// ---------- 缓存目录（按书分目录：/weread/cache/<bookId>/，互不错串，可整本清除） ----------
static String book_cache_dir(const String& bookId) {
    String d = String(SD_CACHE_DIR) + "/" + bookId;
    SD.mkdir(SD_WEREAD_DIR);
    SD.mkdir(SD_CACHE_DIR);
    SD.mkdir(d.c_str());
    return d;
}

// 章节块缓存：<bookId>/ch_<uid>.blk
// 格式：首行 "B1"，之后每块："I <url>\n" 或 "T <字节数>\n<原始字节>"
static String blocks_cache_path(const String& bookId, const String& chapterUid) {
    return book_cache_dir(bookId) + "/ch_" + chapterUid + ".blk";
}

static bool blocks_cache_load(const String& bookId, const String& chapterUid, std::vector<ContentBlock>& out) {
    if (!storage_sd_ok()) return false;
    File f = SD.open(blocks_cache_path(bookId, chapterUid), "r");
    if (!f) return false;
    // 注意 println 写的是 \r\n：每行都要 trim 掉 \r，否则魔数/URL 全坏（曾导致缓存永不命中）
    String magic = f.readStringUntil('\n'); magic.trim();
    if (magic != "B7") { f.close(); return false; } // B7：B6 及更早可能含脏配对内容，作废
    out.clear();
    while (f.available()) {
        String head = f.readStringUntil('\n'); head.trim();
        if (head.startsWith("I ")) {
            ContentBlock b; b.is_image = true; b.text = head.substring(2); b.text.trim();
            out.push_back(b);
        } else if (head.startsWith("T ")) {
            // 格式：T <style> <字节数>
            int sp = head.indexOf(' ', 2);
            int style = 0, len = 0;
            if (sp > 0) { style = head.substring(2, sp).toInt(); len = head.substring(sp + 1).toInt(); }
            if (len <= 0 || len > 200000) break;
            ContentBlock b; b.is_image = false; b.style = (uint8_t)style;
            b.text.reserve(len);
            for (int i = 0; i < len && f.available(); i++) b.text += (char)f.read();
            out.push_back(b);
            while (f.available() && (f.peek() == '\r' || f.peek() == '\n')) f.read(); // 跳过块尾 \r\n
        } else if (head.length() == 0) {
            continue;
        } else break;
    }
    f.close();
    return !out.empty();
}

static bool blocks_cache_exists(const String& bookId, const String& chapterUid) {
    return storage_sd_ok() && SD.exists(blocks_cache_path(bookId, chapterUid));
}

static void blocks_cache_save(const String& bookId, const String& chapterUid, const std::vector<ContentBlock>& blocks) {
    if (!storage_sd_ok() || blocks.empty()) return;
    SD.mkdir(SD_WEREAD_DIR);
    SD.mkdir(SD_CACHE_DIR);
    File f = SD.open(blocks_cache_path(bookId, chapterUid), "w");
    if (!f) return;
    f.println("B7");
    for (auto& b : blocks) {
        if (b.is_image) f.println("I " + b.text);
        else { f.printf("T %d %d\n", (int)b.style, b.text.length()); f.print(b.text); f.println(); }
    }
    f.close();
}

// ---------- 图片解析统一入口 ----------
// http(s) 直链 → img_render::fetch；tarimg:/epubimg: 标记（EPUB 整包防御路径）暂未实现跳过；
// 兼容旧缓存里错误归一化的 weread.qq.com/../ 形态（Web 阅读器 UI 装饰图，非书籍内容）→ 跳过
static String resolve_img(const String& ref) {
    if (ref.startsWith("tarimg:") || ref.startsWith("epubimg:")) return "";
    if (ref.indexOf("weread.qq.com/../") >= 0 || ref.indexOf("weread.qq.com/Images/") >= 0) return "";
    net_lock();   // 图片下载会话与后台预取任务互斥
    String p = img_render::fetch(ref);
    net_unlock();
    return p;
}

// ---------- 版式参数 ----------
// 中文排版规范：版心左右等宽居中；段首缩进两个中文字符；段间距 > 行间距
#define BODY_MARGIN 30                            // 版心左右边距（等宽居中）
static int page_max_w()     { return SCREEN_W - 2 * BODY_MARGIN; }
static int ui_line_h()      { return (g_ui_font.loaded() ? g_ui_font.fontHeight() : 22) + 10; }
static int read_line_h()    { return (g_read_font.loaded() ? g_read_font.fontHeight() : 22) + 10; }
static int page_area_h()    { return SCREEN_H - 80 - 40; } // 版心像素高（标题 80 + 页脚 40）
static int para_gap()       { return read_line_h() / 4; }  // 段间距 = 行距的四分之一（之前 lh/2 太宽）
// 首行缩进 = 两个中文字符宽（以"一"测宽，跟随正文字体）
static int para_indent_w()  { return (g_read_font.loaded() ? g_read_font.textWidth("一") : 18) * 2; }
// 是否段首：块起点或前一字符是换行
static bool is_para_start(const String& text, int idx) {
    return idx <= 0 || text[idx - 1] == '\n';
}
static int page_max_img_h() { return page_area_h() - 120; } // 标题/页脚/上下留白后的图片最大高

// 计算从 idx 开始的一行（按版心像素宽度折行）的结束 offset；分页与渲染共用，保证页与页衔接一致
// font：测宽用字体（正文=g_read_font，UI=g_ui_font）；wscale：宽度倍率（h1 放大 2 倍时传 2.0）
static int next_line_end(EdcFont& font, const String& text, int idx, int max_w, float wscale = 1.0f) {
    int tlen = text.length();
    int end = idx, w = 0;
    while (end < tlen) {
        uint8_t c = (uint8_t)text[end];
        if (c == '\n') { end++; break; }
        int step = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        String ch = text.substring(end, end + step);
        int cw = font.loaded() ? (int)(font.textWidth(ch) * wscale) : (int)((c < 0x80 ? 8 : 16) * wscale);
        if (w + cw > max_w && end > idx) break;
        w += cw; end += step;
    }
    return (end == idx) ? idx + 1 : end; // 防死循环
}

// 画一段多行文字（自动折行，限 max_lines 行），返回结束 y（UI 内容，用 UI 字体测宽/行高）
static int draw_paragraph(const String& text, int x, int y, int max_w, int max_lines) {
    int lines = 0, idx = 0, lh = ui_line_h();
    while (idx < (int)text.length() && lines < max_lines) {
        int end = next_line_end(g_ui_font, text, idx, max_w);
        String line = text.substring(idx, end);
        line.trim();
        if (line.length()) { drawUI(line, x, y); y += lh; lines++; }
        idx = end;
    }
    return y;
}

// ---------- 图片块尺寸（分页/渲染共用，保证一致） ----------
// 缩放规则与 img_render::draw 一致：等比缩进 max_w × max_img_h，永不上采样
static int img_scaled_h(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    float s = min((float)page_max_w() / w, (float)page_max_img_h() / h);
    if (s > 1.0f) s = 1.0f;
    return (int)(h * s);
}

// 取图片块高度：仅查缓存（不触发下载），未缓存用估算高；下载后若与估算不符会触发重新分页
#define IMG_EST_H 300 // 未下载插图的估算高度（分页占位用）
static int img_block_h(const String& url) {
    String p = img_render::cached(url);
    if (!p.length()) return IMG_EST_H;
    int w, h;
    if (!img_render::size(p, w, h)) return IMG_EST_H;
    return img_scaled_h(w, h);
}

// ---------- 书架 ----------
static void sort_shelf_recent() { // 最近阅读在前（无进度记录的保持原顺序在后）
    std::stable_sort(g_shelf.begin(), g_shelf.end(),
        [](const BookEntry& a, const BookEntry& b) { return a.readUpdateTime > b.readUpdateTime; });
}

static int shelf_per_page() { return SHELF_PER_PAGE; }
static int shelf_row_h()    { return (SCREEN_H - 140) / SHELF_PER_PAGE; }

static void show_shelf() {
    bool transition = (g_screen != SCR_SHELF);
    if (transition && g_screen == SCR_READING) maybe_upload_progress(true); // 离开阅读前传一次进度
    g_screen = SCR_SHELF;
    canvas.fillSprite(TFT_WHITE);

    // 标题行（短标题，右上角留 睡眠/WiFi/登录 入口）
    drawUI("书架 (" + String(g_shelf.size()) + ")", 20, 16);
    const char* tags[3] = {"登录", "WiFi", "睡眠"};
    int tx = SCREEN_W - 20;
    for (int i = 0; i < 3; i++) {
        tx -= textWidthUI(tags[i]);
        drawUI(tags[i], tx, 16);
        tx -= 24;
    }

    int row_h = shelf_row_h();
    int total_pages = max(1, ((int)g_shelf.size() + SHELF_PER_PAGE - 1) / SHELF_PER_PAGE);
    g_shelf_page = constrain(g_shelf_page, 0, total_pages - 1);

    int start = g_shelf_page * SHELF_PER_PAGE;
    int end = min(start + SHELF_PER_PAGE, (int)g_shelf.size());
    for (int i = start; i < end; i++) {
        int y = 60 + (i - start) * row_h;
        BookEntry& b = g_shelf[i];
        // 封面（resolve_img 内含网络锁，与后台预取互斥）
        String cp = b.cover.length() ? resolve_img(b.cover) : "";
        int drawn = cp.length() ? img_render::draw(&canvas, cp, 24, y + 6, 96, row_h - 12) : 0;
        if (drawn <= 0) { // 无封面/失败：画占位框
            canvas.drawRect(24, y + 6, 96, row_h - 12, TFT_BLACK);
            drawUI("无封面", 34, y + row_h / 2 - 18);
        }
        // 标题（最多 2 行）+ 作者 + 进度
        int tx0 = 140;
        int ty = draw_paragraph(truncate_px(b.title, 700), tx0, y + 14, SCREEN_W - tx0 - 24, 2);
        if (b.author.length()) drawUI(truncate_px(b.author, 300), tx0, ty + 4);
        if (b.progress > 0) {
            String pg = String(b.progress) + "%";
            drawUI(pg, SCREEN_W - 24 - textWidthUI(pg), y + row_h - 44);
        }
        canvas.drawFastHLine(20, y + row_h - 1, SCREEN_W - 40, TFT_BLACK);
    }

    // 底部：书架翻页（点左半=上一页，右半=下一页）
    drawUI("上页", 24, SCREEN_H - 44);
    String pg = String(g_shelf_page + 1) + "/" + String(total_pages);
    drawUI(pg, (SCREEN_W - textWidthUI(pg)) / 2, SCREEN_H - 44);
    drawUI("下页", SCREEN_W - 24 - textWidthUI("下页"), SCREEN_H - 44);
    epd_flush(epd_mode_t::epd_quality);
    if (transition && g_touch_q) xQueueReset(g_touch_q); // 跨屏时丢弃残留点按
    // 后台预取相邻页封面（翻到那页时封面已在缓存）
    if (g_coverpf_q) { int p = g_shelf_page; xQueueOverwrite(g_coverpf_q, &p); }
}

// ---------- 目录缓存（SD /weread/cache/toc_<bookId>.json；目录很少变） ----------
static String toc_cache_path(const String& bookId) {
    return book_cache_dir(bookId) + "/toc.json"; // 按书分目录（早期平铺文件已废弃）
}

static bool toc_cache_load(const String& bookId, std::vector<ChapterEntry>& out, String& format) {
    if (!storage_sd_ok()) return false;
    File f = SD.open(toc_cache_path(bookId), "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) { out.clear(); return false; } // 失败必须清空，防"当前书=A、目录=B"错位
    format = doc["f"] | "";
    out.clear();
    for (JsonObject c : doc["c"].as<JsonArray>()) {
        ChapterEntry ce;
        ce.chapterUid = c["u"].as<String>();
        ce.title      = c["t"].as<String>();
        ce.chapterIdx = c["i"] | 0;
        ce.tar        = c["tar"].as<String>();
        if (ce.chapterUid.length()) out.push_back(ce);
    }
    return !out.empty();
}

static void toc_cache_save(const String& bookId, const std::vector<ChapterEntry>& chs, const String& format) {
    if (!storage_sd_ok() || chs.empty()) return;
    JsonDocument doc;
    doc["f"] = format;
    JsonArray arr = doc["c"].to<JsonArray>();
    for (auto& ce : chs) {
        JsonObject c = arr.add<JsonObject>();
        c["u"] = ce.chapterUid; c["t"] = ce.title; c["i"] = ce.chapterIdx;
        if (ce.tar.length()) c["tar"] = ce.tar;
    }
    File f = SD.open(toc_cache_path(bookId), "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

// 拉目录到 g_chapters（先 SD 缓存，未命中走网络并缓存；网络段加锁与后台任务互斥）
static bool fetch_toc(const String& bookId, String& err) {
    if (toc_cache_load(bookId, g_chapters, g_format)) {
        Serial.printf("[目录] 缓存命中 %s %d 章\n", bookId.c_str(), g_chapters.size());
        return true;
    }
    g_chapters.clear(); // 先清掉别的书的目录：否则网络失败时会把错目录存进新书的缓存（脏配对污染源！）
    // 偶发连接失败（sock<0）最多试 3 次，退避 1.5s/3s
    for (int attempt = 0; attempt < 3 && g_chapters.empty(); attempt++) {
        if (attempt) delay(1500 * attempt);
        net_lock();
        weread_api::getChapterInfos(bookId, g_chapters, g_format, err);
        net_unlock();
    }
    if (g_chapters.empty()) return false;
    toc_cache_save(bookId, g_chapters, g_format);
    return true;
}
// ---------- 详情缓存（SD /weread/cache/detail_<bookId>.json；简介+评论，命中即离线可读） ----------
static String detail_cache_path(const String& bookId) {
    return book_cache_dir(bookId) + "/detail.json";
}

static bool detail_cache_load(const String& bookId, BookDetail& d, std::vector<Review>& revs) {
    if (!storage_sd_ok()) return false;
    File f = SD.open(detail_cache_path(bookId), "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) return false;
    d.intro     = doc["i"] | "";
    d.publisher = doc["p"] | "";
    revs.clear();
    for (JsonObject r : doc["r"].as<JsonArray>()) {
        Review rv;
        rv.nick    = r["n"].as<String>();
        rv.content = r["c"].as<String>();
        if (rv.content.length()) revs.push_back(rv);
    }
    return d.intro.length() || !revs.empty();
}

static void detail_cache_save(const String& bookId, const BookDetail& d, const std::vector<Review>& revs) {
    if (!storage_sd_ok()) return;
    if (!d.intro.length() && revs.empty()) return; // 空详情不缓存（多半是临时失败）
    JsonDocument doc;
    doc["i"] = d.intro;
    doc["p"] = d.publisher;
    JsonArray arr = doc["r"].to<JsonArray>();
    for (auto& rv : revs) {
        JsonObject r = arr.add<JsonObject>();
        r["n"] = rv.nick; r["c"] = rv.content;
    }
    File f = SD.open(detail_cache_path(bookId), "w");
    if (f) { serializeJson(doc, f); f.close(); }
}

// ---------- 插图开关（按书记忆，SD /weread/imgoff.json；图多的书关掉后不下载不渲染） ----------
static bool book_img_off(const String& bookId) {
    if (!storage_sd_ok()) return false;
    File f = SD.open("/weread/imgoff.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    return !e && (doc[bookId] | false);
}

static void book_img_off_set(const String& bookId, bool off) {
    if (!storage_sd_ok()) return;
    JsonDocument doc;
    File f = SD.open("/weread/imgoff.json", "r");
    if (f) { deserializeJson(doc, f); f.close(); }
    if (off) doc[bookId] = true;
    else doc.remove(bookId);
    File w = SD.open("/weread/imgoff.json", "w");
    if (w) { serializeJson(doc, w); w.close(); }
}

// ---------- 书籍详情页 ----------
static BookDetail g_detail;
static std::vector<Review> g_reviews;

static void render_book_detail() {
    g_screen = SCR_BOOK;
    canvas.fillSprite(TFT_WHITE);
    BookEntry& b = g_cur_book;
    String title = g_detail.title.length() ? g_detail.title : b.title;
    String author = g_detail.author.length() ? g_detail.author : b.author;

    drawUI("书籍详情", 20, 16);
    drawUI("返回", SCREEN_W - 20 - textWidthUI("返回"), 16);

    // 封面大图（左；resolve_img 内含网络锁）
    String cp = b.cover.length() ? resolve_img(b.cover) : "";
    int drawn = cp.length() ? img_render::draw(&canvas, cp, 20, 80, 190, 260) : 0;
    if (drawn <= 0) { canvas.drawRect(20, 80, 190, 260, TFT_BLACK); drawUI("无封面", 60, 200); }

    // 右上：标题/作者/出版社
    int x0 = 236, y = 96;
    y = draw_paragraph(title, x0, y, SCREEN_W - x0 - 20, 3) + 8;
    if (author.length()) { drawUI(truncate_px(author, 260), x0, y); y += ui_line_h(); }
    if (g_detail.publisher.length()) { drawUI(truncate_px(g_detail.publisher, 260), x0, y); y += ui_line_h(); }
    if (b.progress > 0) {
        String pg = "已读 " + String(b.progress) + "%";
        drawUI(pg, x0, y); y += ui_line_h();
    }

    // 简介
    y = 370;
    if (g_detail.intro.length()) {
        drawUI("简介", 20, y); y += ui_line_h();
        y = draw_paragraph(g_detail.intro, 20, y, SCREEN_W - 48, 5) + 8;
    }

    // 热门评论（最多 2 条，各 3 行）
    if (!g_reviews.empty()) {
        drawUI("热门评论", 20, y); y += ui_line_h();
        for (size_t i = 0; i < g_reviews.size() && i < 2 && y < SCREEN_H - 260; i++) {
            String line = g_reviews[i].nick + "：" + g_reviews[i].content;
            y = draw_paragraph(line, 20, y, SCREEN_W - 48, 3) + 6;
        }
    }

    // 底部按钮：[继续阅读] [下载整本]；下方小开关：[插图:开/关]（图多的书关掉后不下载不渲染）
    canvas.drawRect(30, SCREEN_H - 130, 220, 66, TFT_BLACK);
    canvas.drawRect(290, SCREEN_H - 130, 220, 66, TFT_BLACK);
    drawUI("继续阅读", 30 + (220 - textWidthUI("继续阅读")) / 2, SCREEN_H - 116);
    drawUI("下载整本", 290 + (220 - textWidthUI("下载整本")) / 2, SCREEN_H - 116);
    bool imgoff = book_img_off(b.bookId);
    canvas.drawRect(30, SCREEN_H - 56, 220, 44, TFT_BLACK);
    String imgt = imgoff ? "插图：关" : "插图：开";
    drawUI(imgt, 30 + (220 - textWidthUI(imgt)) / 2, SCREEN_H - 48);
    drawUI(g_chapters.empty() ? "目录获取失败" : ("共 " + String(g_chapters.size()) + " 章"),
            290, SCREEN_H - 46);
    epd_flush(epd_mode_t::epd_quality);
    if (g_touch_q) xQueueReset(g_touch_q);
}

static void open_book_detail(int shelf_idx) {
    g_book_idx = shelf_idx;
    g_cur_book = g_shelf[shelf_idx];
    BookEntry& b = g_cur_book;
    String err;
    g_detail = BookDetail();
    g_reviews.clear();

    // 详情先查缓存：命中则离线直出（简介/评论不重新联网）
    if (detail_cache_load(b.bookId, g_detail, g_reviews)) {
        Serial.println("[详情] 缓存命中");
    } else {
        screen_msg("加载详情...", b.title);
        net_lock();
        for (int i = 0; i < 2; i++) { // 偶发连接中止重试一次
            if (weread_api::getBookDetail(b.bookId, g_detail, err)) break;
            if (i == 0) { Serial.println("[详情] info 重试"); delay(1000); }
        }
        for (int i = 0; i < 2; i++) {
            if (weread_api::getHotReviews(b.bookId, g_reviews, 2, err)) break;
            if (i == 0) { Serial.println("[详情] 评论重试"); delay(1000); }
        }
        net_unlock();
        detail_cache_save(b.bookId, g_detail, g_reviews);
    }

    if (!fetch_toc(b.bookId, err)) {
        screen_msg("目录获取失败", err); Serial.println("[错误] 目录: " + err);
        if (g_touch_q) xQueueReset(g_touch_q);
        return;
    }
    Serial.printf("[详情] 《%s》 %d 章 评论 %d 条\n", b.title.c_str(), g_chapters.size(), g_reviews.size());
    render_book_detail();
}

// ---------- 打开书 / 章 ----------
static void open_chapter(int ch_idx, int page = 0);

// 正文获取失败处理页：显示原因，点按=重试，顶部=返回详情/书架；30s 无操作视为返回
static bool chapter_fail_wait(const String& err) {
    String hint = err.indexOf("HTTP -1") >= 0 || err.indexOf("请求失败") >= 0 ? "网络连接失败，检查 WiFi 或稍后再试"
                : err.indexOf("2012") >= 0 ? "登录过期，返回后点右上角登录重扫"
                : "可能被风控，稍后再试";
    canvas.fillSprite(TFT_WHITE);
    drawUIBold("正文获取失败", 24, 140);
    drawUI(truncate_px(err, 440), 24, 240);
    drawUI(hint, 24, 320);
    canvas.drawFastHLine(16, 380, SCREEN_W - 32, TFT_BLACK);
    drawUI("点按屏幕 = 重试", 24, 430);
    drawUI("点顶部 = 返回", 24, 500);
    epd_flush(epd_mode_t::epd_quality);
    if (g_touch_q) xQueueReset(g_touch_q);
    TouchPoint pt;
    if (!g_touch_q || xQueueReceive(g_touch_q, &pt, pdMS_TO_TICKS(30000)) != pdTRUE) return false;
    return pt.y >= 70; // 中下部=重试，顶部=返回
}

// 继续阅读：服务器进度(章级)优先，本地进度(页级)补足
static void continue_reading() {
    if (!g_cur_book.bookId.length() || g_chapters.empty()) return;
    BookEntry& b = g_cur_book;
    int ch_idx = 0, page = 0;
    int srv_ch = -1;
    if (b.progressChapterUid.length()) {
        for (int i = 0; i < (int)g_chapters.size(); i++)
            if (g_chapters[i].chapterUid == b.progressChapterUid) { srv_ch = i; break; }
    }
    int loc_ch = 0, loc_page = 0;
    bool has_loc = progress_load_local(b.bookId, loc_ch, loc_page);
    if (srv_ch >= 0) {
        ch_idx = srv_ch;
        if (has_loc && loc_ch == srv_ch) page = loc_page;
    } else if (has_loc) {
        ch_idx = loc_ch; page = loc_page;
    }
    ch_idx = constrain(ch_idx, 0, (int)g_chapters.size() - 1);
    Serial.printf("[继续阅读] ch=%d page=%d (srv=%d loc=%d/%d)\n", ch_idx, page, srv_ch, loc_ch, loc_page);
    open_chapter(ch_idx, page);
}

// 下载进度屏（含进度条），异步推送不阻塞
static void draw_download_progress(const String& title, int done, int total, int fetched) {
    canvas.fillSprite(TFT_WHITE);
    drawUI("下载整本", 24, 60);
    drawUI(truncate_px(title, 440), 24, 150);
    drawUI(String("进度 ") + done + "/" + total + (fetched ? String(" (新 ") + fetched + ")" : ""), 24, 240);
    // 进度条
    int bw = SCREEN_W - 48;
    canvas.drawRect(24, 300, bw, 26, TFT_BLACK);
    if (total > 0) canvas.fillRect(26, 302, (int)((long)(bw - 4) * done / total), 22, TFT_BLACK);
    drawUI("点按屏幕取消", 24, 390);
    epd_flush_async(epd_mode_t::epd_fast);
}

// 下载整本：逐章拉 blocks 存 SD 缓存（插图上屏时才按需下载，不在此下载）；点按取消
static void download_all() {
    if (!g_cur_book.bookId.length() || g_chapters.empty()) return;
    BookEntry& b = g_cur_book;
    int total = g_chapters.size(), done = 0, fetched = 0;
    unsigned long t0 = millis();
    draw_download_progress(b.title, 0, total, 0); // 立即显示，别让用户等
    for (int i = 0; i < total; i++) {
        // 取消检测（触摸任务在后台填充队列）
        TouchPoint pt;
        if (g_touch_q && xQueuePeek(g_touch_q, &pt, 0)) {
            xQueueReceive(g_touch_q, &pt, 0);
            xQueueReset(g_touch_q);
            screen_msg("下载已取消", b.title, String("已完成 ") + done + "/" + total);
            render_book_detail();
            return;
        }
        ChapterEntry& ch = g_chapters[i];
        g_last_activity = millis(); // 下载期间保持活跃：防止完成后闲置计时器过期立即休眠
        if (blocks_cache_exists(b.bookId, ch.chapterUid)) { done++; }
        else {
            std::vector<ContentBlock> blocks;
            String err;
            net_lock();
            if (!weread_api::getChapterBlocks(b.bookId, ch, blocks, err)) {
                delay(2000); // 偶发连接失败重试一次
                weread_api::getChapterBlocks(b.bookId, ch, blocks, err);
            }
            net_unlock();
            if (!blocks.empty()) {
                blocks_cache_save(b.bookId, ch.chapterUid, blocks);
                done++; fetched++;
            } else {
                Serial.printf("[下载] 第%d章失败: %s\n", i + 1, err.c_str());
            }
        }
        draw_download_progress(b.title, done, total, fetched); // 每章更新（异步推送）
    }
    Serial.printf("[下载] 完成 %d/%d 新拉 %d 耗时 %lus\n", done, total, fetched, (millis() - t0) / 1000);
    screen_msg("下载完成", b.title, String(done) + "/" + String(total) + " 章");
    render_book_detail();
}

// 串口直读（跳过详情页）：N:C
static void open_book(int shelf_idx, int ch_idx_1based = 1) {
    BookEntry& b = g_shelf[shelf_idx];
    screen_msg("拉取目录...", b.title);
    String err;
    if (!fetch_toc(b.bookId, err)) {
        screen_msg("目录获取失败", err); Serial.println("[错误] 目录: " + err);
        if (g_touch_q) xQueueReset(g_touch_q);
        return;
    }
    g_book_idx = shelf_idx;
    g_cur_book = g_shelf[shelf_idx];
    Serial.printf("[目录] 《%s》 %d 章 format=%s\n", b.title.c_str(), g_chapters.size(), g_format.c_str());
    if (ch_idx_1based < 1 || ch_idx_1based > (int)g_chapters.size()) ch_idx_1based = 1;
    open_chapter(ch_idx_1based - 1);
}

// ---------- 休眠（电源键不可读：入睡走屏幕入口/闲置超时，唤醒=按电源键冷启动+状态恢复） ----------
// 入睡：全屏显示当前书封面（墨水屏断电后图像永久保持），然后整机断电
static void enter_sleep() {
    Serial.println("[睡眠] 进入休眠，封面显示；再按电源键开机恢复");
    // 入睡前兜底保存轮换过的 cookie（防重启后旧 cookie 失效要重扫）
    if (WR.cookiesDirty()) { WR.saveCookiesToConfig(); WR.clearCookiesDirty(); }
    canvas.fillSprite(TFT_WHITE);
    if (g_cur_book.bookId.length()) {
        BookEntry& b = g_cur_book;
        // 写一次性恢复标记（开机读到就回到本页）
        JsonDocument doc;
        doc["bookId"] = b.bookId;
        doc["c"] = g_ch_idx;
        doc["p"] = g_page;
        File w = SD.open("/weread/state.json", "w");
        if (w) { serializeJson(doc, w); w.close(); }
        // 封面全屏（等比缩放居中），下面留书名（resolve_img 内含网络锁）
        String cp = b.cover.length() ? resolve_img(b.cover) : "";
        int drawn = cp.length() ? img_render::draw(&canvas, cp, 0, 60, SCREEN_W, SCREEN_H - 220) : 0;
        if (drawn <= 0) { // 无封面：大字书名代替
            String t = truncate_px(b.title, 400);
            drawUI(t, (SCREEN_W - textWidthUI(t)) / 2, 430);
        }
        drawUI(truncate_px(b.title, 460), 24, SCREEN_H - 120);
        drawUI("已休眠 · 摸屏幕唤醒", 24, SCREEN_H - 64);
    } else {
        String t = "微信读书";
        drawUI(t, (SCREEN_W - textWidthUI(t)) / 2, 430);
        drawUI("已休眠", (SCREEN_W - textWidthUI("已休眠")) / 2, 510);
    }
    epd_flush(epd_mode_t::epd_quality);
    M5.Display.waitDisplay();
    delay(2000);            // 等波形真正刷完再断 EPD 电（M5ReadPaper 经验）
    M5.Display.sleep();     // EPD 断电（墨水屏图像永久保持）
    Serial.println("[睡眠] 进入浅睡，摸屏幕唤醒");
    M5.Power.lightSleep(0, true); // 浅睡 + GPIO48 触摸唤醒（内部含防误醒等待），醒后从下一行继续
    // ---- 唤醒点（程序原地恢复，不重启）----
    Serial.println("[睡眠] 已唤醒");
    M5.Display.wakeup();    // EPD 重新上电
    g_last_activity = millis();
    // 浅睡期间 WiFi 断开：重连 + 重新校时
    if (WiFi.status() != WL_CONNECTED) {
        canvas.fillSprite(TFT_WHITE);
        drawUI("已唤醒，正在连接 WiFi...", 24, 400);
        epd_flush(epd_mode_t::epd_fast);
        WR.connectWiFi();
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    }
    // 重绘当前界面，原地恢复
    if (g_screen == SCR_READING && !g_pages.empty()) render_page(epd_mode_t::epd_quality, true);
    else show_shelf();
    if (g_touch_q) xQueueReset(g_touch_q); // 丢掉唤醒那次触摸事件
}

// 休眠恢复：开机时若存在一次性标记 /weread/state.json，直接回到上次阅读页
static bool try_restore_reading() {
    if (!storage_sd_ok()) return false;
    File f = SD.open("/weread/state.json", "r");
    if (!f) return false;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    // 不删标记：它随翻页持续重写，永远指向最近阅读位置（回书架时会显式删除）
    if (e) return false;
    String bid = doc["bookId"] | "";
    int c = doc["c"] | 0, p = doc["p"] | 0;
    if (!bid.length()) return false;

    // 书架已加载则找下标（预取/详情上下文用）；快速恢复时书架可能还没加载，不依赖它
    g_book_idx = -1;
    for (int i = 0; i < (int)g_shelf.size(); i++)
        if (g_shelf[i].bookId == bid) { g_book_idx = i; break; }

    // 恢复当前书上下文（字段全部来自 state.json，不依赖书架）
    g_cur_book.bookId = bid;
    g_cur_book.title  = doc["title"] | "";
    g_cur_book.cover  = doc["cover"] | "";
    g_format          = doc["format"] | "";
    if (g_book_idx >= 0) g_cur_book = g_shelf[g_book_idx]; // 书架有则用全量

    // 目录必须命中缓存，否则放弃（走正常书架流程）
    if (!toc_cache_load(bid, g_chapters, g_format)) {
        Serial.println("[睡眠] 目录无缓存，放弃快速恢复");
        g_cur_book.bookId = ""; // 重置，防"当前书=A、目录=B"错位
        g_book_idx = -1;
        return false;
    }
    c = constrain(c, 0, (int)g_chapters.size() - 1);
    Serial.printf("[睡眠] 恢复《%s》ch=%d page=%d\n", g_cur_book.title.c_str(), c, p);
    open_chapter(c, p);
    return g_screen == SCR_READING;
}

static void open_chapter(int ch_idx, int page) {
    if (!g_cur_book.bookId.length() || ch_idx < 0 || ch_idx >= (int)g_chapters.size()) return;
    BookEntry& b = g_cur_book;
    ChapterEntry& ch = g_chapters[ch_idx];
    g_title = ch.title; // 提前设置：拉取进度屏要用（否则显示上一章的旧标题）

    // 先查 SD 缓存，未命中再走网络（blocks 交换整体加锁：后台插图预取任务会并发读 g_blocks）
    net_lock();
    bool from_cache = blocks_cache_load(b.bookId, ch.chapterUid, g_blocks);
    if (from_cache) {
        Serial.printf("[缓存] 命中 %s_%s %d 块\n", b.bookId.c_str(), ch.chapterUid.c_str(), g_blocks.size());
    } else {
        screen_msg("拉取正文...", ch.title);
        String err;
        g_stage_ui_ok = true; // 主任务拉取期间允许进度屏（后台任务的打点被禁画）
        bool ok = weread_api::getChapterBlocks(b.bookId, ch, g_blocks, err);
        if (!ok) {
            // e_0 偶发 sock 连接失败（实测重试即恢复）：等 2 秒自动重试一次再报错
            Serial.println("[正文] 首次失败，2s 后重试: " + err);
            delay(2000);
            ok = weread_api::getChapterBlocks(b.bookId, ch, g_blocks, err);
        }
        g_stage_ui_ok = false;
        if (!ok) {
            net_unlock();
            Serial.println("[错误] 正文: " + err);
            // 失败页：点按=重试，顶部=返回（详情/书架）
            if (chapter_fail_wait(err)) { open_chapter(ch_idx, page); return; }
            if (g_chapters.size() && g_book_idx >= 0) render_book_detail();
            else show_shelf();
            return;
        }
        blocks_cache_save(b.bookId, ch.chapterUid, g_blocks);
    }
    net_unlock();

    g_ch_idx = ch_idx;
    g_title = ch.title;
    g_last_activity = millis(); // 拉取/预取可能耗时较长，完成后重置闲置计时
    Serial.printf("[正文] 《%s》%s %d 块%s\n", b.title.c_str(), ch.title.c_str(), g_blocks.size(),
                  from_cache ? "（缓存）" : "");
    paginate_current();
    g_page = constrain(page, 0, (int)g_pages.size() - 1);
    g_fast_count = 0;
    g_screen = SCR_READING;
    Serial.printf("[分页] 共 %d 页，起始第 %d 页\n", g_pages.size(), g_page + 1);
    render_page(epd_mode_t::epd_quality, true); // 章首页全刷（顺带清残影）
    if (g_touch_q) xQueueReset(g_touch_q);      // 丢弃拉取/渲染期间的点按
    progress_save_local(b.bookId, g_ch_idx, g_page);
    maybe_upload_progress(true);
    // 后台预取下一章（用户翻到时秒开）；队列只保留最新目标，带 bookId 校验
    if (g_prefetch_q && g_ch_idx + 1 < (int)g_chapters.size()) {
        PrefetchJob pj;
        strlcpy(pj.bookId, b.bookId.c_str(), sizeof(pj.bookId));
        pj.next1 = g_ch_idx + 2; // 下一章 1-based
        xQueueOverwrite(g_prefetch_q, &pj);
    }
    // 本章插图交给后台慢慢下（不再开章时全部堵下载，图多的章不再卡"加载插图"）
    if (g_imgpre_q) xQueueOverwrite(g_imgpre_q, &g_ch_idx);
}

// ---------- 正文分页（图文混排，像素级；段首缩进两字符 + 段间距） ----------
// 把 g_blocks 切成页：记录每页起始游标（空行被 trim 不占行，与渲染逻辑一致）
static void paginate_current() {
    g_pages.clear();
    g_pages.push_back({0, 0});
    Cursor cur{0, 0};
    int y = 0, H = page_area_h(), max_w = page_max_w();
    int lh = read_line_h(), pg_gap = para_gap();
    while (cur.blk < (int)g_blocks.size()) {
        ContentBlock& b = g_blocks[cur.blk];
        if (b.is_image) {
            if (book_img_off(g_cur_book.bookId)) { cur.blk++; cur.off = 0; continue; } // 插图关闭：整块跳过
            int need = img_block_h(b.text) + lh / 2; // 图后间距
            if (y + need > H && y > 0) { // 放不下整块移下页
                g_pages.push_back({cur.blk, 0});
                y = 0;
                continue;
            }
            y += need;
            cur.blk++; cur.off = 0;
        } else {
            if (cur.off >= (int)b.text.length()) { cur.blk++; cur.off = 0; continue; }
            uint8_t st = b.style;
            int blh = (st == 1) ? lh * 2 : lh;          // h1 行高放大（2x 渲染）
            float wsc = (st == 1) ? 2.0f : 1.0f;         // h1 宽度倍率
            bool para = (st == 0) && is_para_start(b.text, cur.off); // 标题不做首行缩进
            int indent = para ? para_indent_w() : 0;
            int end = next_line_end(g_read_font, b.text, cur.off, max_w - indent, wsc);
            String line = b.text.substring(cur.off, end);
            line.trim();
            if (line.length()) {
                int need = blh + ((para && y > 0) ? pg_gap : 0); // 段首行额外加段间距（页首不加）
                if (y + need > H && y > 0) {
                    g_pages.push_back({cur.blk, cur.off});
                    y = 0;
                    continue; // 本行移下页，重新评估
                }
                y += need;
            }
            cur.off = end;
        }
    }
}

// 渲染第 g_page 页（标题 + 图文 + 页码）；wait=false 时异步推送不阻塞（翻页即时响应）
static void render_page(epd_mode_t mode, bool wait) {
    canvas.fillSprite(TFT_WHITE);

    // 标题（UI 仿粗体，纯黑）；右上角"字体"入口提示
    drawUIBold(truncate_px(g_title, 300), 20, 14);
    drawUI("书架", 350, 14);
    drawUI("字体", SCREEN_W - 20 - textWidthUI("字体"), 14);
    canvas.drawFastHLine(16, 60, SCREEN_W - 32, TFT_BLACK);

    int lh = read_line_h(), pg_gap = para_gap();
    int max_w = page_max_w(), H = 80 + page_area_h();
    int y = 80;
    // 插图总数（下载进度提示用）
    int img_total = 0, img_no = 0;
    for (auto& b : g_blocks) if (b.is_image) img_total++;
    Cursor cur = g_pages[g_page];
    while (cur.blk < (int)g_blocks.size()) {
        ContentBlock& b = g_blocks[cur.blk];
        if (b.is_image) {
            if (book_img_off(g_cur_book.bookId)) { cur.blk++; cur.off = 0; continue; } // 插图关闭：整块跳过
            img_no++;
            // 未缓存的图：先提示"下载插图 n/m"再下载（防图多章节像卡死）；期间可点按取消
            String cp = img_render::cached(b.text);
            String p = cp;
            if (!cp.length()) {
                drawUI(String("下载插图 ") + img_no + "/" + img_total + "...", BODY_MARGIN, SCREEN_H - 44);
                epd_flush_async(epd_mode_t::epd_fast);
                // 点按取消：给用户一个出口
                TouchPoint pt;
                if (g_touch_q && xQueuePeek(g_touch_q, &pt, 0)) {
                    xQueueReset(g_touch_q);
                    screen_msg("已取消", g_cur_book.title, "点按重进");
                    return;
                }
                p = resolve_img(b.text);
            }
            int h = 0;
            if (p.length()) {
                int iw, ih;
                if (img_render::size(p, iw, ih) && !cp.length()) {
                    // 刚下载的图：校验估算是否偏差，偏了重新分页一次（每图最多一次）
                    if (img_scaled_h(iw, ih) != IMG_EST_H) {
                        paginate_current();
                        g_page = constrain(g_page, 0, (int)g_pages.size() - 1);
                        render_page(mode, wait);
                        return;
                    }
                }
                h = img_render::draw(&canvas, p, BODY_MARGIN, y, max_w, page_max_img_h());
            }
            if (h <= 0) { drawUI("[图片]", BODY_MARGIN, y); h = lh; }
            y += h + lh / 2;
            cur.blk++; cur.off = 0;
            if (y > SCREEN_H - 60) break; // 兜底防溢出
        } else {
            if (cur.off >= (int)b.text.length()) { cur.blk++; cur.off = 0; continue; }
            uint8_t st = b.style;
            int blh = (st == 1) ? lh * 2 : lh;
            float wsc = (st == 1) ? 2.0f : 1.0f;
            bool para = (st == 0) && is_para_start(b.text, cur.off); // 标题不做首行缩进
            int indent = para ? para_indent_w() : 0;
            int need = blh + ((para && y > 80) ? pg_gap : 0); // 段首行加段间距（页首不加）
            if (y + need > H && y > 80) break;                // 与分页同款判定
            if (para && y > 80) y += pg_gap;
            int end = next_line_end(g_read_font, b.text, cur.off, max_w - indent, wsc);
            String line = b.text.substring(cur.off, end);
            line.trim();
            if (line.length()) {
                drawReadStyle(line, BODY_MARGIN + indent, y, st); // 按原书样式（h1 放大/h2..h4 仿粗）
                y += blh;
            }
            cur.off = end;
        }
    }

    // 页脚只留页码（点按区域：左半屏=上一页，右半屏=下一页，顶部=目录/书架/字体）
    String pg = String(g_page + 1) + "/" + String(g_pages.size());
    drawUI(pg, (SCREEN_W - textWidthUI(pg)) / 2, SCREEN_H - 44);
    if (wait) epd_flush(mode); else epd_flush_async(mode);
}

// 翻页：章末再翻 → 下一章；章首回翻 → 上一章；平时 epd_fast 异步快刷，每 N 页一次全刷
static void turn_page(int delta) {
    if (g_screen != SCR_READING || g_pages.empty()) return;
    int np = g_page + delta;
    if (np >= (int)g_pages.size()) {
        if (g_ch_idx + 1 < (int)g_chapters.size()) { open_chapter(g_ch_idx + 1); return; }
        if (g_page == (int)g_pages.size() - 1) return; // 全书末页，原地
        np = g_pages.size() - 1;
    } else if (np < 0) {
        if (g_ch_idx > 0) { open_chapter(g_ch_idx - 1); return; }
        if (g_page == 0) return;
        np = 0;
    }
    g_page = np;
    g_fast_count++;
    epd_mode_t mode = (g_fast_count % FULL_REFRESH_EVERY == 0) ? epd_mode_t::epd_quality : epd_mode_t::epd_fast;
    Serial.printf("[翻页] %d/%d (%s)\n", g_page + 1, g_pages.size(), mode == epd_mode_t::epd_fast ? "fast" : "quality");
    unsigned long t0 = millis();
    render_page(mode, false); // 异步推送，不阻塞触摸
    unsigned long t1 = millis();
    progress_save_local(g_cur_book.bookId, g_ch_idx, g_page);
    unsigned long t2 = millis();
    maybe_upload_progress(false);
    Serial.printf("[翻页计时] 渲染+推送 %lu ms, 存进度 %lu ms, 上传 %lu ms\n",
                  t1 - t0, t2 - t1, millis() - t2);
}

// ---------- 字体选择（阅读页内进入，只改正文字体，选中即重排实时看效果） ----------
static std::vector<String> g_fonts; // /font/*.bin 全路径
#define FONT_LIST_Y0 96
#define FONT_ROW_H  48

// 字体列表预览页缓存：整屏 565 位图存 SD，key=字体清单+当前字体（换字体/换选择自动失效）
#define FONT_PAGE_IMG "/weread/cache/fontpage.bin"
#define FONT_PAGE_KEY "/weread/cache/fontpage.key"

static String fontpage_key() {
    String k = "v2|" + g_read_font_path + "|";
    for (auto& f : g_fonts) { k += f; k += ';'; }
    return k;
}

static bool fontpage_try_load(const String& key) {
    if (!storage_sd_ok()) return false;
    File kf = SD.open(FONT_PAGE_KEY, "r");
    if (!kf) return false;
    String old = kf.readString(); kf.close();
    if (old != key) return false; // 字体清单或当前选择变了 → 失效
    File f = SD.open(FONT_PAGE_IMG, "r");
    if (!f) return false;
    const size_t BYTES = (size_t)SCREEN_W * SCREEN_H * 2;
    uint16_t* buf = (uint16_t*)heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) { f.close(); return false; }
    size_t n = f.read((uint8_t*)buf, BYTES);
    f.close();
    if (n != BYTES) { heap_caps_free(buf); return false; }
    canvas.pushImage(0, 0, SCREEN_W, SCREEN_H, buf);
    heap_caps_free(buf);
    epd_flush(epd_mode_t::epd_quality);
    if (g_touch_q) xQueueReset(g_touch_q);
    Serial.println("[字体] 预览页缓存命中");
    return true;
}

static void fontpage_save(const String& key) {
    if (!storage_sd_ok()) return;
    const size_t BYTES = (size_t)SCREEN_W * SCREEN_H * 2;
    uint16_t* buf = (uint16_t*)heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) return;
    canvas.readRect(0, 0, SCREEN_W, SCREEN_H, buf);
    File f = SD.open(FONT_PAGE_IMG, "w");
    if (f) { f.write((uint8_t*)buf, BYTES); f.close(); }
    heap_caps_free(buf);
    File kf = SD.open(FONT_PAGE_KEY, "w");
    if (kf) { kf.print(key); kf.close(); }
    Serial.println("[字体] 预览页已缓存");
}

static void render_fonts() {
    // 预览要逐个加载十几款字体（几秒），先给个即时反馈
    canvas.fillSprite(TFT_WHITE);
    drawUI("加载字体预览...", 24, 400);
    epd_flush(epd_mode_t::epd_fast);

    canvas.fillSprite(TFT_WHITE);
    drawUI("选择正文字体", 20, 16);
    drawUI("返回", SCREEN_W - 20 - textWidthUI("返回"), 16);
    int y = FONT_LIST_Y0;
    EdcFont preview; // 预览专用：逐个加载画行，内存只驻留这一个
    for (size_t i = 0; i < g_fonts.size() && y < SCREEN_H - 60; i++) {
        String name = g_fonts[i];
        int slash = name.lastIndexOf('/');
        if (slash >= 0) name = name.substring(slash + 1);
        bool cur = (g_fonts[i] == g_read_font_path);
        if (cur) drawUI("√", 24, y);
        if (cur) {
            drawRead(name, 60, y); // 当前正文字体已加载，直接用
        } else if (preview.load(g_fonts[i].c_str())) {
            drawTextFont(preview, name, 60, y);
        } else {
            drawUI(name + "（加载失败）", 60, y);
        }
        y += FONT_ROW_H;
    }
    epd_flush(epd_mode_t::epd_quality);
    if (g_touch_q) xQueueReset(g_touch_q);
}

static void show_fonts() {
    g_fonts.clear();
    File dir = SD.open("/font");
    if (dir) {
        File e;
        while ((e = dir.openNextFile())) {
            if (!e.isDirectory()) {
                String n = e.name(); // 注意：e.name() 返回的是裸文件名（不带目录！）
                if (n.endsWith(".bin") && !n.startsWith("._")) // 过滤 macOS 元数据文件
                    g_fonts.push_back("/font/" + n); // 存完整路径，load 才不会失败
            }
            e.close();
        }
        dir.close();
    }
    std::sort(g_fonts.begin(), g_fonts.end());
    Serial.printf("[字体] 发现 %d 个\n", g_fonts.size());
    g_screen = SCR_FONT;
    // 预览页整屏缓存：字体清单+当前选择没变就直接出图（省几秒逐字体渲染）
    String key = fontpage_key();
    if (!fontpage_try_load(key)) {
        render_fonts();
        fontpage_save(key);
    }
}

// 返回阅读页并重绘当前页（字体没变时也可用来退出字体页）
static void back_to_reading() {
    g_screen = SCR_READING;
    render_page(epd_mode_t::epd_quality, true);
    if (g_touch_q) xQueueReset(g_touch_q);
}

static void apply_font(int i) {
    Serial.println("[字体] 切换 → " + g_fonts[i]);
    if (g_read_font.load(g_fonts[i].c_str()) && g_read_font.fontHeight() > 8 && g_read_font.fontHeight() < 200) {
        g_read_font_path = g_fonts[i];
        config_set("font", g_fonts[i]);
        Serial.printf("[字体] 已切换，字高=%d，重新分页\n", g_read_font.fontHeight());
    } else {
        Serial.println("[字体] 加载失败/字高异常，保持原字体");
        g_read_font.load(g_read_font_path.c_str()); // 回滚到原字体（load 失败后对象状态可能脏）
        screen_msg("字体加载失败", g_fonts[i].substring(g_fonts[i].lastIndexOf('/') + 1));
        delay(1500);
    }
    // 正文字体变化 → 版式变化，重新分页并立即回阅读页看效果
    paginate_current();
    g_page = constrain(g_page, 0, (int)g_pages.size() - 1);
    back_to_reading();
}

// ---------- 目录屏（正文左上角点章节名进入；顶部返回正文，底部翻页） ----------
static int g_toc_page = 0;
#define TOC_LIST_Y0 120

static int toc_row_h() { return ui_line_h() + 10; }
static int toc_per_page() { return (SCREEN_H - TOC_LIST_Y0 - 70) / toc_row_h(); }

static void render_toc() {
    canvas.fillSprite(TFT_WHITE);
    drawUI("目录", 20, 16);
    drawUI(truncate_px(g_cur_book.title, 300), 110, 16);
    drawUI("返回", SCREEN_W - 20 - textWidthUI("返回"), 16);

    int per = toc_per_page();
    int total_pages = max(1, ((int)g_chapters.size() + per - 1) / per);
    g_toc_page = constrain(g_toc_page, 0, total_pages - 1);
    int start = g_toc_page * per;
    int end = min(start + per, (int)g_chapters.size());
    int y = TOC_LIST_Y0, lh = toc_row_h();
    for (int i = start; i < end; i++) {
        String line = (i == g_ch_idx ? "→ " : "  ") + truncate_px(g_chapters[i].title, 420);
        drawUI(line, 24, y);
        y += lh;
    }

    // 底部：上一页 / 页码 / 下一页
    drawUI("上一页", 24, SCREEN_H - 44);
    String pg = String(g_toc_page + 1) + "/" + String(total_pages);
    drawUI(pg, (SCREEN_W - textWidthUI(pg)) / 2, SCREEN_H - 44);
    drawUI("下一页", SCREEN_W - 24 - textWidthUI("下一页"), SCREEN_H - 44);
    epd_flush_async(epd_mode_t::epd_fast); // 菜单页用快刷+不阻塞：翻目录时保持点按响应
    // 不重置触摸队列：渲染期间点的会排队补处理（长目录翻页不再"点了没反应"）
}

static void show_toc() {
    if (g_chapters.empty()) return;
    g_toc_page = g_ch_idx / toc_per_page(); // 跳到当前章所在页
    g_screen = SCR_TOC;
    render_toc();
    if (g_touch_q) xQueueReset(g_touch_q); // 仅进入时清一次（防上个界面的残留点按）
}

static void handle_toc_touch(int x, int y) {
    if (y < 70) { back_to_reading(); return; } // 顶部：返回正文
    if (y > SCREEN_H - 70) {                   // 底部：翻页
        int total_pages = max(1, ((int)g_chapters.size() + toc_per_page() - 1) / toc_per_page());
        g_toc_page = constrain(g_toc_page + ((x < SCREEN_W / 2) ? -1 : 1), 0, total_pages - 1);
        render_toc();
        return;
    }
    if (y < TOC_LIST_Y0) return;
    int row = (y - TOC_LIST_Y0) / toc_row_h();
    int idx = g_toc_page * toc_per_page() + row;
    if (row < toc_per_page() && idx < (int)g_chapters.size()) {
        Serial.printf("[触摸] 目录选章 %d. %s\n", idx + 1, g_chapters[idx].title.c_str());
        open_chapter(idx);
    }
}

// ---------- 触摸 ----------
static void handle_shelf_touch(int x, int y) {
    // 顶部标题行：右上角 睡眠/WiFi/登录 入口（从右往左：登录 WiFi 睡眠）
    if (y < 60) {
        int lw = textWidthUI("登录");
        int ww = textWidthUI("WiFi");
        int sw = textWidthUI("睡眠");
        if (x >= SCREEN_W - 28 - lw) {
            Serial.println("[触摸] 重新登录");
            shelf_relogin();
        } else if (x >= SCREEN_W - 28 - lw - 24 - ww - 8) {
            Serial.println("[触摸] 进入配网门户");
            run_provisioning_portal(screen_msg, ""); // 永不返回（成功后重启）
        } else if (x >= SCREEN_W - 28 - lw - 24 - ww - 24 - sw - 8) {
            Serial.println("[触摸] 休眠");
            enter_sleep();
        }
        return;
    }
    // 底部翻页区
    if (y > SCREEN_H - 70) {
        g_shelf_page += (x < SCREEN_W / 2) ? -1 : 1;
        show_shelf();
        Serial.printf("[触摸] 书架翻页 → %d\n", g_shelf_page + 1);
        return;
    }
    // 行 → 书籍详情
    int row = (y - 60) / shelf_row_h();
    int idx = g_shelf_page * SHELF_PER_PAGE + row;
    if (row < SHELF_PER_PAGE && idx < (int)g_shelf.size()) {
        Serial.printf("[触摸] 详情 %d. %s\n", idx + 1, g_shelf[idx].title.c_str());
        open_book_detail(idx);
    }
}

static void handle_book_touch(int x, int y) {
    if (y < 60) { // 顶部：返回书架
        Serial.println("[触摸] 返回书架");
        show_shelf();
        return;
    }
    if (y >= SCREEN_H - 130 && y <= SCREEN_H - 64) { // 底部按钮
        if (x >= 30 && x <= 250)  { Serial.println("[触摸] 继续阅读"); continue_reading(); return; }
        if (x >= 290 && x <= 510) { Serial.println("[触摸] 下载整本"); download_all(); return; }
    }
    if (y > SCREEN_H - 64 && y <= SCREEN_H - 6 && x >= 30 && x <= 250) { // 插图开关
        bool now = !book_img_off(g_cur_book.bookId);
        book_img_off_set(g_cur_book.bookId, now);
        Serial.printf("[触摸] 插图开关 → %s\n", now ? "关" : "开");
        render_book_detail();
        return;
    }
}

static void handle_reading_touch(int x, int y) {
    if (y < 70) { // 顶部：左=目录，中=书架，右=字体
        if (x < 180) {
            Serial.println("[触摸] 目录");
            show_toc();
        } else if (x < 400) {
            Serial.println("[触摸] 返回书架");
            if (storage_sd_ok()) SD.remove("/weread/state.json"); // 显式退出阅读：清掉恢复标记
            show_shelf();
        } else {
            Serial.println("[触摸] 字体选择");
            show_fonts();
        }
        return;
    }
    bool next = (x >= SCREEN_W / 2); // 左半屏=上一页，右半屏=下一页
    Serial.printf("[触摸] %s\n", next ? "下一页" : "上一页");
    turn_page(next ? 1 : -1);
}

static void handle_font_touch(int x, int y) {
    if (y < 60) { back_to_reading(); return; } // 顶部：返回阅读页
    int row = (y - FONT_LIST_Y0) / FONT_ROW_H;
    if (row >= 0 && row < (int)g_fonts.size()) apply_font(row);
}

// ---------- 书架页"登录"入口：登录成功后重新拉书架 ----------
static void shelf_relogin() {
    if (do_qr_login()) {
        String err;
        net_lock(); // g_shelf 重写与后台任务读取互斥
        g_shelf.clear();
        bool ok = weread_api::getBookshelf(g_shelf, err) && !g_shelf.empty();
        net_unlock();
        if (ok) {
            Serial.printf("[书架] %d 本\n", g_shelf.size());
            sort_shelf_recent();
        } else {
            screen_msg("书架获取失败", err, "点按继续");
            wait_tap(60000);
        }
    }
    show_shelf();
}

// ---------- 串口配置写入 ----------
// 通过串口发送一行 JSON 写配置文件（SD 优先），格式：
//   CFG {"wifi_ssid":"..","wifi_pass":"..","wr_vid":"..","wr_skey":"..","wr_rt":"..","api_key":""}
static bool try_serial_config() {
    if (!Serial.available()) return false;
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (!line.startsWith("CFG ")) return false;
    String json = line.substring(4);
    File f = open_config_write();
    if (!f) { Serial.println("[cfg] 写入失败"); return false; }
    f.print(json); f.close();
    Serial.println("[cfg] 已写入配置文件，重新加载...");
    return WR.loadConfig();
}

// 拉正文进度屏：接口层每个阶段（reader/e_0/e_1/e_3）打点触发
// 注意：打点也可能来自后台预取任务（net_worker）——只有主任务拉取时才允许画屏，否则两个任务共画一张画布会崩
static void chapter_stage_ui(const char* stage, int cur, int total) {
    if (!g_stage_ui_ok) return;
    canvas.fillSprite(TFT_WHITE);
    drawUIBold("拉取正文...", 24, 180);
    drawUI(truncate_px(g_title, 400), 24, 260);
    // 简易进度条 + 阶段名
    int bw = SCREEN_W - 48;
    canvas.drawRect(24, 330, bw, 22, TFT_BLACK);
    canvas.fillRect(26, 332, (int)((long)(bw - 4) * cur / total), 18, TFT_BLACK);
    drawUI(String(stage) + "  " + cur + "/" + total, 24, 380);
    epd_flush(epd_mode_t::epd_fast);
}

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== PaperS3 微信读书 ===");
    weread_api::chapter_stage_cb = chapter_stage_ui; // 拉正文进度打点 → 屏幕进度条

    // mbedTLS 内存分配改走 PSRAM（DRAM 长期近满导致 mbedtls_ssl_setup -0x7F00，
    // 简介/评论等请求因此发不出去；PSRAM 8MB 足够。性能影响可忽略，失败回退内部）
    mbedtls_platform_set_calloc_free(
        [](size_t n, size_t size) -> void* {
            void* p = heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
            if (!p) p = malloc(n * size);
            if (p) memset(p, 0, n * size);
            return p;
        },
        heap_caps_free);

    M5.Display.setRotation(0); // 竖屏 540x960（rotation=2 倒置 180°，0 为正立阅读方向）
    M5.Display.setEpdMode(epd_mode_t::epd_quality);
    // 关键：EPD 必须先 clear + 全屏刷新一次，否则 frame buffer 是上电随机值
    M5.Display.clear(TFT_WHITE);
    M5.Display.display();
    M5.Display.waitDisplay();
    canvas.setColorDepth(16);
    canvas.createSprite(SCREEN_W, SCREEN_H);
    // 首屏推迟到字体加载完之后（一次就好，不刷两遍）

    // 触摸采集任务（10ms 高频，渲染/刷新期间也不丢点按）
    g_touch_q = xQueueCreate(8, sizeof(TouchPoint));
    xTaskCreatePinnedToCore(touch_task, "touch", 4096, NULL, 2, NULL, 1);

    // USB 插入检测（GPIO5，UserDemo 同款）：接着 USB 时禁止自动休眠
    gpio_reset_pin(GPIO_NUM_5);
    gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT);

    // 后台网络任务（预取下一章/相邻页封面/进度上传）与互斥锁
    g_net_mtx = xSemaphoreCreateRecursiveMutex();
    g_upload_q = xQueueCreate(4, sizeof(UploadJob));
    g_prefetch_q = xQueueCreate(1, sizeof(PrefetchJob)); // 长度 1，配合 xQueueOverwrite 只留最新目标
    g_coverpf_q = xQueueCreate(1, sizeof(int));
    g_imgpre_q = xQueueCreate(1, sizeof(int));
    xTaskCreatePinnedToCore(net_worker, "netjob", 12288, NULL, 1, NULL, 1);

    // 探测 SD 卡；配置/缓存/字体都依赖它（storage 层配置优先 SD，缺失回退 SPIFFS）
    // PaperS3 SD 用 SPI 模式：CS=47, SCK=39, MOSI=38, MISO=40
    {
        static SPIClass sdSPI(FSPI);
        sdSPI.begin(39 /*SCK*/, 40 /*MISO*/, 38 /*MOSI*/, 47 /*CS*/);
        if (SD.begin(47 /*CS*/, sdSPI, 40000000)) {
            Serial.printf("[SD] 挂载成功(SPI) 容量 %llu MB 已用 %llu MB\n",
                          SD.totalBytes() / 1048576, SD.usedBytes() / 1048576);
            storage_note_sd(true);
            SD.mkdir(SD_WEREAD_DIR);
            SD.mkdir(SD_CACHE_DIR);
            img_render::begin();
        } else {
            Serial.println("[SD] SPI 挂载失败（无卡或引脚/卡问题）");
            storage_note_sd(false);
        }
    }

    if (!SPIFFS.begin(true)) { screen_msg("SPIFFS 挂载失败"); Serial.println("SPIFFS fail"); return; }

    // 先读配置，再加载双字体：UI 固定文楷（保证菜单永远可显示），正文用配置字体（失败回滚文楷）
    WR.loadConfig();
    if (storage_sd_ok()) {
        if (g_ui_font.load(UI_FONT_PATH)) {
            Serial.printf("[font] UI %s 字高=%d\n", UI_FONT_PATH, g_ui_font.fontHeight());
        } else {
            Serial.println("[font] UI 字体加载失败（菜单将缺中文）");
        }
        g_read_font_path = WR.cfg_font.length() ? WR.cfg_font : String(UI_FONT_PATH);
        if (!g_read_font.load(g_read_font_path.c_str()) ||
            g_read_font.fontHeight() < 8 || g_read_font.fontHeight() > 200) {
            Serial.println("[font] 正文字体加载失败/异常，回滚文楷");
            g_read_font.load(UI_FONT_PATH);
            g_read_font_path = UI_FONT_PATH;
        }
        Serial.printf("[font] 正文 %s 字高=%d\n", g_read_font_path.c_str(), g_read_font.fontHeight());
    }
    show_splash(); // 唯一一次首屏：图标+标题（字体已就绪）

    // ---- WiFi 配置检查：无配置给 5 秒串口 CFG 窗口（首屏保持），然后进配网门户 ----
    bool have_cfg = WR.wifi_ssid.length() && WR.hasValidCookie();
    if (!have_cfg) {
        Serial.println("[提示] 无有效配置，5 秒内可串口发 CFG {...}");
        unsigned long t0 = millis();
        bool got = false;
        while (millis() - t0 < 5000) {
            if (try_serial_config() && WR.wifi_ssid.length() && WR.hasValidCookie()) {
                got = true; break;
            }
            delay(50);
        }
        if (!got) run_provisioning_portal(screen_msg, "未配置 WiFi"); // 永不返回（成功后重启）
    } else {
        Serial.printf("[cfg] 已加载保存的配置 ssid=%s\n", WR.wifi_ssid.c_str());
    }

    splash_status("正在连接 WiFi...");
    if (!WR.connectWiFi()) {
        Serial.println("[错误] WiFi 连接失败");
        run_provisioning_portal(screen_msg, "连接超时/密码错误"); // 永不返回（成功后重启）
    }
    configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // 校时（进度上传签名用 ts）

    // 主动续期：wr_skey 是短效令牌（几小时过期），有 wr_rt 就能换新，不用天天扫码
    if (WR.hasValidCookie()) {
        if (WR.tryRenew()) Serial.println("[cfg] 令牌已续期");
        else Serial.println("[cfg] 续期失败（若书架也 401 才需要重扫）");
    }

    // ---- 拉书架（兼 cookie 有效性检查；过期先续期，续不动再扫码登录）----
    splash_status("正在加载书架...");
    String err;
    if (!weread_api::getBookshelf(g_shelf, err) || g_shelf.empty()) {
        if (err.indexOf("-2012") >= 0) {
            Serial.println("[login] cookie 过期(-2012)，先试 renewal 续期");
            if (WR.tryRenew()) {
                err = "";
                splash_status("正在加载书架...");
                weread_api::getBookshelf(g_shelf, err); // 续期成功重拉
            }
        }
        if (err.indexOf("-2012") >= 0) { // 续期也不行（wr_rt 死了）→ 扫码
            Serial.println("[login] 续期失败，进入扫码登录");
            if (do_qr_login()) {
                err = "";
                splash_status("正在加载书架...");
                if (!weread_api::getBookshelf(g_shelf, err) || g_shelf.empty()) {
                    screen_msg("书架获取失败", err, "点按屏幕重启");
                    wait_tap(600000);
                    ESP.restart();
                }
            } else {
                screen_msg("登录未完成", "点按屏幕重启");
                wait_tap(600000);
                ESP.restart();
            }
        } else if (err.length() || g_shelf.empty()) {
            screen_msg("书架获取失败", err, "点按屏幕重启");
            wait_tap(600000);
            ESP.restart();
        }
    }
    Serial.printf("[书架] %d 本\n", g_shelf.size());
    sort_shelf_recent();
    for (size_t i = 0; i < g_shelf.size(); i++) // 排序后打印，串口序号与屏幕一致
        Serial.printf("  %d. %s [%s] 进度%d%%\n", i + 1, g_shelf[i].title.c_str(), g_shelf[i].bookId.c_str(), g_shelf[i].progress);
    if (g_touch_q) xQueueReset(g_touch_q); // 清掉启动期间的点按
    if (!try_restore_reading()) show_shelf(); // 有睡眠标记则直接回上次阅读页
    g_last_activity = millis();
}

void loop() {
    // 触摸事件（由 touch_task 高频采集，翻页渲染/刷新期间也不丢）
    TouchPoint pt;
    if (g_touch_q && xQueueReceive(g_touch_q, &pt, 0)) {
        g_last_activity = millis();
        switch (g_screen) {
            case SCR_READING: handle_reading_touch(pt.x, pt.y); break;
            case SCR_BOOK:    handle_book_touch(pt.x, pt.y); break;
            case SCR_FONT:    handle_font_touch(pt.x, pt.y); break;
            case SCR_TOC:     handle_toc_touch(pt.x, pt.y); break;
            default:          handle_shelf_touch(pt.x, pt.y); break;
        }
    }

    // 串口指令
    if (Serial.available()) {
        g_last_activity = millis();
        String line = Serial.readStringUntil('\n');
        line.trim();
        if (line.startsWith("CFG ")) {
            File f = open_config_write();
            if (f) { f.print(line.substring(4)); f.close(); Serial.println("[cfg] 已写入"); WR.loadConfig(); }
            return;
        }
        // 翻页/返回/功能入口（串口等价触摸，调试用）
        if (line == "n") { turn_page(1); return; }
        if (line == "p") { turn_page(-1); return; }
        if (line == "s") { show_shelf(); return; }
        if (line == "t") { show_toc(); return; }   // 目录屏
        if (line == "tp") { g_toc_page++; render_toc(); return; } // 目录下一页
        if (line == "tm") { g_toc_page--; render_toc(); return; } // 目录上一页
        if (line.startsWith("ts ")) { // 目录选章 N(1 起)
            int i = line.substring(3).toInt();
            if (i >= 1 && i <= (int)g_chapters.size()) open_chapter(i - 1);
            return;
        }
        if (line == "dl") { download_all(); return; } // 下载整本（测试用）
        if (line == "sp") { g_shelf_page++; show_shelf(); return; }  // 书架下一页
        if (line == "sm") { g_shelf_page--; show_shelf(); return; }  // 书架上一页
        if (line == "wifi") { run_provisioning_portal(screen_msg, ""); return; } // 进配网门户
        if (line == "login") { shelf_relogin(); return; }                        // 进扫码登录
        if (line.startsWith("netdiag")) { // 分层网络诊断：不发 cookie，不打印用户内容
            int rounds = line.substring(7).toInt();
            if (rounds <= 0) rounds = 6;
            net_lock();
            network_diag::run((unsigned)rounds);
            net_unlock();
            return;
        }
        if (line.startsWith("netapi")) { // 生产 API 路径压测：发现有 cookie 但只输出脱敏统计
            int rounds = line.substring(6).toInt();
            if (rounds <= 0) rounds = 10;
            net_lock();
            network_diag::run_api((unsigned)rounds);
            net_unlock();
            return;
        }
        if (line == "cookie") { // 导出当前 cookie（调试用，供电脑端脚本测试接口）
            Serial.printf("COOKIE wr_vid=%s wr_skey=%s wr_rt=%s\n",
                          WR.getCookie("wr_vid").c_str(), WR.getCookie("wr_skey").c_str(), WR.getCookie("wr_rt").c_str());
            return;
        }
        if (line == "usb") { // 诊断：USB/充电状态（自动休眠门控用）
            Serial.printf("[usb] GPIO5(USB_DET)=%d isCharging=%d\n",
                          gpio_get_level(GPIO_NUM_5), (int)M5.Power.isCharging());
            return;
        }
        if (line == "books") { // 打印当前书架（测试用）
            for (size_t i = 0; i < g_shelf.size(); i++)
                Serial.printf("  %d. %s [%s]\n", i + 1, g_shelf[i].title.c_str(), g_shelf[i].bookId.c_str());
            return;
        }
        if (line == "imgoff") { // 切换当前书插图开关（卡下载时解围用）
            bool now = !book_img_off(g_cur_book.bookId);
            book_img_off_set(g_cur_book.bookId, now);
            Serial.printf("[img] 插图开关 → %s\n", now ? "关" : "开");
            if (g_screen == SCR_READING && !g_pages.empty()) render_page(epd_mode_t::epd_quality, true);
            return;
        }
        if (line == "imgs") { // 导出当前章图片块 URL（图片问题调试用）
            int i = 0;
            for (auto& b : g_blocks) if (b.is_image) Serial.printf("[img块 %d] %s\n", ++i, b.text.c_str());
            Serial.printf("[img块] 共 %d 个\n", i);
            return;
        }
        if (line == "fonts") { // 诊断：列出 /font/*.bin 并逐个试加载
            File dir = SD.open("/font");
            if (!dir) { Serial.println("[fonts] /font 目录打不开"); return; }
            File e;
            while ((e = dir.openNextFile())) {
                String n = e.name();
                if (e.isDirectory() || !n.endsWith(".bin")) { e.close(); continue; }
                // 读 5 字节头：u32 char_count + u8 font_height
                uint8_t hdr[5] = {0};
                int rn = e.read(hdr, 5);
                uint32_t cc = rn == 5 ? (hdr[0] | (hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24)) : 0;
                Serial.printf("[fonts] %s 大小=%d 头=%d 字符数=%u 字高=%d ",
                              n.c_str(), (int)e.size(), rn, cc, rn == 5 ? hdr[4] : -1);
                e.close();
                // 试加载到正文字体（不改 g_read_font_path，纯验证）
                EdcFont t;
                bool ok = t.load(n.c_str());
                Serial.printf("→ load=%s\n", ok ? "OK" : "失败");
            }
            dir.close();
            return;
        }
        if (line.startsWith("font ")) { // font N：切到第 N 个字体（同字体页点选）
            if (g_fonts.empty()) show_fonts();
            int i = line.substring(5).toInt();
            if (i >= 1 && i <= (int)g_fonts.size() && g_book_idx >= 0) apply_font(i - 1);
            return;
        }
        // 选书：N = 详情页；N:C = 直读第 C 章（跳过详情，调试用）
        int colon = line.indexOf(':');
        int n = line.toInt();
        int chIdx = (colon > 0) ? line.substring(colon + 1).toInt() : 1;
        if (n >= 1 && n <= (int)g_shelf.size()) {
            if (colon > 0) open_book(n - 1, chIdx);
            else open_book_detail(n - 1);
        }
    }

    // 闲置自动休眠（充电中=接着 USB 就不睡；浅睡不返回；失败兜底会回来继续跑）
    bool usb_in = (M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging);
    if (millis() - g_last_activity > AUTO_SLEEP_MS && !usb_in) enter_sleep();

    // keep-alive 会话闲置关闭（释放 DRAM，防后续请求 TLS 内存不足 -0x7F00）
    static unsigned long last_hk = 0;
    if (millis() - last_hk > 5000) {
        last_hk = millis();
        weread_api::housekeeping();
        img_render::housekeeping();
    }

    // cookie 被服务器轮换后定期落盘（否则重启读旧 cookie → -2012 又要扫码）
    static unsigned long last_ck_save = 0;
    if (WR.cookiesDirty() && millis() - last_ck_save > 60000) {
        last_ck_save = millis();
        WR.saveCookiesToConfig();
        WR.clearCookiesDirty();
        Serial.println("[cfg] 轮换 cookie 已写回");
    }

    // 定期续期已移除（见 request 层自动续期）：浅睡时主循环冻结不会跑这里，无唤醒耗电问题
    delay(5);
}

// ESP-IDF 兼容入口：创建独立任务运行 setup/loop
#ifdef __cplusplus
extern "C"
{
#endif
    static void MainTask(void *pvParameters)
    {
        setup();
        while (true) {
            loop();
        }
    }

    void app_main(void)
    {
        BaseType_t rc = xTaskCreatePinnedToCore(MainTask, "MainTask", 32768, NULL, 1, NULL, 1);
        (void)rc;
    }
#ifdef __cplusplus
}
#endif
