// provision.cpp — WiFi 配网门户实现
// AP "PaperS3-阅读器"（开放）+ DNS 劫持（captive portal）+ Web 配网页 http://192.168.4.1
// 成功后把 wifi_ssid/wifi_pass 写回配置（保留其它字段），展示结果约 3 秒后自动重启
#include "provision.h"
#include "storage.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>

// ---------------- 连接状态机 ----------------
enum ProvState { ST_IDLE, ST_CONNECTING, ST_OK, ST_FAIL };

static DNSServer   g_dns;
static WebServer   g_server(80);
static void      (*g_ui)(const String&, const String&, const String&) = nullptr;

static volatile ProvState g_state = ST_IDLE;  // 事件回调里也会写，加 volatile
static ProvState g_ui_state = ST_IDLE;        // 已刷新到屏幕的状态
static String    g_ssid;                      // 正在连接/已连上的 SSID
static String    g_pass;                      // 对应密码（成功时写配置用）
static String    g_ip;                        // 成功后拿到的 IP
static String    g_reason;                    // 失败原因（中文）
static unsigned long g_connect_start = 0;     // connecting 起点（15 秒超时用）
static unsigned long g_ok_ms = 0;             // 成功时刻（再服务 3 秒后重启）

// ---------------- 配网页面（PROGMEM，UTF-8 中文，内联 JS） ----------------
static const char PORTAL_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PaperS3 配网</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}
button{font-size:16px;padding:8px 20px;border:0;border-radius:6px;background:#222;color:#fff}
.ap{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin:8px 0;cursor:pointer}
.ap:active{background:#e8e8e8}
.dim{color:#888}
#passbox{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin-top:12px}
#pass{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
#st{margin-top:16px;font-size:15px;min-height:22px}
.ok{color:#1a7f37}
.err{color:#c00}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #ccc;border-top-color:#333;border-radius:50%;animation:sp .8s linear infinite;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<h2>PaperS3 阅读器配网</h2>
<button onclick="scan()">刷新</button>
<div id="list" class="dim" style="margin-top:12px"></div>
<div id="passbox" style="display:none">
<div>网络：<b id="ssidv"></b></div>
<input id="pass" type="password" placeholder="WiFi 密码（开放网络留空）">
<button onclick="conn()">连接</button>
</div>
<div id="st"></div>
<script>
var sel=null,timer=null;
function scan(){
  var L=document.getElementById('list');
  L.textContent='扫描中…';
  fetch('/scan').then(function(r){return r.json();}).then(function(d){
    if(d.ready===false){setTimeout(scan,2000);return;}
    L.textContent='';
    if(!d.length){L.textContent='未发现网络，点刷新重试';return;}
    d.forEach(function(n){
      var div=document.createElement('div');
      div.className='ap';
      div.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');
      div.onclick=function(){pick(n);};
      L.appendChild(div);
    });
  }).catch(function(){L.textContent='扫描失败，点刷新重试';});
}
function pick(n){
  sel=n;
  document.getElementById('ssidv').textContent=n.ssid;
  document.getElementById('passbox').style.display='block';
  document.getElementById('pass').focus();
}
function conn(){
  if(!sel)return;
  var body='ssid='+encodeURIComponent(sel.ssid)+'&pass='+encodeURIComponent(document.getElementById('pass').value);
  fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
  document.getElementById('passbox').style.display='none';
  show('connecting');
  if(timer)clearInterval(timer);
  timer=setInterval(poll,2000);
}
function poll(){
  fetch('/status').then(function(r){return r.json();}).then(function(s){
    show(s.state,s);
    if(s.state==='ok'||s.state==='fail'){clearInterval(timer);timer=null;}
  }).catch(function(){});
}
function show(st,s){
  var el=document.getElementById('st');
  if(st==='connecting'){el.className='dim';el.innerHTML='<span class="spin"></span> 连接中，请稍候…';}
  else if(st==='ok'){el.className='ok';el.textContent='连接成功！IP: '+((s&&s.ip)||'')+'，设备即将重启';}
  else if(st==='fail'){el.className='err';el.textContent='连接失败：'+((s&&s.reason)||'未知原因')+'（可重新选择网络重试）';}
  else{el.className='dim';el.textContent='';}
}
scan();
</script>
</body>
</html>)rawliteral";

// ---------------- 断开原因码 → 中文 ----------------
static String fail_reason_text(uint8_t reason) {
    switch (reason) {
        case 201: return "找不到该网络（SSID 错误或信号差）";
        case 15:  return "密码错误（四次握手超时）";
        case 2:   return "认证失败";
        case 5:   return "关联失败（AP 拒绝）";
        case 202:
        case 203: return "信号差或认证超时";
        case 8:   return "连接被断开";
        default:  return "连接失败(code " + String(reason) + ")";
    }
}

// ---------------- STA 事件跟踪 ----------------
static void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        g_ip = WiFi.localIP().toString();
        g_state = ST_OK;
        Serial.printf("[prov] 连接成功 IP=%s\n", g_ip.c_str());
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        // 只关心 connecting 期间的失败（重试/初始事件不覆盖状态）
        if (g_state == ST_CONNECTING) {
            g_reason = fail_reason_text(info.wifi_sta_disconnected.reason);
            g_state = ST_FAIL;
            Serial.printf("[prov] 连接失败 reason=%d\n", (int)info.wifi_sta_disconnected.reason);
        }
    }
}

// ---------------- HTTP 路由 ----------------
static void handle_root() {
    g_server.send_P(200, "text/html", PORTAL_HTML);
}

static void handle_scan() {
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        // 还在扫：页面 2 秒后重试
        g_server.send(200, "application/json", "{\"ready\":false}");
        return;
    }
    if (n == WIFI_SCAN_FAILED) {
        // 上一次失败：重新 kick 一轮
        WiFi.scanNetworks(true);
        g_server.send(200, "application/json", "{\"ready\":false}");
        return;
    }
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (int16_t i = 0; i < n; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue; // 跳过隐藏 SSID
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]   = ssid;
        o["rssi"]   = WiFi.RSSI(i);
        o["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    String out;
    serializeJson(doc, out);
    WiFi.scanDelete();
    WiFi.scanNetworks(true); // 为下次刷新再 kick 一轮
    g_server.send(200, "application/json", out);
}

static void handle_connect() {
    String ssid = g_server.arg("ssid"); // WebServer 自动做 form 解码
    String pass = g_server.arg("pass");
    if (ssid.length() == 0) {
        g_server.send(400, "application/json", "{\"state\":\"fail\",\"reason\":\"缺少 ssid\"}");
        return;
    }
    Serial.printf("[prov] 尝试连接: %s\n", ssid.c_str());
    g_ssid = ssid;
    g_pass = pass;
    g_ip = "";
    g_reason = "";
    g_state = ST_CONNECTING;
    g_connect_start = millis();
    WiFi.begin(ssid.c_str(), pass.c_str()); // AP_STA 双模下 STA 侧去连（空密码=开放网络）
    g_server.send(200, "application/json", "{\"state\":\"connecting\"}");
}

static void handle_status() {
    JsonDocument doc;
    switch (g_state) {
        case ST_CONNECTING: doc["state"] = "connecting"; break;
        case ST_OK:         doc["state"] = "ok";         break;
        case ST_FAIL:       doc["state"] = "fail";       break;
        default:            doc["state"] = "idle";       break;
    }
    doc["reason"] = g_reason;
    doc["ssid"]   = g_ssid;
    doc["ip"]     = g_ip;
    String out;
    serializeJson(doc, out);
    g_server.send(200, "application/json", out);
}

static void handle_not_found() {
    // 配合 DNS 劫持：任意域名/路径都拉回门户首页，触发手机 captive 弹窗
    g_server.sendHeader("Location", "http://192.168.4.1/");
    g_server.send(302, "text/plain", "");
}

// ---------------- 保存 WiFi 配置（保留原有全部字段，只改两项） ----------------
static bool save_wifi_config(const String& ssid, const String& pass) {
    JsonDocument doc;
    File f = open_config_read();
    if (f) { deserializeJson(doc, f); f.close(); } // 没有文件/解析失败就从空配置开始
    doc["wifi_ssid"] = ssid;
    doc["wifi_pass"] = pass;
    File w = open_config_write();
    if (!w) {
        Serial.println("[prov] 配置写入失败：无法打开配置文件");
        return false;
    }
    serializeJson(doc, w);
    w.close();
    Serial.printf("[prov] WiFi 配置已保存（%s）: %s\n", storage_sd_ok() ? "SD 卡" : "SPIFFS", ssid.c_str());
    return true;
}

// ---------------- 门户入口（永不返回，成功后重启） ----------------
void run_provisioning_portal(void (*ui)(const String&, const String&, const String&), const char* boot_error) {
    g_ui = ui;
    Serial.println("[prov] 进入配网门户");
    SPIFFS.begin(true); // 兜底：确保无 SD 时配置可写

    // 进入门户的原因先显示一屏
    if (boot_error && boot_error[0]) {
        g_ui("WiFi 连接失败", String(boot_error), "进入配网模式...");
        delay(3000);
    }
    g_ui("配网模式", "热点: PaperS3-阅读器", "浏览器打开 192.168.4.1");

    WiFi.disconnect();           // 停掉 STA 侧残留的连接尝试（否则 AP 的 DHCP 可能起不来）
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    // 显式配置 AP 网段并给 dhcpd 启动时间，避免客户端拿不到地址（自分配 169.254.x.x）
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
    bool ap_ok = WiFi.softAP("PaperS3-阅读器"); // 开放热点，无密码
    delay(500);
    IPAddress ap_ip = WiFi.softAPIP(); // 固定 192.168.4.1
    Serial.printf("[prov] AP 已启动 ok=%d IP=%s\n", (int)ap_ok, ap_ip.toString().c_str());

    WiFi.onEvent(on_wifi_event);

    g_dns.start(53, "*", ap_ip); // 劫持所有域名 → captive portal

    g_server.on("/", HTTP_GET, handle_root);
    g_server.on("/scan", HTTP_GET, handle_scan);
    g_server.on("/connect", HTTP_POST, handle_connect);
    g_server.on("/status", HTTP_GET, handle_status);
    g_server.onNotFound(handle_not_found);
    g_server.begin();

    WiFi.scanDelete();
    WiFi.scanNetworks(true); // 启动即 kick 一轮异步扫描，首个 /scan 不必等太久

    while (true) {
        g_dns.processNextRequest();
        g_server.handleClient();

        // connecting 状态 15 秒无结果 → 超时失败
        if (g_state == ST_CONNECTING && millis() - g_connect_start > 15000) {
            g_reason = "连接超时";
            g_state = ST_FAIL;
            Serial.println("[prov] 连接超时");
        }

        // 状态变化时刷新墨水屏（OK 在下方单独处理：先存配置再提示）
        if (g_state != g_ui_state) {
            g_ui_state = g_state;
            if (g_state == ST_CONNECTING) {
                g_ui("配网模式", "连接中... " + g_ssid, "热点: PaperS3-阅读器");
            } else if (g_state == ST_FAIL) {
                g_ui("WiFi 连接失败", g_reason, "请在网页重试");
            }
        }

        // 成功路径：保存配置 → 提示 → 再服务约 3 秒让网页轮询到 ok → 重启
        if (g_state == ST_OK) {
            if (g_ok_ms == 0) {
                g_ok_ms = millis();
                save_wifi_config(g_ssid, g_pass);
                g_ui("WiFi 连接成功", g_ssid, "设备即将重启...");
            }
            if (millis() - g_ok_ms > 3000) {
                ESP.restart(); // 不返回
            }
        }

        delay(2);
    }
}
