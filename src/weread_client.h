// weread_client.h — HTTP/TLS 客户端 + cookie 管理（基于 esp_http_client）
#pragma once
#include <Arduino.h>
#include <vector>
#include <map>

struct HttpResponse {
    int status = 0;
    String body;
    std::map<String,String> headers; // 小写 key
    String error;
    // 大响应走 PSRAM（String 超 64KB 有 bug）；非空时以 psbody/pslen 为准
    char* psbody = nullptr;
    size_t pslen = 0;
    // 拥有 psbody 所有权：移动语义 + 析构释放，禁止拷贝（防 double-free）
    HttpResponse() = default;
    ~HttpResponse();
    HttpResponse(HttpResponse&& o) noexcept;
    HttpResponse& operator=(HttpResponse&& o) noexcept;
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;
    bool ok() const { return status >= 200 && status < 300; }
    // 有效数据的指针和长度（优先 PSRAM）
    const char* data() const { return psbody ? psbody : body.c_str(); }
    size_t length() const { return psbody ? pslen : (size_t)body.length(); }
};

class WereadClient {
public:
    // 从 SPIFFS 配置加载 WiFi 凭据与 weread cookie
    bool loadConfig();
    bool connectWiFi(unsigned long timeout_ms = 15000);

    // cookie 操作
    void setCookie(const String& name, const String& value);
    String getCookie(const String& name) const;
    String cookieHeader() const;
    bool hasValidCookie() const;
    void saveCookies();      // 持久化到 SPIFFS
    bool tryRenew();         // 用 wr_rt 续期 wr_skey（短效令牌）；成功自动落盘
    bool saveCookiesToConfig(); // 把当前 wr_* cookie 写回配置文件（保留 wifi 等其它字段）
    // cookie 被服务器轮换后置脏，主循环定期落盘（否则重启后旧 cookie 失效要重扫）
    bool cookiesDirty() const { return cookies_dirty_; }
    void clearCookiesDirty() { cookies_dirty_ = false; }

    // 通用请求（自动带 cookie、UA、referer）
    HttpResponse get(const String& url, const String& referer = "https://weread.qq.com/");
    HttpResponse postJson(const String& url, const String& jsonBody, const String& referer = "https://weread.qq.com/");

    // 配置值（从配置文件读入）
    String wifi_ssid, wifi_pass;
    String cfg_wr_vid, cfg_wr_skey, cfg_wr_rt, cfg_api_key;
    String cfg_font; // 字体文件路径（config "font" 字段，可空=默认字体）

private:
    std::map<String,String> cookies_;
    bool cookies_dirty_ = false;    // wr_skey/wr_rt 被 set-cookie 轮换后置真
    bool renewing_ = false;         // tryRenew 进行中（防 request 层自动续期递归）
    unsigned long last_renew_fail_ = 0; // 上次续期失败时间（10 分钟防抖）
    HttpResponse request(const String& method, const String& url, const String* body, const String& referer, const String& contentType);
    void absorbSetCookie(const HttpResponse& resp);
};

extern WereadClient WR;
