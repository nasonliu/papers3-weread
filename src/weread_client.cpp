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
};

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    RespBuf* rb = (RespBuf*)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb && evt->data_len > 0) {
                // 优先写 PSRAM buffer
                if (rb->psbuf) {
                    if (rb->pslen + evt->data_len <= rb->pscap) {
                        memcpy(rb->psbuf + rb->pslen, evt->data, evt->data_len);
                        rb->pslen += evt->data_len;
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

HttpResponse WereadClient::request(const String& method, const String& url, const String* body,
                                   const String& referer, const String& contentType) {
    auto do_once = [&](const String& m, const String& u, const String* b,
                       const String& ref, const String& ct) -> HttpResponse {
        HttpResponse resp;
        RespBuf rb;
        rb.body = &resp.body;
        rb.headers = &resp.headers;

        // 预分配 PSRAM 接收缓冲（大 JSON 避免 String 64KB bug）。给 512KB 足够书架 157KB
        const size_t BUFCAP = 512 * 1024;
        char* psbuf = (char*)heap_caps_malloc(BUFCAP, MALLOC_CAP_SPIRAM);
        if (psbuf) { rb.psbuf = psbuf; rb.pscap = BUFCAP; rb.pslen = 0; }

        esp_http_client_config_t cfg = {};
        cfg.url = u.c_str();
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.event_handler = http_event_handler;
        cfg.user_data = &rb;
        cfg.timeout_ms = HTTP_TIMEOUT_MS;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 2048;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { resp.error = "init failed"; if (psbuf) heap_caps_free(psbuf); return resp; }

        if (m == "POST") esp_http_client_set_method(client, HTTP_METHOD_POST);
        else esp_http_client_set_method(client, HTTP_METHOD_GET);

        esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
        esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
        esp_http_client_set_header(client, "Referer", ref.c_str());
        String cookie = cookieHeader();
        if (cookie.length()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
        if (b) {
            esp_http_client_set_header(client, "Content-Type", ct.c_str());
            esp_http_client_set_post_field(client, b->c_str(), b->length());
        }

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            resp.status = esp_http_client_get_status_code(client);
        } else {
            resp.error = String(esp_err_to_name(err));
            resp.status = -1;
        }
        esp_http_client_cleanup(client);

        // 接收结果挂到 resp：用了 PSRAM 就转移所有权，否则保持 String
        if (psbuf && rb.pslen > 0) {
            if (rb.pslen < rb.pscap) psbuf[rb.pslen] = '\0'; // 补结尾，data() 可安全 strstr
            resp.psbody = psbuf;
            resp.pslen = rb.pslen;
        } else if (psbuf) {
            heap_caps_free(psbuf); // 没收到数据，释放
        }
        absorbSetCookie(resp);
        return resp;
    };

    HttpResponse resp = do_once(method, url, body, referer, contentType);

    // 自动续期：wr_skey 短效（几小时过期），任何请求吃到 -2012 就用 wr_rt 续期并重试一次。
    // 只在响应小（错误页都几十字节）且含 -2012 时触发；renewal 自身/近期续期失败不触发（防抖）。
    if (resp.ok() && !renewing_ && hasValidCookie() && resp.length() < 2048 &&
        memmem(resp.data(), resp.length(), "-2012", 5) != nullptr) {
        if (millis() - last_renew_fail_ > 600000) { // 续期失败 10 分钟内不再试（防每个请求翻倍）
            Serial.println("[cookie] 请求返回 -2012，自动续期并重试");
            if (tryRenew()) { // tryRenew 内部自置 renewing_ 防递归
                resp = do_once(method, url, body, referer, contentType); // 新 cookie 重发
            } else {
                last_renew_fail_ = millis();
            }
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
// 服务器返回 {"succ":1,...} 或 {"errcode":-2012,...}（refresh token 也死了 → 只能重扫）
bool WereadClient::tryRenew() {
    renewing_ = true; // 防 request 层对 renewal 响应再触发自动续期（递归）
    String body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    HttpResponse r = postJson(String(WEREAD_HOST_WEB) + "/web/login/renewal", body);
    renewing_ = false;
    if (!r.ok()) {
        Serial.printf("[cookie] renewal HTTP 失败 status=%d err=%s\n", r.status, r.error.c_str());
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
        Serial.printf("[cookie] renewal 失败 errcode=%d（wr_rt 已失效，需重新扫码）\n", ec);
        return false;
    }
    // 新 wr_skey 已由 absorbSetCookie 吸收进内存，落盘
    Serial.println("[cookie] renewal 成功，新令牌已生效");
    saveCookiesToConfig();
    clearCookiesDirty();
    return true;
}
