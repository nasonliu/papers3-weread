#include "network_diag.h"

#include "config.h"
#include "weread_client.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <cerrno>
#include <cstring>

namespace network_diag {
namespace {

constexpr int kConnectTimeoutMs = 8000;
constexpr int kHttpTimeoutMs = 12000;
constexpr const char* kWereadHost = "weread.qq.com";
constexpr const char* kControlHost = "www.qq.com";
constexpr const char* kWereadUrl = "https://weread.qq.com/favicon.ico";
constexpr const char* kControlUrl = "https://www.qq.com/favicon.ico";

struct TcpResult {
    bool dns_ok = false;
    bool connect_ok = false;
    unsigned dns_ms = 0;
    unsigned connect_ms = 0;
    int gai_error = 0;
    int socket_error = 0;
    char ip[INET_ADDRSTRLEN] = {0};
};

struct HttpResult {
    bool ok = false;
    unsigned elapsed_ms = 0;
    int status = -1;
    esp_err_t error = ESP_FAIL;
};

esp_err_t discard_http_event(esp_http_client_event_t*) {
    return ESP_OK;
}

TcpResult tcp_probe(const char* host) {
    TcpResult result;
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    const unsigned dns_started = millis();
    addrinfo* addresses = nullptr;
    result.gai_error = getaddrinfo(host, "443", &hints, &addresses);
    result.dns_ms = millis() - dns_started;
    if (result.gai_error != 0 || !addresses) {
        return result;
    }
    result.dns_ok = true;

    const sockaddr_in* target = reinterpret_cast<const sockaddr_in*>(addresses->ai_addr);
    inet_ntop(AF_INET, &target->sin_addr, result.ip, sizeof(result.ip));

    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        result.socket_error = errno;
        freeaddrinfo(addresses);
        return result;
    }

    const int original_flags = fcntl(fd, F_GETFL, 0);
    if (original_flags >= 0) {
        fcntl(fd, F_SETFL, original_flags | O_NONBLOCK);
    }

    const unsigned connect_started = millis();
    int rc = connect(fd, addresses->ai_addr, addresses->ai_addrlen);
    if (rc == 0) {
        result.connect_ok = true;
    } else if (errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        timeval timeout = {kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
        rc = select(fd + 1, nullptr, &writable, nullptr, &timeout);
        if (rc > 0 && FD_ISSET(fd, &writable)) {
            socklen_t length = sizeof(result.socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result.socket_error, &length) == 0) {
                result.connect_ok = result.socket_error == 0;
            }
        } else {
            result.socket_error = rc == 0 ? ETIMEDOUT : errno;
        }
    } else {
        result.socket_error = errno;
    }
    result.connect_ms = millis() - connect_started;

    close(fd);
    freeaddrinfo(addresses);
    return result;
}

esp_http_client_handle_t make_http_client(const char* url, bool keep_alive) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = discard_http_event;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.keep_alive_enable = keep_alive;
    return esp_http_client_init(&config);
}

HttpResult http_probe(esp_http_client_handle_t client, const char* url) {
    HttpResult result;
    if (!client) {
        result.error = ESP_ERR_NO_MEM;
        return result;
    }

    esp_http_client_set_url(client, url);
    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    esp_http_client_set_header(client, "User-Agent", "PaperS3-NetDiag/1");
    esp_http_client_set_header(client, "Accept", "*/*");

    const unsigned started = millis();
    result.error = esp_http_client_perform(client);
    result.elapsed_ms = millis() - started;
    if (result.error == ESP_OK) {
        result.status = esp_http_client_get_status_code(client);
        result.ok = result.status > 0;
    }
    return result;
}

void print_tcp(const char* phase, unsigned round, const char* host, const TcpResult& r) {
    Serial.printf(
        "[netdiag] phase=%s round=%u host=%s dns=%s dns_ms=%u ip=%s tcp=%s tcp_ms=%u err=%d\n",
        phase, round, host, r.dns_ok ? "ok" : "fail", r.dns_ms,
        r.ip[0] ? r.ip : "-", r.connect_ok ? "ok" : "fail", r.connect_ms,
        r.dns_ok ? r.socket_error : r.gai_error);
}

void print_http(const char* phase, unsigned round, const char* host, const HttpResult& r) {
    Serial.printf(
        "[netdiag] phase=%s round=%u host=%s tls=%s ms=%u status=%d err=%s\n",
        phase, round, host, r.ok ? "ok" : "fail", r.elapsed_ms, r.status,
        esp_err_to_name(r.error));
}

}  // namespace

void run(unsigned rounds) {
    if (rounds < 1) rounds = 1;
    if (rounds > 30) rounds = 30;

    Serial.printf(
        "[netdiag] begin rounds=%u wifi=%s rssi=%d channel=%d heap=%u internal=%u psram=%u\n",
        rounds, WiFi.status() == WL_CONNECTED ? "up" : "down", WiFi.RSSI(),
        WiFi.channel(), static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[netdiag] end reason=wifi_down");
        return;
    }

    unsigned weread_tcp_ok = 0;
    unsigned control_tcp_ok = 0;
    unsigned fresh_tls_ok = 0;
    unsigned control_tls_ok = 0;
    unsigned reused_tls_ok = 0;

    for (unsigned round = 1; round <= rounds; ++round) {
        TcpResult weread = tcp_probe(kWereadHost);
        print_tcp("tcp", round, kWereadHost, weread);
        weread_tcp_ok += weread.connect_ok;

        TcpResult control = tcp_probe(kControlHost);
        print_tcp("tcp-control", round, kControlHost, control);
        control_tcp_ok += control.connect_ok;

        esp_http_client_handle_t fresh = make_http_client(kWereadUrl, false);
        HttpResult fresh_result = http_probe(fresh, kWereadUrl);
        print_http("tls-fresh", round, kWereadHost, fresh_result);
        fresh_tls_ok += fresh_result.ok;
        if (fresh) esp_http_client_cleanup(fresh);

        esp_http_client_handle_t fresh_control = make_http_client(kControlUrl, false);
        HttpResult control_result = http_probe(fresh_control, kControlUrl);
        print_http("tls-control", round, kControlHost, control_result);
        control_tls_ok += control_result.ok;
        if (fresh_control) esp_http_client_cleanup(fresh_control);

        delay(250);
    }

    esp_http_client_handle_t reused = make_http_client(kWereadUrl, true);
    for (unsigned round = 1; round <= rounds; ++round) {
        HttpResult reused_result = http_probe(reused, kWereadUrl);
        print_http("tls-reuse", round, kWereadHost, reused_result);
        reused_tls_ok += reused_result.ok;
        if (!reused_result.ok && reused) {
            esp_http_client_close(reused);
        }
        delay(250);
    }
    if (reused) esp_http_client_cleanup(reused);

    Serial.printf(
        "[netdiag] summary rounds=%u weread_tcp=%u control_tcp=%u fresh_tls=%u control_tls=%u reused_tls=%u "
        "heap=%u internal=%u psram=%u\n",
        rounds, weread_tcp_ok, control_tcp_ok, fresh_tls_ok, control_tls_ok, reused_tls_ok,
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    Serial.println("[netdiag] end");
}

void run_api(unsigned rounds) {
    if (rounds < 1) rounds = 1;
    if (rounds > 30) rounds = 30;

    unsigned succeeded = 0;
    unsigned failed = 0;
    Serial.printf(
        "[netapi] begin rounds=%u rssi=%d channel=%d internal=%u internal_largest=%u "
        "internal_min=%u psram=%u\n",
        rounds, WiFi.RSSI(), WiFi.channel(),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    const String url = String(WEREAD_HOST_WEB) + "/web/shelf/sync";
    for (unsigned round = 1; round <= rounds; ++round) {
        const unsigned started = millis();
        int status = -1;
        size_t bytes = 0;
        String error;
        {
            HttpResponse response = WR.get(url, "https://weread.qq.com/");
            status = response.status;
            bytes = response.length();
            error = response.error;
        }
        const unsigned elapsed = millis() - started;
        const bool ok = status >= 200 && status < 300 && bytes > 0;
        succeeded += ok;
        failed += !ok;
        Serial.printf(
            "[netapi] round=%u result=%s ms=%u status=%d bytes=%u err=%s internal=%u "
            "largest=%u min=%u psram=%u\n",
            round, ok ? "ok" : "fail", elapsed, status, static_cast<unsigned>(bytes),
            error.length() ? error.c_str() : "ESP_OK",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        delay(500);
    }

    Serial.printf("[netapi] summary rounds=%u ok=%u fail=%u\n", rounds, succeeded, failed);
    Serial.println("[netapi] end");
}

}  // namespace network_diag
