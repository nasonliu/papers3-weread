#!/usr/bin/env python3
"""微信读书扫码登录抓 cookie（复刻 REweread login-qr.lua 流程）"""
import requests, time, json, sys, os

UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 Safari/537.36 Edg/135.0.0.0"
BASE = "https://weread.qq.com"

s = requests.Session()
s.headers.update({
    "User-Agent": UA,
    "Origin": "https://weread.qq.com",
    "Referer": "https://weread.qq.com/",
})

def api(path, params=None, timeout=70):
    r = s.get(BASE + path, params=params or {}, timeout=timeout)
    r.raise_for_status()
    return r.json()

print("=== 1. 获取登录 uid ===")
d = api("/api/auth/getLoginUid", timeout=20)
uid = d.get("uid") or d.get("data", {}).get("uid")
if not uid:
    print("拿不到 uid:", d); sys.exit(1)
print("uid =", uid)

confirm_url = f"https://weread.qq.com/web/confirm?uid={uid}"
print("\n=== 2. 生成二维码 ===")
print("确认 URL:", confirm_url)

import qrcode
qr = qrcode.QRCode(border=2)
qr.add_data(confirm_url)
qr.make()
img_path = os.path.expanduser("~/papers3-weread/weread_login_qr.png")
qr.make_image().save(img_path)
print("二维码已保存:", img_path)

# 终端也打印 ASCII 二维码
qr.print_ascii(invert=True)

print("\n=== 3. 等待扫码（最多 300 秒）===")
print("请用微信读书 App 或微信扫描二维码...\n")

deadline = time.time() + 300
login_result = None
while time.time() < deadline:
    try:
        # 直接拿 response 对象（不经过 api() 的 .json()），保留 set-cookie
        resp = s.get(BASE + "/api/auth/getLoginInfo", params={"uid": uid, "otp": ""}, timeout=70)
        resp.raise_for_status()
        r = resp.json()
    except Exception as e:
        time.sleep(1); continue
    data = r.get("data", r)
    succeed = data.get("succeed") or r.get("succeed")
    vid = data.get("webLoginVid") or data.get("vid") or data.get("userVid") or data.get("user_vid")
    token = data.get("accessToken")
    logic = str(data.get("logicCode") or "")
    print(f"  轮询: succeed={succeed} vid={'有' if vid else '无'} token={'有' if token else '无'} logic={logic}", flush=True)
    if succeed and vid and token:
        login_result = {"vid": vid, "accessToken": token, "raw": data}
        # 打印这次响应的完整 set-cookie（真正的 wr_skey 在这里）
        print("\n=== 登录成功响应的 set-cookie header ===")
        sc = resp.headers.get("set-cookie", "(无)")
        print(sc if sc else "(无 set-cookie)")
        print("\n=== 登录成功后 cookie jar 全部内容 ===")
        for c in s.cookies:
            print(f"  {c.name} = {c.value}  (domain={c.domain})")
        break
    if logic in ("NEED_OTP", "OTP_EXPIRED", "OTP_NOT_MATCH"):
        print("需要手机 OTP 验证，暂不支持:", logic); sys.exit(1)
    if logic and logic != "LOGIN_TIMEOUT":
        print("登录失败:", logic, data); sys.exit(1)
    time.sleep(1)

if not login_result:
    print("\n扫码超时"); sys.exit(1)

print("\n=== 4. 登录成功 ===")
print("wr_vid =", login_result["vid"])
print("accessToken =", login_result["accessToken"])

# 关键：登录接口返回的 accessToken 不是 cookie 里的 wr_skey。
# 真正的 wr_skey 在 getLoginInfo 响应的 set-cookie 里（requests 已存入 s.cookies）。
# 收集 set-cookie
cookies = dict(s.cookies)
print("\n=== 5. 会话 cookie jar（服务器 set 的真实 cookie）===")
for k, v in cookies.items():
    print(f"  {k} = {v}")

# 真实 wr_skey 优先取 cookie jar 里的 wr_skey（服务器 set-cookie 下发）
real_skey = cookies.get("wr_skey") or login_result["accessToken"]
print(f"\n>>> 采用 wr_skey = {real_skey} (来源: {'cookie jar' if cookies.get('wr_skey') else 'accessToken'})")

# 用真实 cookie 验证书架接口
print("\n=== 6. 验证 cookie 有效性（请求书架）===")
import requests as _rq
vr = _rq.get("https://i.weread.qq.com/shelf/sync",
             params={"userVid": login_result["vid"], "synckey": "0", "lectureSynckey": "0"},
             headers={"User-Agent": UA},
             cookies={"wr_vid": login_result["vid"], "wr_skey": real_skey, "wr_rt": cookies.get("wr_rt", "")},
             timeout=20)
print("  HTTP", vr.status_code, "返回前120:", vr.text[:120])
try:
    vj = vr.json()
    if vj.get("errCode") == -2012 or vj.get("errcode") == -2012:
        print("  ⚠️ 仍是登录超时，cookie 无效")
    else:
        books = vj.get("books", [])
        print(f"  ✅ cookie 有效！书架 {len(books)} 本书")
except Exception as e:
    print("  解析失败", e)

# 保存完整 cookie（用真实 wr_skey）
all_cookies = {"wr_vid": login_result["vid"], "wr_skey": real_skey}
all_cookies.update(cookies)
all_cookies["wr_skey"] = real_skey  # 确保不被覆盖
out = os.path.expanduser("~/papers3-weread/weread_cookies.json")
with open(out, "w") as f:
    json.dump(all_cookies, f, ensure_ascii=False, indent=2)
print("\n完整 cookie 已保存:", out)
print(json.dumps(all_cookies, ensure_ascii=False, indent=2))
