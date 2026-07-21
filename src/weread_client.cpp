// weread_client.cpp — 实现
#include "weread_client.h"
#include "config.h"
#include "storage.h"
#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <nvs_flash.h>
#include <esp_heap_caps.h>

WereadClient WR;

// ---- HttpResponse 所有权实现 ----
HttpResponse::~HttpResponse() { if (psbody) heap_caps_free(psbody); }
HttpResponse::HttpResponse(HttpResponse&& o) noexcept
    : status(o.status), body(std::move(o.body)), headers(std::move(o.headers)),
      error(std::move(o.error)), psbody(o.psbody), pslen(o.pslen) {
    o.psbody = nullptr; o.pslen = 0;
}
HttpResponse& HttpResponse::operator=(HttpResponse&& o) noexcept {
    if (this != &o) {
        if (psbody) heap_caps_free(psbody);
        status = o.status; body = std::move(o.body); headers = std::move(o.headers);
        error = std::move(o.error); psbody = o.psbody; pslen = o.pslen;
        o.psbody = nullptr; o.pslen = 0;
    }
    return *this;
}

// esp_http_client 事件回调里累积响应体
// 大响应写入 PSRAM buffer（String 在 ESP32 上超 ~64KB 有 bug，长度虚高内容空洞）
struct RespBuf {
    String* body;
    std::map<String,String>* headers;
    char* psbuf = nullptr;   // PSRAM 接收缓冲
    size_t pslen = 0;        // 已写入长度
    size_t pscap = 0;        // 缓冲容量
    bool overflow = false;   // 扩容到顶仍放不下 → 数据被截断，不能当成功响应用
};

// 接收缓冲：初始 512KB，不够按需翻倍，上限 4MB（PSRAM 共 8MB）
// 旧版固定 512KB 静默丢字节 → 大书架（几百本书）JSON 被齐截，解析必报 IncompleteInput
#define HTTP_BUF_INIT (512 * 1024)
#define HTTP_BUF_MAX  (4 * 1024 * 1024)

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    RespBuf* rb = (RespBuf*)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb && evt->data_len > 0) {
                // 优先写 PSRAM buffer
                if (rb->psbuf) {
                    size_t need = rb->pslen + evt->data_len;
                    if (need > rb->pscap) { // 放不下 → 翻倍扩容（封顶 HTTP_BUF_MAX）
                        size_t newcap = rb->pscap;
                        while (newcap < need && newcap < HTTP_BUF_MAX) newcap *= 2;
                        if (newcap > HTTP_BUF_MAX) newcap = HTTP_BUF_MAX;
                        if (newcap >= need) {
                            char* nb = (char*)heap_caps_realloc(rb->psbuf, newcap, MALLOC_CAP_SPIRAM);
                            if (nb) {
                                Serial.printf("[http] 接收缓冲扩容 %u→%u KB\n",
                                              (unsigned)(rb->pscap / 1024), (unsigned)(newcap / 1024));
                                rb->psbuf = nb; rb->pscap = newcap;
                            } else {
                                Serial.printf("[http] PSRAM 扩容到 %u KB 失败\n", (unsigned)(newcap / 1024));
                            }
                        }
                    }
                    if (rb->pslen + evt->data_len <= rb->pscap) {
                        memcpy(rb->psbuf + rb->pslen, evt->data, evt->data_len);
                        rb->pslen += evt->data_len;
                    } else {
                        rb->overflow = true; // 到顶仍放不下：宁可报错，绝不静默截断
                    }
                } else if (rb->body) {
                    rb->body->concat((const char*)evt->data, evt->data_len);
                }
            }
            break;
        case HTTP_EVENT_ON_HEADER:
            if (rb && rb->headers && evt->header_key && evt->header_value) {
                String k = evt->header_key; k.toLowerCase();
                String v = evt->header_value;
                // set-cookie 可能多条，用换行拼接
                if (k == "set-cookie" && rb->headers->count("set-cookie"))
                    (*rb->headers)["set-cookie"] += "\n" + v;
                else
                    (*rb->headers)[k] = v;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

bool WereadClient::loadConfig() {
    if (!SPIFFS.begin(true)) return false;
    File f = open_config_read(); // SD 优先，缺失回退 SPIFFS
    if (!f) {
        Serial.println("[cfg] 配置文件不存在（SD/SPIFFS），请通过串口或配网门户写入");
        return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { Serial.printf("[cfg] JSON 解析失败: %s\n", err.c_str()); return false; }

    wifi_ssid   = doc["wifi_ssid"] | "";
    wifi_pass   = doc["wifi_pass"] | "";
    cfg_wr_vid  = doc["wr_vid"]  | "";
    cfg_wr_skey = doc["wr_skey"] | "";
    cfg_wr_rt   = doc["wr_rt"]   | "";
    cfg_api_key = doc["api_key"] | "";
    cfg_font    = doc["font"]    | "";

    if (cfg_wr_vid.length())  cookies_["wr_vid"]  = cfg_wr_vid;
    if (cfg_wr_skey.length()) cookies_["wr_skey"] = cfg_wr_skey;
    if (cfg_wr_rt.length())   cookies_["wr_rt"]   = cfg_wr_rt;
    return true;
}

bool WereadClient::connectWiFi(unsigned long timeout_ms) {
    if (wifi_ssid.isEmpty()) { Serial.println("[wifi] 未配置 ssid"); return false; }

    // 关键：WiFi 校准数据存 NVS，必须先初始化（M5ReadPaper 验证）
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[nvs] 无空闲页/版本变更，erase 后重试");
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }
    if (nvs_ret != ESP_OK) {
        Serial.printf("[nvs] 初始化失败: %d\n", nvs_ret);
    }

    WiFi.disconnect(true, false); // 复位旧状态（wifioff=true, 不清已存AP）
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);         // 关 WiFi 省电模式：ps 模式会吞包/中止连接（TCP 握手被 abort 的常见根因）
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());
    Serial.printf("[wifi] 连接 %s", wifi_ssid.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeout_ms) {
        delay(300); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] 已连接 IP=%s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
        return true;
    }
    Serial.println("[wifi] 连接超时");
    return false;
}

void WereadClient::setCookie(const String& name, const String& value) {
    if (value.length()) cookies_[name] = value;
    if (name == "wr_rt") rt_dead_ = false; // 新 rt 到手（扫码登录/服务器轮换）即复活
}

String WereadClient::getCookie(const String& name) const {
    auto it = cookies_.find(name);
    return it == cookies_.end() ? String("") : it->second;
}

bool WereadClient::saveCookiesToConfig() {
    // 读出原有配置（保留 wifi_ssid/wifi_pass/api_key 等），只更新 wr_* 三个字段
    JsonDocument doc;
    File f = open_config_read();
    if (f) { deserializeJson(doc, f); f.close(); }
    doc["wr_vid"]  = getCookie("wr_vid");
    doc["wr_skey"] = getCookie("wr_skey");
    doc["wr_rt"]   = getCookie("wr_rt");
    File w = open_config_write();
    if (!w) { Serial.println("[cfg] 保存 cookie 失败：无法打开配置文件"); return false; }
    serializeJson(doc, w);
    w.close();
    Serial.printf("[cfg] cookie 已保存到 %s\n", storage_sd_ok() ? "SD 卡" : "SPIFFS");
    return true;
}

String WereadClient::cookieHeader() const {
    String h;
    for (auto& kv : cookies_) {
        if (h.length()) h += "; ";
        h += kv.first + "=" + kv.second;
    }
    return h;
}

bool WereadClient::hasValidCookie() const {
    return cookies_.count("wr_vid") && cookies_.count("wr_skey");
}

void WereadClient::saveCookies() {
    File f = SPIFFS.open("/cookies.txt", "w");
    if (!f) return;
    for (auto& kv : cookies_) f.println(kv.first + "=" + kv.second);
    f.close();
}

void WereadClient::absorbSetCookie(const HttpResponse& resp) {
    auto it = resp.headers.find("set-cookie");
    if (it == resp.headers.end()) return;
    // 逐行解析 name=value（只取第一个分号前的部分）
    int start = 0;
    String all = it->second;
    while (start < (int)all.length()) {
        int nl = all.indexOf('\n', start);
        if (nl < 0) nl = all.length();
        String line = all.substring(start, nl);
        int eq = line.indexOf('=');
        int semi = line.indexOf(';');
        if (eq > 0) {
            String name = line.substring(0, eq); name.trim();
            String val = (semi > eq) ? line.substring(eq + 1, semi) : line.substring(eq + 1);
            val.trim();
            // 只关心 weread 的会话字段，避免膨胀
            if (name.startsWith("wr_")) {
                // 会话关键字段被服务器轮换（值变化）→ 置脏，主循环定期写回配置文件
                if ((name == "wr_skey" || name == "wr_rt" || name == "wr_vid") &&
                    cookies_.count(name) && cookies_[name] != val) {
                    cookies_dirty_ = true;
                }
                setCookie(name, val);
            }
        }
        start = nl + 1;
    }
}

// ---- keep-alive 会话（普通 API 用，与 ShardSession 同规）----
// host 固定 weread.qq.com：书架/目录/详情/评论/续期/进度上传全走这一条 TLS 连接，
// 握手从每请求 1 次（1-2s）降到整会话 1 次；失败 close 重建重试一次。

// 会话响应缓冲：event_handler 的 user_data 在 init 时绑定无法按请求改，
// 故用静态固定缓冲，每次请求前重置，成功后转所有权（与 ShardSession 同规）。
static RespBuf g_sess_rb;

bool WereadClient::sessOpen_() {
    if (sess_) return true;
    esp_http_client_config_t cfg = {};
    cfg.host = "weread.qq.com";
    cfg.path = "/";
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.keep_alive_enable = true;
    cfg.event_handler = http_event_handler; // 直接用主 handler（读 evt->user_data）
    cfg.user_data = &g_sess_rb;             // init 时绑定，之后不可改
    sess_ = esp_http_client_init(&cfg);
    return sess_ != nullptr;
}

void WereadClient::sessClose_() {
    if (sess_) { esp_http_client_cleanup(sess_); sess_ = nullptr; }
}

void WereadClient::housekeeping() {
    if (sess_ && sess_in_use_ == 0 && millis() - sess_last_use_ > 30000) {
        Serial.println("[wr] 会话闲置 30s，关闭释放内存");
        sessClose_();
    }
}

// 会话路径单次请求（不_retry）；path 为相对路径（如 "/web/shelf/sync"）
HttpResponse WereadClient::requestSess_(const String& method, const String& path, const String* body,
                                        const String& referer, const String& contentType) {
    HttpResponse resp;
    struct UseGuard { volatile int& c; UseGuard(volatile int& c_) : c(c_) { c++; } ~UseGuard() { c--; } } guard(sess_in_use_);

    if (!sessOpen_()) { resp.error = "sess init failed"; resp.status = -1; return resp; }

    // 重置会话缓冲（headers/PSRAM 指针指到当次 resp）
    g_sess_rb.body = &resp.body;
    g_sess_rb.headers = &resp.headers;
    g_sess_rb.pslen = 0;
    g_sess_rb.overflow = false;
    if (!g_sess_rb.psbuf) {
        g_sess_rb.psbuf = (char*)heap_caps_malloc(HTTP_BUF_INIT, MALLOC_CAP_SPIRAM);
        if (!g_sess_rb.psbuf) { resp.error = "PSRAM 分配失败"; resp.status = -1; return resp; }
        g_sess_rb.pscap = HTTP_BUF_INIT;
    }

    esp_http_client_set_url(sess_, path.c_str());
    esp_http_client_set_method(sess_, method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(sess_, "User-Agent", WEREAD_USER_AGENT);
    esp_http_client_set_header(sess_, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(sess_, "Origin", "https://weread.qq.com");
    esp_http_client_set_header(sess_, "Referer", referer.c_str());
    String cookie = cookieHeader();
    if (cookie.length()) esp_http_client_set_header(sess_, "Cookie", cookie.c_str());
    if (body) {
        esp_http_client_set_header(sess_, "Content-Type", contentType.c_str());
        esp_http_client_set_post_field(sess_, body->c_str(), body->length());
    } else {
        esp_http_client_set_post_field(sess_, nullptr, 0); // 清掉上次 POST body
    }

    esp_err_t err = esp_http_client_perform(sess_);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(sess_);
    } else {
        resp.error = String(esp_err_to_name(err));
        resp.status = -1;
    }
    sess_last_use_ = millis();

    if (g_sess_rb.overflow) {
        Serial.printf("[wr] 响应超 %u KB 上限被截断 path=%s\n",
                      (unsigned)(g_sess_rb.pscap / 1024), path.c_str());
        g_sess_rb.overflow = false;
        resp.error = "response too large";
        resp.status = -1;
    }
    // 成功收到数据：转所有权给 resp（g_sess_rb.psbuf 置空，下次请求重新分配）
    if (resp.status > 0 && g_sess_rb.psbuf && g_sess_rb.pslen > 0) {
        if (g_sess_rb.pslen < g_sess_rb.pscap) g_sess_rb.psbuf[g_sess_rb.pslen] = '\0';
        resp.psbody = g_sess_rb.psbuf;
        resp.pslen = g_sess_rb.pslen;
        g_sess_rb.psbuf = nullptr;
        g_sess_rb.pscap = 0;
    }
    absorbSetCookie(resp);
    return resp;
}

// 一次性独立 client（fallback：异 host 如 i.weread.qq.com，或会话重建后仍失败的兜底）
HttpResponse WereadClient::requestOnce_(const String& method, const String& url, const String* body,
                                        const String& referer, const String& contentType) {
    HttpResponse resp;
    RespBuf rb;
    rb.body = &resp.body;
    rb.headers = &resp.headers;

    char* psbuf = (char*)heap_caps_malloc(HTTP_BUF_INIT, MALLOC_CAP_SPIRAM);
    if (psbuf) { rb.psbuf = psbuf; rb.pscap = HTTP_BUF_INIT; rb.pslen = 0; }

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &rb;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { resp.error = "init failed"; if (psbuf) heap_caps_free(psbuf); return resp; }

    if (method == "POST") esp_http_client_set_method(client, HTTP_METHOD_POST);
    else esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
    esp_http_client_set_header(client, "Referer", referer.c_str());
    String cookie = cookieHeader();
    if (cookie.length()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
    if (body) {
        esp_http_client_set_header(client, "Content-Type", contentType.c_str());
        esp_http_client_set_post_field(client, body->c_str(), body->length());
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
    } else {
        resp.error = String(esp_err_to_name(err));
        resp.status = -1;
    }
    esp_http_client_cleanup(client);

    if (rb.overflow) {
        Serial.printf("[wr] 响应超 %u KB 上限被截断 url=%s\n",
                      (unsigned)(rb.pscap / 1024), url.c_str());
        resp.error = "response too large";
        resp.status = -1;
    }
    if (rb.psbuf && rb.pslen > 0) {
        if (rb.pslen < rb.pscap) rb.psbuf[rb.pslen] = '\0';
        resp.psbody = rb.psbuf;
        resp.pslen = rb.pslen;
    } else if (rb.psbuf) {
        heap_caps_free(rb.psbuf);
    }
    absorbSetCookie(resp);
    return resp;
}

HttpResponse WereadClient::request(const String& method, const String& url, const String* body,
                                   const String& referer, const String& contentType) {
    // 自动路由：weread.qq.com 走 keep-alive 会话；异 host 走一次性 client
    static const char* WEB_PREFIX = "https://weread.qq.com";
    bool use_sess = url.startsWith(WEB_PREFIX);

    auto do_once = [&](const String& m, const String& u, const String* b,
                       const String& ref, const String& ct) -> HttpResponse {
        if (use_sess) {
            String path = u.substring(strlen(WEB_PREFIX)); // "/web/..." 部分
            if (!path.length()) path = "/";
            HttpResponse r = requestSess_(m, path, b, ref, ct);
            if (r.status == -1) { // 连接级失败：丢弃重建再试一次（同 ShardSession）
                Serial.println("[wr] 会话请求失败，重建连接重试");
                sessClose_();
                r = requestSess_(m, path, b, ref, ct);
            }
            return r;
        }
        return requestOnce_(m, u, b, ref, ct);
    };

    HttpResponse resp = do_once(method, url, body, referer, contentType);

    // 自动续期：wr_skey 短效（几小时过期），任何请求吃到 -2012 就用 wr_rt 续期并重试一次。
    // 只在响应小（错误页都几十字节）且含 -2012 时触发；renewal 自身/近期续期失败不触发（防抖）。
    // 防抖时长由 tryRenew 按失败类型设置（传输失败 30s，rt 死亡 10 分钟）；
    // last_renew_fail_ 为 0 表示从未失败过——开机首次 -2012 必须放行（旧代码此处有 bug，开机 10 分钟内不续期）
    if (resp.ok() && !renewing_ && hasValidCookie() && resp.length() < 2048 &&
        memmem(resp.data(), resp.length(), "-2012", 5) != nullptr) {
        if (!last_renew_fail_ || millis() - last_renew_fail_ > 600000) {
            Serial.println("[cookie] 请求返回 -2012，自动续期并重试");
            if (tryRenew()) { // tryRenew 内部自置 renewing_ 防递归
                resp = do_once(method, url, body, referer, contentType); // 新 cookie 重发
            }
            // 失败防抖时间戳由 tryRenew 按失败类型自设，这里不覆盖
        }
    }
    return resp;
}

HttpResponse WereadClient::get(const String& url, const String& referer) {
    return request("GET", url, nullptr, referer, "");
}

HttpResponse WereadClient::postJson(const String& url, const String& jsonBody, const String& referer) {
    return request("POST", url, &jsonBody, referer, "application/json;charset=UTF-8");
}

// 续期：wr_skey 是短效令牌（几小时过期），用 wr_rt 调 renewal 换新；成功返回 true 并落盘
// 失败分两类：传输失败（网络问题，30s 后可再试）/ 服务器拒绝（wr_rt 已死，置 rt_dead_ 只能重扫码）
bool WereadClient::tryRenew() {
    renewing_ = true; // 防 request 层对 renewal 响应再触发自动续期（递归）
    String body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    HttpResponse r = postJson(String(WEREAD_HOST_WEB) + "/web/login/renewal", body);
    renewing_ = false;
    if (!r.ok()) {
        Serial.printf("[cookie] renewal 传输失败 status=%d err=%s（30s 后可再试）\n", r.status, r.error.c_str());
        last_renew_fail_ = millis() - 570000; // 相当于 30s 后解除防抖（600s-570s）
        return false;
    }
    // 解析响应：errcode 非 0 / 无 succ 即失败（r 可能是 PSRAM，用 data()/length()）
    int ec = 0, succ = 0;
    JsonDocument doc;
    if (!deserializeJson(doc, r.data(), r.length())) {
        ec = doc["errcode"] | doc["errCode"] | 0;
        succ = doc["succ"] | 0;
    }
    if (ec != 0 || !succ) {
        Serial.printf("[cookie] renewal 被拒 errcode=%d（wr_rt 已失效，需重新扫码）\n", ec);
        last_renew_fail_ = millis(); // rt 死亡：10 分钟防抖（防每个请求翻倍）
        rt_dead_ = true;
        return false;
    }
    // 新 wr_skey 已由 absorbSetCookie 吸收进内存，落盘
    Serial.println("[cookie] renewal 成功，新令牌已生效");
    saveCookiesToConfig();
    clearCookiesDirty();
    return true;
}
