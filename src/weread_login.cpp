// weread_login.cpp — 实现
#include "weread_login.h"
#include "weread_client.h"
#include "config.h"
#include <ArduinoJson.h>

namespace weread_login {

// 从 JSON 字段取字符串（webLoginVid/uid 等可能是数字而非字符串）
static String jstr(JsonVariantConst v) {
    if (v.is<const char*>()) return v.as<String>();
    if (v.is<long long>())  return String((long)v.as<long long>());
    if (v.is<double>())     return String((long)v.as<long long>());
    return "";
}

bool run(std::function<void(const String& url, const String& status)> render_qr,
         std::function<void(const String&, const String&, const String&)> msg,
         unsigned long timeout_ms) {

    // ---- 1. 获取登录 uid ----
    // 注意 HttpResponse 禁止拷贝，只能移动；数据可能在 PSRAM，必须用 data()/length()
    HttpResponse r = WR.get(String(WEREAD_HOST_WEB) + "/api/auth/getLoginUid");
    if (!r.ok()) {
        Serial.printf("[login] getLoginUid 失败 status=%d err=%s\n", r.status, r.error.c_str());
        msg("登录失败", "拿不到 uid", "点按重试");
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, r.data(), r.length())) {
        Serial.println("[login] getLoginUid JSON 解析失败");
        msg("登录失败", "拿不到 uid", "点按重试");
        return false;
    }
    String uid = jstr(doc["uid"]); // 顶层 uid 或 data.uid
    if (!uid.length()) uid = jstr(doc["data"]["uid"]);
    if (!uid.length()) {
        msg("登录失败", "拿不到 uid", "点按重试");
        return false;
    }
    Serial.printf("[login] uid=%s\n", uid.c_str());

    // ---- 2. 显示二维码，等待扫码 ----
    render_qr(String(WEREAD_HOST_WEB) + "/web/confirm?uid=" + uid, "用微信/微信读书扫码");

    // ---- 3. 每 2 秒轮询登录状态 ----
    unsigned long start = millis();
    bool ok_login = false;
    String vid, token;
    while (millis() - start < timeout_ms) {
        HttpResponse pr = WR.get(String(WEREAD_HOST_WEB) + "/api/auth/getLoginInfo?uid=" + uid + "&otp=");
        if (!pr.ok()) {
            // HTTP 层失败（非 2xx/网络错误）不算致命，继续轮询
            Serial.printf("[login] 轮询 HTTP 失败 status=%d err=%s，继续\n", pr.status, pr.error.c_str());
            delay(2000);
            continue;
        }
        JsonDocument pdoc;
        if (deserializeJson(pdoc, pr.data(), pr.length())) {
            Serial.println("[login] 轮询 JSON 解析失败，继续");
            delay(2000);
            continue;
        }
        // 字段取值：优先 "data" 对象，缺失回退顶层（等价 python data = r.get("data", r)）
        JsonObjectConst data = pdoc["data"].as<JsonObject>();
        auto field = [&](const char* key) -> JsonVariantConst {
            JsonVariantConst v;
            if (!data.isNull()) v = data[key];
            if (v.isNull()) v = pdoc[key];
            return v;
        };
        bool succeed  = field("succeed").as<bool>();
        String pv     = jstr(field("webLoginVid"));
        if (!pv.length()) pv = jstr(field("vid"));
        if (!pv.length()) pv = jstr(field("userVid"));
        if (!pv.length()) pv = jstr(field("user_vid"));
        String ptok   = jstr(field("accessToken"));
        String logic  = jstr(field("logicCode"));
        Serial.printf("[login] 轮询 succeed=%d vid=%s token=%s logic=%s\n",
                      succeed, pv.length() ? "有" : "无", ptok.length() ? "有" : "无", logic.c_str());

        if (succeed && pv.length() && ptok.length()) { // 扫码确认成功
            vid = pv; token = ptok; ok_login = true;
            break;
        }
        if (logic == "NEED_OTP" || logic == "OTP_EXPIRED" || logic == "OTP_NOT_MATCH") {
            msg("需要手机 OTP 验证", "暂不支持", logic);
            return false;
        }
        if (logic.length() && logic != "LOGIN_TIMEOUT") {
            msg("登录失败", logic, "点按重试");
            return false;
        }
        delay(2000);
    }

    if (!ok_login) {
        msg("扫码超时", "点按屏幕重试", "");
        return false;
    }

    // ---- 4. 收尾：保存 cookie ----
    // 真正的 wr_skey/wr_vid 在 getLoginInfo 响应的 set-cookie 里，
    // WereadClient 已自动吸收进 cookie jar；只在缺失时用 accessToken/vid 兜底
    if (!WR.getCookie("wr_skey").length()) WR.setCookie("wr_skey", token);
    if (!WR.getCookie("wr_vid").length())  WR.setCookie("wr_vid", vid);
    WR.saveCookiesToConfig(); // 把 wr_* 写回配置文件并保留其它字段
    msg("登录成功", "正在加载书架...", "");
    return true;
}

} // namespace weread_login
