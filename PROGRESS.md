# PaperS3 微信读书阅读器 — 项目进度 / 交接文档

> 最后更新：2026-07-17（阅读器完整形态版）
> 状态：**产品形态可用**：开机首屏 → 自动连网/配网/扫码登录 → 封面书架（最近阅读排序）→ 书籍详情（继续阅读/下载整本）→ 图文混排阅读（快刷翻页/进度同步/字体切换）→ 休眠封面 + 开机恢复。

---

## 一、项目目标

把微信读书 Web 接口移植到 **M5Stack PaperS3**（ESP32-S3 + 4.7 寸墨水屏 ED047TC1，960x540）：
串口/按键交互 → WiFi → 拉书架 → 选书 → 拉目录 → 拉正文 → 墨水屏中文渲染。

协议蓝本：`ref/weread.koplugin`（KOReader 插件）
固件基线参考：`ref/M5ReadPaper`、`ref/M5PaperS3-UserDemo`（官方出厂示例，已克隆到 ref/）

---

## 二、当前已完成（里程碑）

| 模块 | 状态 | 说明 |
|------|------|------|
| 编译/烧录 | ✅ | PlatformIO，`firmware.bin` ~1.9MB，Flash 占 13.9% |
| SD 卡挂载 | ✅ | SPI 模式；配置/进度/章节缓存/图片缓存全在 SD `/weread/` |
| 中文字体 | ✅ | EDCBook `.bin` 双字体：**UI 固定霞鹜文楷** + **正文可切换**（阅读页内实时换，切换即重排）。字体列表**每行用它自己的字体渲染**（预览页整屏位图缓存 SD，清单/选择没变秒开）。**load 重载必须清 entries_/字形缓存**（否则换字体无效）；**e.name() 返回裸文件名，必须拼 "/font/" 前缀**（裸名 load 必失败） |
| 中文渲染 | ✅ | 黑白反转 bug 已修复 |
| WiFi 连接 | ✅ | 自动重连 |
| 配网门户 | ✅ | 无配置/失败自动开 AP `PaperS3-阅读器` + captive 弹窗 + 网页选 WiFi；失败显示中文原因 |
| 扫码登录 | ✅ | cookie 过期自动弹 QR（qrcodegen 本地生成），手机扫码即登录 |
| 书架 | ✅ | 封面 5 本/页 + **最近阅读排序**（bookProgress.readUpdateTime）+ 进度百分比 |
| 书籍详情 | ✅ | 封面/标题/作者/简介/热门评论(listType=3)；[继续阅读]（服务器章级+本地页级进度）[下载整本]（进度条+取消） |
| 正文阅读 | ✅ | **图文混排**（getChapterBlocks：文本+图片块，游标分页）；**字形缓存后翻页 0.4-0.9s**；每 8 页全刷 |
| 后台任务 | ✅ | net_worker：读完一章自动预取下一章（正文+插图进缓存）；进度上传挪后台（翻页不再偶发 5-6s 卡）；g_net_mtx 递归锁管两任务网络互斥 |
| 图片 | ✅ | JPEG/PNG/GIF 解码灰度上屏（TJpg_Decoder/PNGdec/AnimatedGIF），SD 缓存，鉴权图源 cookie 重试 |
| 进度同步 | ✅ | 上传 POST /web/book/read（koplugin 签名 payload 实测 succ:1）；本地 SD progress.json 页级 |
| 休眠 | ✅ | 书架"睡眠"按钮 / 闲置 5 分钟 → 封面全屏 → **light sleep 浅睡，摸屏幕秒醒原地恢复**（不重启）；WiFi 醒后重连。断电恢复路径（state.json）保留兜底 |
| 正文显示 | ✅ | 大章 PSRAM 化（64KB String 损坏）+ **swap 算法修复**（URL/字节错位根因，已实锤） |

---

## 三、硬件引脚（PaperS3）

### SD 卡（SPI 模式，非 SD_MMC）
```
CS   = 47
SCK  = 39
MOSI = 38
MISO = 40
```
代码里：`SD.begin(47, sdSPI, 40000000)`，其中 `sdSPI.begin(39, 40, 38, 47)`（SCK, MISO, MOSI, CS）。

### 屏幕
- 物理 960x540（横），阅读用竖屏逻辑分辨率 **540x960**
- M5GFX 自动识别 `board_M5PaperS3`

### 串口
- 烧录/日志：`/dev/cu.usbmodem2101`，波特率 115200

### 电源键（重要认知）
**软件不可读**（不是 GPIO/PMIC 事件，是纯硬件上电锁存）。软件只能用 GPIO44 脉冲**主动断电**（`M5.Power.powerOff()`）。所以"按电源键休眠/唤醒"做不到 → 休眠用 **light sleep 浅睡**（`M5.Power.lightSleep(0,true)`，GPIO48 触摸 INT 唤醒，内部含防误醒等待）：入睡显示封面（墨水屏断电图像永久保持），摸屏幕**秒醒原地恢复**（RAM 保持，程序从下一行继续，不重启）；醒后 `M5.Display.wakeup()` + WiFi 重连 + NTP 重校时。断电恢复路径（state.json 一次性标记）保留作兜底（电池耗尽等情况）。

---

## 四、中文字体（双字体架构）

- **UI 字体**：固定 `/font/霞鹜文楷_大.bin`（`g_ui_font`），菜单/书架/标题/按钮全用它——任何字体出问题都不会影响 UI。
- **正文字体**：`g_read_font`，配置项 `font` 字段；阅读页顶栏右半"字体"→ 字体列表 → 点选即重排实时预览（`apply_font` 会校验字高 8..200，失败回滚文楷）。
- 字体目录运行时扫描 `/font/*.bin`；两个 EdcFont 实例各占 ~300KB PSRAM 索引，无压力。
- 渲染核心 `drawTextFont(font,...)`，包装 `drawUI()` / `drawRead()`；测宽同理 `textWidthUI/Read`；`next_line_end(font,...)` 分页测宽用正文字体。

### ⚠️ 关键 bug：灰度黑白反转（已修复）
字体数据是 **4-bit 灰度**：`0=黑，15=白`。渲染直接 `v = g*255/15`，**不要再加反转**。

---

## 五、微信读书接口（关键）

### 登录态（cookie）
存 SD `/weread/config.json`（SPIFFS `/config.json` 回退兼容）。

**令牌机制（重要）**：`wr_skey` 是**短效 access token（几小时过期）**，`wr_rt` 是长效 refresh token。过期后调 `POST /web/login/renewal`（cookie 明文）换新——**不用扫码**。续期策略（`WereadClient::tryRenew`，成功自动落盘）：① 开机主动续一次；② **request 层反应式续期**：任何请求返回 -2012 自动续期并重发（tryRenew 自置 renewing_ 防递归，失败 10 分钟防抖）——登录一次即可长期自维护，直到 wr_rt 也失效才需重扫。wr_skey 同时会被 set-cookie 轮换，absorbSetCookie 检测变化置脏，主循环 60s 定期写回。**没有定时唤醒**：浅睡时主循环冻结，续期只在开机/请求失败时发生，无耗电顾虑。

### 接口清单（全部实测，2026-07-17）
| 功能 | 方法 | URL | 备注 |
|------|------|-----|------|
| 书架+进度 | GET | `weread.qq.com/web/shelf/sync` | books[] + **bookProgress[]**（progress/chapterUid/updateTime） |
| 目录章节 | POST | `weread.qq.com/web/book/chapterInfos` | 章节带 tar 字段（资源包，插图其实是 wrepub 直链，tar 可忽略） |
| 正文 | GET reader HTML 取 psvts → POST e_0/e_1/e_3 或 t_0/t_1 分片 | 签名见 weread_crypto |
| 进度上传 | POST | `weread.qq.com/web/book/read` | koplugin 全套签名参数（appId/b/c/ci/co/sm/pr/rt/ts/rn/sg/ct/ps/pc+s），实测 succ:1 |
| 书籍详情 | GET | `weread.qq.com/web/book/info?bookId=` | intro/publisher/isbn/category；**无作者简介字段** |
| 热门评论 | GET | `weread.qq.com/web/review/list?bookId=&listType=3&maxIdx=0&count=N` | listType：1=好友流 2=最新 **3=本书推荐长评**；无点赞数 |
| 图片 | GET | CDN 直链（cdn/res.weread.qq.com、myqcloud） | 大部分裸取 200；**res.weread.qq.com 需 cookie（401）→ 302 到签名 CDN**；正文 `../Images/*` 相对图是阅读器 UI 装饰，跳过 |

### ⚠️ 域名坑
- ❌ `i.weread.qq.com/*` → 401 要额外签名（book/info、review/list、shelf/sync 都是）
- ✅ `weread.qq.com/web/*` → cookie 明文即可，**新接口全走 web 域名**

### -2012 错误
`{"errCode":-2012,"errMsg":"登录超时"}` = cookie 过期。**字段名有 errcode/errCode 两种**（实测是驼峰），书架 filter 两个都要留，否则检测失效。

### 风控
e_0 等分片接口偶发 `Connection failed, sock < 0` / `{}`——open_chapter / download_all 已加 2s 重试一次（实测有效）。

---

## 六、扫码登录流程

设备端内置（`src/weread_login.cpp`）：`getLoginUid` → 屏显 QR（`web/confirm?uid=`）→ 2s 轮询 `getLoginInfo` → set-cookie 落 SD。
电脑端备用：`weread_qr_login.py`（cookie 存 weread_cookies.json，供 Mac 侧接口测试）。
**串口 `cookie` 命令可导出设备当前 cookie**（调试用）。

---

## 七、写入设备配置（串口 CFG）

```
CFG {"wifi_ssid":"..","wifi_pass":"..","wr_vid":"..","wr_skey":"..","wr_rt":"..","api_key":""}
```
整体覆盖写，WiFi 和 cookie 必须一起发。串口命令：`n/p` 翻页、`s` 书架、`wifi` 配网、`login` 登录、`cookie` 导出、`N` 详情、`N:C` 直读第 C 章。

---

## 八、编译 / 烧录

```bash
cd ~/papers3-weread
export PATH="$HOME/.local/bin:$PATH"
pio run
pio run -t upload --upload-port /dev/cu.usbmodem2101
```

### framework
`arduino, espidf` 混合框架（不能去 espidf）。build_flags 有 `-fexceptions`（qrcodegen 需要）+ `-Iinclude`（PNGdec SIMD 桩头用）。

### 依赖库（lib_deps）
M5Unified 0.2.14、M5GFX 0.2.20（**别升 0.2.25，编译的是锁定版**）、ArduinoJson 7、TJpg_Decoder、PNGdec、unzipLIB（EPUB zip 防御路径）、AnimatedGIF。

### 桩头 `include/dsps_fft2r_platform.h`
PNGdec 的 S3 SIMD 汇编要 esp-dsp 的头（只用特性宏），桩头定义 `dsps_fft2r_sc16_aes3_enabled=1` 即可编译。

---

## 九、PlatformIO 环境踩坑（重要）

### 磁盘满导致编译假死
删 `.pio/build/PaperS3/.sconsign*.dblite`（保留 `.o`），重跑增量编译。

### RISC-V 工具链
espidf 混合框架强制要求 `toolchain-riscv32-esp`，不能从 platform.json 删。

---

## 十、待办（下一步，交接给后续 agent）

### 进行中
- [ ] **字体预览页待目检**：各行用各自字体渲染已实现（16 字体逐个 load 画行），整屏缓存已实现（第二次进秒开）——用户还没看效果
- [ ] 浅睡摸屏唤醒待用户确认（代码在，未实测）

### 后续增强
- [ ] 目录上屏选章（当前靠串口 `:章序号` 或详情页继续阅读）
- [ ] 正文排版优化（版心边距、行距，config.h 已定义 540x960 竖屏 + margin 36/56）
- [ ] 简介/热门评论缓存（详情页剩 ~4s 就来自这两个在线请求）
- [ ] EPUB 整包 zip 形态（防御路径已就绪未实测；包内图 epubimg: 提取未做）
- [ ] 自动休眠时间可配置

### 已知小坑
- `strip_xhtml` 不去 `<style>` 块：epub 版权页等前几张会显示一小段 CSS 文本（正文页无影响）
- 个别小 PNG（weread 装饰图）PNGdec 报 rc=7 拒画 → 显示 [图片] 占位，不影响正文
- 配网门户是开放热点（无密码）
- 书架排序按服务器 readUpdateTime，上传进度会改变自己这边顺序（串口选书序号每次开机以书单打印为准）
- SD 上 macOS 元数据文件（`._*.bin`）会混进字体目录扫描——已过滤，但往 SD 拷字体后最好用 `dot_clean` 清一下

---

## 十一、快速恢复清单（换电脑/重来时）

1. TF 卡放 `/font/霞鹜文楷_大.bin`（EDCBook 字体）
2. 串口发 CFG 写入 WiFi + cookie（见第七节）；或直接用配网门户
3. cookie 过期 → 设备自动弹 QR；电脑端备用 `weread_qr_login.py`
4. 编译卡死 → 删 `.sconsign*.dblite`（第九节）
5. 验证 cookie → 必须调 `/web/shelf/sync` 不是 `i.weread.qq.com`（第五节）
6. 屏幕字发虚 → 检查是否黑白反转 bug 回潮（第四节，g=0 应为纯黑）
7. 点按无反应/连翻 → 检查是否在阻塞翻页时丢触摸（翻页必须 `epd_flush_async`，触摸必须在独立 10ms 任务里，见第十二节）
8. 文字全不显示 → 检查是不是又把 UI 文字绑到了可换字体上（UI 必须用 g_ui_font，第四节）

---

## 十二、EPD 刷新与触摸架构（M5GFX 0.2.20 调研结论）

> 来源：`ref/M5PaperS3-UserDemo`、`ref/M5ReadPaper`、`.pio/libdeps/PaperS3/M5GFX@0.2.20` 源码。

### 刷新模式（`epd_mode_t`，enum.hpp:44）
| 模式 | 帧数 | 灰度 | 消影相 | 用途 |
|------|------|------|--------|------|
| `epd_quality` | ~31 | 16 级 | 有 | 全刷/书架/章首页，~1.7s |
| `epd_text` | ~31 | 16 级 | 有 | **不比 quality 快**，别当中间档 |
| `epd_fast` | ~9 | 1bit | 无 | **翻页首选**，~650ms |
| `epd_fastest` | ~6 | 1bit | 无 | 最快但残影最重 |

- fast/fastest 无消影相 → **每 N 页插一次 quality 全刷**（当前 N=8）。
- `setEpdMode` 必须在 `pushSprite` 之前调（写入时按模式抖动位深）。
- `pushSprite` 自动触发异步刷新，**不要再调 `display()`**；`waitDisplay()` 等波形物理播完。
- 0.2.20 的 `display(x,y,w,h)` 窗口不做旋转换算且不省时间 → 只用无参刷新。
- 开机必须一次全屏 quality 刷新同步面板状态。

### 触摸（GT911，驱动在 M5GFX）
- `M5.begin` 后触摸直接可用；坐标随 `setRotation` 自动映射为 540x960 逻辑坐标。
- `M5.update()` 必须在**独立任务**里 10ms 周期调用（GT911 超 128ms 不读丢数据）——本固件 `touch_task`，事件经 `g_touch_q` 队列给主循环。
- `wasPressed()`=落指（快，推荐）；`wasClicked()`=抬手（慢一拍）。
- PaperS3 **没有物理按键**；跨屏切换时 `xQueueReset` 丢弃残留点按。

---

## 十三、配网门户 / 扫码登录 / 休眠（2026-07-17 新增）

### 模块划分
- `src/provision.cpp` — 配网门户（阻塞，成功 ESP.restart）。AP + captive DNS + WebServer。路由 `/`、`/scan`、`/connect`、`/status`。
- `src/weread_login.cpp` + `src/qrcodegen.*` — 扫码登录 + QR 生成（vendored nayuki，MIT）。**坑：`Ecc::LOW/HIGH` 撞 Arduino 宏** → include 前 `#undef LOW/HIGH`；库用 throw → `-fexceptions`。
- `src/storage.cpp` — 配置 SD 优先、SPIFFS 回退（`open_config_read/write`）。
- `src/img_render.cpp` — 图片下载/缓存/解码（JPEG/PNG/GIF）/灰度上屏；`fetch_raw` 任意资源下载。
- `src/weread_api.cpp` — 接口层：书架(含 bookProgress)/目录/详情/评论/进度上传/正文 blocks（PSRAM 管线）。

### 启动流程（main.cpp setup）
```
首屏(图标+微信读书) → SD/SPIFFS/双字体 → 有配置?
  无 → 5s 串口 CFG 窗口(静默) → 配网门户
  有 → "正在连接 WiFi" → 失败 → 配网门户
→ NTP 校时 → "正在加载书架" → -2012? → 扫码登录
→ 排序(最近阅读) → 有睡眠标记? → 恢复阅读页 : 书架
```

### ⚠️ 坑（都是真机踩出来的）
1. **AP DHCP 自分配 169.254.x.x**：进门户前必须 `WiFi.disconnect()` + `softAPConfig` 显式网段 + `delay(500)`。
2. **-2012 字段名 errcode/errCode 两种**，filter 漏一个就检测失效。
3. **HttpResponse 数据在 PSRAM**：一律 `r.data()/r.length()`，`r.body` 永远为空（已改移动语义+析构释放）。
4. **String 超 64KB 损坏**（"长度虚高内容空洞"）：大章 xhtml/解码**全程 PSRAM**（weread_crypto::decode_content_shards_psram + weread_api 指针版切块），单文本块 ≤48KB。
5. **swap_positions 算法移植错误（图片 URL 损坏根因，已实锤）**：原算法是 `tonumber(bin(byte), 4)`（二进制数字符串按四进制解读 = bit 展开 第 b 位→4^b），早期移植错写成"每 2 位二进制转一个四进制数字拼接"，导致每章少量字节被换到错误位置——正文看不出，URL 被砸中就 404。修复：`val += 1U << (2*b)`。Mac 侧用同一章真实分片验证：错误实现逐字节复现设备坏 URL，修正后全部完好。
6. **分片禁止用 String 持有**：单片分片可超 64KB（~116KB 实测），String 持有即失真 → MD5 丢片 → swap 错位 → 内容错乱。分片数据从 HTTP 接收到解码全程 PSRAM 指针（steal_response_data 零复制）。
7. **图片 URL 归一化**：正文 `../Images/*` 相对图是阅读器 UI 装饰（tar 包里没有），跳过；CDN 绝对图才是书籍插图。
8. **res.weread.qq.com 要 cookie**（401），带 cookie 后 302 到签名 CDN URL；img_render 自动裸取失败→cookie 重试。图片下载两轮重试 + `.fail` 负缓存（404/坏 URL 不再刷屏重试）。
9. **cookie 会被服务器轮换**：设备端 absorbSetCookie 自动跟进（所以设备上一直有效，Mac 上存的副本会过期）——Mac 侧测试前用串口 `cookie` 命令导出最新值。
10. **电源键软件不可读**，只能主动断电；浅睡唤醒=摸屏幕（GPIO48），恢复靠 RAM 保持原地续跑。
11. **Arduino println 写 \r\n**：SD 缓存文件按行解析时必须 trim `\r`（曾致 blocks 缓存永不命中，下载完仍走网络）。
12. **cookie 轮换要落盘**：wr_skey/wr_rt 被 set-cookie 轮换后只活内存，重启读旧值即 -2012。已加脏标记（absorbSetCookie 检测变化）+ 主循环 60s 定期写回 + 入睡前兜底。
13. **下载提速 = 连接复用**：正文 reader+3 分片走 keep-alive 会话（weread_api 内部 ShardSession，握手 1 次/章跨章复用，失败丢弃重建）+ psvts 按书缓存（失效自动重取）；图片下载同 host keep-alive（img_render g_img_client）。偶发连接失败（sock<0）各入口都有 1.5-2s 重试一次。
14. **翻页慢的主因是字形逐字 SD 读**（~600 字/页 × ~4ms ≈ 2.5s），不是刷新也不是网络：EdcFont 解码结果缓存进 PSRAM（1.5MB 预算，超出整体清空；**load 重载必须清空 entries_ 和缓存**，否则换字体无效/翻页回落）。缓存后翻页 0.4-0.9s。
15. **多任务网络互斥**：net_worker（预取/上传）与主任务共用 ShardSession/img 会话/psvts 缓存 → 所有 weread_api/img_render 调用必须持 `g_net_mtx` 递归锁（net_lock/net_unlock），主任务改 g_chapters/g_shelf 时同理。**详情页卡死的根因就是封面下载漏加锁**：主任务与预取任务共用 g_img_client/g_cur_dl，请求数据写进对方栈缓冲区 → 内存损坏死机。教训：任何新增网络调用点先想锁。
16. **SD e.name() 只返回裸文件名**（不带目录）——拼路径必须自己加前缀（`/font/` + name），否则 SD.open 必失败（"换字体没效果"的最后一层原因）。且 SD 上会有 macOS 的 `._*` 元数据文件，列目录要过滤。

---

## 十四、交接速览（给下一个 agent）

**一句话现状**：产品形态可用——首屏/配网/登录全自动，封面书架+详情页+图文混排阅读+进度同步+字体切换+休眠恢复都在真机跑通。剩 URL 损坏收尾（agent 在查）+ 体验优化。

**立即可做**：
```bash
cd ~/papers3-weread && export PATH="$HOME/.local/bin:$PATH"
# 串口：n/p 翻页、s 书架、wifi/login/cookie、N 详情、N:C 直读
python3 -c "import serial,time; s=serial.Serial('/dev/cu.usbmodem2101',115200,timeout=1); time.sleep(0.5); s.write(b'1:3\n'); s.flush(); time.sleep(2); [print(s.readline().decode('utf-8','replace').rstrip()) for _ in range(60) if s.in_waiting or time.sleep(0.1) is None]"
```

**关键文件**：
- `src/main.cpp` — 启动流程/全部界面/触摸/分页/进度/休眠（drawTextFont 双字体是渲染核心）
- `src/weread_api.cpp` — 接口层（PSRAM 管线；书架用 `/web/shelf/sync`）
- `src/weread_crypto.cpp` — 签名/解码（PSRAM 版 decode_content_shards_psram）
- `src/img_render.cpp` — 图片三格式解码上屏
- `src/provision.cpp` / `src/weread_login.cpp` — 配网/登录
- `weread_qr_login.py` — 电脑端登录备用

**已解决的坑**（详见第五、十二、十三节）：①字体黑白反转 ②书架接口域名 ③响应体 PSRAM 不在 r.body ④AP DHCP 自分配 ⑤-2012 字段名+filter ⑥String 64KB 损坏 ⑦电源键不可读。

---

## 一、项目目标

把微信读书 Web 接口移植到 **M5Stack PaperS3**（ESP32-S3 + 4.7 寸墨水屏 ED047TC1，960x540）：
串口/按键交互 → WiFi → 拉书架 → 选书 → 拉目录 → 拉正文 → 墨水屏中文渲染。

协议蓝本：`ref/weread.koplugin`（KOReader 插件）
固件基线参考：`ref/M5ReadPaper`

---

## 二、当前已完成（里程碑）

| 模块 | 状态 | 说明 |
|------|------|------|
| 编译/烧录 | ✅ | PlatformIO，`firmware.bin` 1.2MB，Flash 占 9.2% |
| SD 卡挂载 | ✅ | SPI 模式，识别字体目录 |
| 中文字体加载 | ✅ | EDCBook `.bin` 解析成功，霞鹜文楷 21920 字 / 字高 36 |
| 中文渲染 | ✅ | **黑白反转 bug 已修复**，书架清晰黑色显示（用户已确认） |
| WiFi 连接 | ✅ | office2，IP 192.168.5.6 |
| 登录态获取 | ✅ | 扫码登录脚本可用，cookie 有效 |
| 书架接口 | ✅ | `/web/shelf/sync` 返回 200，115 本书，已上屏显示 |
| 正文显示 | ✅ | 选书→目录→正文解码→EDC 渲染真机验证通过（第二章 31427 字） |
| 分页翻页 | ✅ | 按版心像素宽分页（`paginate_current`），页码 x/y，串口 n/p 可翻 |
| 触摸 | ✅ | 独立 10ms 任务采集入队（GT911 超 128ms 不读丢数据）；阅读页左半=上页/右半=下页/顶部=回书架；书架点行选书、底部翻书架页 |
| 快刷 | ✅ | 翻页 epd_fast 异步推送 ~260ms（阻塞式 ~650ms→后台播放）；每 8 页 epd_quality 全刷清残影 |
| 配网门户 | ✅ | 无配置/WiFi失败自动开 AP `PaperS3-阅读器` + captive DNS + Web 配网页（192.168.4.1），网页选 SSID 输密码，成功保存 SD 并重启；失败显示中文原因（密码错误/找不到网络等）。书架右上"WiFi"或串口 `wifi` 可手动进 |
| 扫码登录 | ✅ | cookie 过期(-2012)自动弹 QR（qrcodegen 本地生成），手机微信/微信读书扫码 → 轮询 getLoginInfo → cookie 落 SD → 自动拉书架。书架右上"登录"或串口 `login` 可手动进 |
| SD 存储 | ✅ | 配置 `/weread/config.json`（SD 优先，SPIFFS 回退兼容）；章节正文缓存 `/weread/cache/<bookId>_<chapterUid>.txt`，命中不走网络 |

---

## 三、硬件引脚（PaperS3）

### SD 卡（SPI 模式，非 SD_MMC）
```
CS   = 47
SCK  = 39
MOSI = 38
MISO = 40
```
代码里：`SD.begin(47, sdSPI, 40000000)`，其中 `sdSPI.begin(39, 40, 38, 47)`（SCK, MISO, MOSI, CS）。

### 屏幕
- 物理 960x540（横），阅读用竖屏逻辑分辨率 **540x960**
- M5GFX 自动识别 `board_M5PaperS3`

### 串口
- 烧录/日志：`/dev/cu.usbmodem2101`，波特率 115200

---

## 四、中文字体（核心难点，已解决）

### 结论
**不用 LovyanGFX 的 VLW/TTF**，而是直接解析 **EDCBook 墨水屏阅读器的 `.bin` 点阵字体**（用户 TF 卡里现成收集的霞鹜文楷、思源宋体等，显示效果好）。

### 字体文件
- 路径（TF 卡）：`/font/霞鹜文楷_大.bin`
- 格式（已逆向，见 `src/edcbook_font.h`）：
  - `[头]` `uint32 char_count` + `uint8 font_height`
  - 索引区：每个字符 → glyph 数据偏移
  - 索引进 PSRAM 加速
- 实测加载日志：
  ```
  [font] /font/霞鹜文楷_大.bin 字符数=21920 字高=36
  [font] 索引加载完成 21920 条
  [font] 加载成功，字高=36
  ```

### 渲染
- `src/edcbook_font.cpp`：`.bin` 解析 + glyph 提取
- `main.cpp` 里 `drawTextEDC()`：UTF-8 解码 → 逐字渲染到 M5Canvas
- `nextCodepoint()` 已改为 public（UTF-8 解码用）

### ⚠️ 关键 bug：灰度黑白反转（已修复）
字体数据是 **4-bit 灰度**：`0=黑，15=白`（注释第 6 行）。
原渲染代码 `main.cpp` 里写的是 `gray = 15 - g`，把黑色（g=0）算成 255（白），导致**所有字画成接近白色、屏幕发虚几乎看不见**。
**修复**（2026-07-17）：去掉反转，直接 `v = g*255/15; color565(v,v,v)`。g=0→纯黑，g 越大越浅。
```cpp
uint8_t g = pix[row*w+col];        // 0=黑, 15=白
if (g >= 15) continue;             // 白=背景跳过
uint8_t v = g * 255 / 15;          // 0(黑)..238(浅灰)
uint16_t color = canvas.color565(v, v, v);
canvas.drawPixel(..., color);
```
> 现象回顾：书架能显示但字极淡发虚。不是字体/波形问题，是这一步颜色映射反了。

### 字体方案踩坑记录
1. LovyanGFX 自带 efont 有日文/部分 CJK，但**中文不全** → 弃用
2. TTF 渲染需要 FreeType，固件未编译 → 弃用
3. VLW 需转换工具链，且用户想要现成霞鹜文楷 → 弃用
4. **最终选 EDCBook .bin**：用户已有、效果好、格式已逆向

---

## 五、微信读书接口（关键）

### 登录态（cookie）
存 SPIFFS `/config.json`，字段：
```json
{
  "wifi_ssid": "office2",
  "wifi_pass": "<WIFI密码>",
  "wr_vid":  "3800183",
  "wr_skey": "<wr_skey>",
  "wr_rt":   "<wr_rt>",
  "api_key": ""
}
```
**当前有效登录态（2026-07-17 扫码获取）：**
```
wr_vid  = 3800183
wr_skey = <wr_skey>
wr_rt   = <wr_rt>
wr_ql   = 0
```
（同步保存在项目根 `weread_cookies.json`，cookie 有效期约 30 天，过期需重扫）

### 接口清单
| 功能 | 方法 | URL | 鉴权 |
|------|------|-----|------|
| 书架 | GET | `https://weread.qq.com/web/shelf/sync` | cookie |
| 书籍信息 | GET | `https://i.weread.qq.com/book/info?bookId=` | cookie |
| 目录章节 | POST | `https://weread.qq.com/web/book/chapterInfos` | cookie |
| 正文 | GET | reader HTML → 提取 psvts → 拉章节 | cookie + 签名 |

### ⚠️ 关键坑：书架接口有两个，别用错
- ❌ `https://i.weread.qq.com/shelf/sync` → **401 登录超时**（即使 cookie 正确！需要额外签名）
- ✅ `https://weread.qq.com/web/shelf/sync` → **200 正常**（cookie 明文即可）

> 设备端 `weread_api.cpp` 用的是正确的 `/web/shelf/sync`。
> 调试时我误用 `i.weread.qq.com/shelf/sync` 测 cookie，导致误判"cookie 无效"，浪费多轮扫码。**测 cookie 有效性必须用 `/web/shelf/sync`。**

### -2012 错误
`{"errcode":-2012,"errmsg":"登录超时"}` = cookie 过期或接口错。先确认接口是 `/web/shelf/sync`，再确认 cookie 新鲜。

### ⚠️ 关键坑：HttpResponse 的响应体在 PSRAM，不在 `r.body`（已修复）
`weread_client.cpp` 的 HTTP 层**所有**响应数据都写进 PSRAM 缓冲（`psbody/pslen`），`resp.body` 这个 String **永远是空的**。读响应必须用访问器：
```cpp
deserializeJson(doc, r.data(), r.length());   // ✅ data()/length() 自动优先 PSRAM
deserializeJson(doc, r.body);                  // ❌ 解析空串 → "JSON 解析失败"
```
- 曾因此导致 `chapterInfos JSON 解析失败`（书架正常只是因为 `getBookshelf` 用了 `data()`）。
- `HttpResponse` 已改为**移动语义 + 析构自动释放 PSRAM**（禁止拷贝），同时修掉每请求泄漏 512KB PSRAM 的问题。
- PSRAM 缓冲在 `request()` 里已补 `\0` 结尾，`data()` 可安全 `strstr`（如 `extractPsvts(const char*, size_t)`）。
- **新增任何接口时，一律用 `r.data()`/`r.length()`，不要碰 `r.body`。**

---

## 六、扫码登录流程（`weread_qr_login.py`）

复刻 KOReader 插件 `login-qr.lua` 流程：

1. `GET /api/auth/getLoginUid` → 拿 `uid`
2. 生成确认页二维码：`https://weread.qq.com/web/confirm?uid=<uid>`
3. 轮询 `GET /api/auth/getLoginInfo?uid=<uid>` 直到 `succeed=true`
4. 登录成功响应的 **set-cookie** 里取 `wr_skey` / `wr_vid` / `wr_rt`

### 运行
```bash
cd ~/papers3-weread
python3 weread_qr_login.py
# 用微信 / 微信读书 App 扫 weread_login_qr.png
# 成功后 cookie 存到 weread_cookies.json 并自动验证书架
```

### 登录脚本关键点
- **wr_skey = accessToken**（两者相同，都是 8 位随机串）
- 必须从登录成功响应的 **set-cookie header** 或 cookie jar 取，且 cookie 值要全部转成 **str**（`wr_ql:0` 若为 int 会导致 requests 崩溃）
- 脚本第 6 步会**自动调 `/web/shelf/sync` 验证**，直接告诉你 cookie 有没有效，不用烧设备试

---

## 七、写入设备配置（串口 CFG）

设备启动后监听串口，发一行 JSON 写 `/config.json`：

```
CFG {"wifi_ssid":"office2","wifi_pass":"<WIFI密码>","wr_vid":"3800183","wr_skey":"<wr_skey>","wr_rt":"<wr_rt>","api_key":""}
```

设备响应 `[cfg] 已写入 /config.json，重新加载...` 后自动重连 WiFi + 用新 cookie。

> 注意：CFG 是**整体覆盖**写入，WiFi 和 cookie 必须一起发（没有单独更新 cookie 的命令）。

---

## 八、编译 / 烧录

```bash
cd ~/papers3-weread
export PATH="$HOME/.local/bin:$PATH"

# 编译
pio run

# 烧录
pio run -t upload --upload-port /dev/cu.usbmodem2101

# 看日志（macOS 无 timeout 命令，用 python pyserial）
python3 -c "import serial,time; s=serial.Serial('/dev/cu.usbmodem2101',115200,timeout=1); time.sleep(0.3); [print(s.readline().decode('utf-8','replace').rstrip()) for _ in range(200) if s.in_waiting or time.sleep(0.1) is None]"
```

### framework
`arduino, espidf` 混合框架（代码用了 ESP-IDF 原生 API：`esp_http_client`、`nvs_flash`、`esp_heap_caps` 管理 PSRAM，**不能去掉 espidf**）。

---

## 九、PlatformIO 环境踩坑（重要）

### 磁盘满导致编译假死
- 症状：`pio run` 卡住、无 xtensa 编译进程、`.o` 数量不涨
- 原因：之前磁盘满写坏 scons 状态（`.sconsign*.dblite`）
- 修复：删 `.pio/build/PaperS3/.sconsign*.dblite`（保留 `.o`），重跑增量编译

### RISC-V 工具链
- espidf 混合框架**强制要求** `toolchain-riscv32-esp`（即使 S3 是 Xtensa 用不到）
- 不能从 `platform.json` 删掉它（espidf 构建脚本 `self.packages["toolchain-riscv32-esp"]` 会 KeyError）
- 下载慢/卡：走代理约 1MB/s，直连很慢；可手动多线程分段下载 187MB tar + 解压到 `~/.platformio/packages/toolchain-riscv32-esp/`
- 手动放置需补 `package.json` + `.piopm`，但更稳的是让 PlatformIO 自己下（走代理）

### M5GFX 全量编译慢
- M5GFX 有几个 30 万行的字体 `.c`，全量编译 7-10 分钟
- 增量编译只重编改动的文件，快很多

---

## 十、待办（下一步，交接给后续 agent）

### 已完成（2026-07-17）
- [x] **选书联调**：串口发书序号 → 拉目录 → 拉正文 → EDC 字体显示，真机通过
- [x] **章节选择**：串口格式 `书序号:章序号`（如 `38:4`），不发 `:` 默认第 1 章
- [x] **正文翻页**：分页 + 页码，串口 n/p 或触摸翻页；章末再翻自动进下一章，章首回翻回上一章
- [x] **触摸**：独立任务 10ms 采集（参考 M5ReadPaper），`wasPressed` 落指即响应；左半=上页/右半=下页/顶部=回书架；书架点行选书、底部左右翻书架页；跨屏 `xQueueReset` 丢弃残留点按
- [x] **快刷**：翻页 `epd_fast` + **异步推送不 waitDisplay**（`epd_flush_async`，阻塞是之前 3-4 秒延迟的根因）；每 8 页 quality 全刷清残影；`setEpdMode` 必须在 `pushSprite` 之前调
- [x] **配网门户**：无配置/WiFi 失败自动开热点 + captive 弹窗 + 网页选 WiFi 输密码；成功存 SD 重启，失败显示中文原因；入口：自动 / 书架右上"WiFi" / 串口 `wifi`
- [x] **扫码登录**：cookie 过期自动弹 QR，手机扫码即登录（真机验证首轮轮询即 LOGIN_SUCCESS）；入口：自动 / 书架右上"登录" / 串口 `login`
- [x] **SD 存储**：配置 `/weread/config.json`（SD 优先 SPIFFS 回退）；章节正文缓存 `/weread/cache/`，命中秒开不走网络

### 后续增强
- [ ] 目录上屏选章（当前靠串口 `:章序号`，需先知道章号）
- [ ] 正文排版优化（版心边距、行距，config.h 已定义 540x960 竖屏 + margin 36/56）
- [ ] 阅读进度记忆（bookId → 章/页，存 SD）
- [ ] 墨水屏睡眠/唤醒（GT911 INT=GPIO48 可作 light-sleep 唤醒源，参考 M5ReadPaper setup.cpp）
- [ ] 目录缓存到 SD（目录结构很少变，可省一次网络往返）

### 已知小坑
- `strip_xhtml` 不去 `<style>` 块：epub 版权页等前几张会显示一小段 CSS 文本（正文页无影响）
- `show_chapter_text` 标题截断到 14 字，长书名显示不全
- 配网门户是开放热点（无密码），配网窗口期理论上附近任何人可连——家用可接受，介意可加 AP 密码

---

## 十一、快速恢复清单（换电脑/重来时）

1. TF 卡放 `/font/霞鹜文楷_大.bin`（EDCBook 字体）
2. 串口发 CFG 写入 WiFi + cookie（见第七节）
3. cookie 过期 → 跑 `weread_qr_login.py` 重扫（第六节）
4. 编译卡死 → 删 `.sconsign*.dblite`（第九节）
5. 验证 cookie → 必须调 `/web/shelf/sync` 不是 `i.weread.qq.com`（第五节）
6. 屏幕字发虚 → 检查是否黑白反转 bug 回潮（第四节，g=0 应为纯黑）
7. 点按无反应/连翻 → 检查是否在阻塞翻页时丢触摸（`waitDisplay` 只能用在非阅读页；翻页必须走 `epd_flush_async`，触摸必须在独立 10ms 任务里，见第十三节）

---

## 十二、EPD 刷新与触摸架构（M5GFX 0.2.20 调研结论）

> 来源：`m5stack/M5PaperS3-UserDemo`（已克隆到 `ref/`）、`ref/M5ReadPaper`、`.pio/libdeps/PaperS3/M5GFX@0.2.20` 源码。
> 注意 libdeps 下还有一份 M5GFX 0.2.25，**参与编译的是 0.2.20**（platformio.ini 锁定），行号以 0.2.20 为准。

### 刷新模式（`epd_mode_t`，enum.hpp:44）
| 模式 | 帧数 | 灰度 | 消影相 | 用途 |
|------|------|------|--------|------|
| `epd_quality` | ~31 | 16 级 | 有 | 全刷/书架/章首页，~1.7s |
| `epd_text` | ~31 | 16 级 | 有 | **不比 quality 快**，别当中间档 |
| `epd_fast` | ~9 | 1bit | 无 | **翻页首选**，~650ms |
| `epd_fastest` | ~6 | 1bit | 无 | 最快但残影最重 |

- fast/fastest 无消影相 → 残影累积，**必须每 N 页插一次 quality 全刷**（当前 N=8）。
- `setEpdMode` 立即生效于下一次刷新，**必须在 `pushSprite` 之前调**（写入时按模式决定 1bit/4bit 抖动）。
- `pushSprite` 自动触发异步刷新（panel 后台 FreeRTOS 任务播波形），**不要再调 `display()`**；`waitDisplay()` 等的是波形物理播完。
- 0.2.20 的 `display(x,y,w,h)` 窗口不做旋转换算且**不省时间**（每帧仍扫全屏）→ 别用局部刷新，只靠 fast 波形提速。
- 开机必须一次全屏 quality 刷新同步面板状态（setup 已做）。

### 触摸（GT911，驱动在 M5GFX）
- `M5.begin` 后触摸直接可用，无需校准；坐标随 `setRotation` 自动映射为 540x960 逻辑坐标。
- 事件读法：`M5.update()` 后 `M5.Touch.getDetail()`；`wasPressed()`=落指（快，推荐），`wasClicked()`=抬手（慢一拍）。
- **GT911 超 128ms 不读会丢数据** → `M5.update()` 必须在独立任务里 10ms 周期调用（本固件 `touch_task`），主循环渲染/刷新阻塞再久也不丢点按。
- PaperS3 **没有物理按键**（BtnA/B/C/PWR 均不可用）；睡眠唤醒可用 GT911 INT=GPIO48 低电平触发。

---

## 十三、配网门户与扫码登录（2026-07-17 新增）

### 模块划分
- `src/provision.h/.cpp` — 配网门户。`run_provisioning_portal(ui回调, boot_error)`：**阻塞，成功后 ESP.restart 永不返回**。AP `PaperS3-阅读器`（开放）+ DNSServer 劫持（captive 弹窗）+ WebServer(80)。路由：`/` 页面、`/scan` 异步扫描、`/connect` 开始连接、`/status` 状态轮询。WiFi 事件捕获断开原因码映射中文（201=找不到网络、15=密码错误、202/203=信号/认证超时…），15 秒超时。
- `src/weread_login.h/.cpp` — 扫码登录。`weread_login::run(render_qr回调, msg回调, timeout)`：getLoginUid → render_qr（main.cpp 用 qrcodegen 画 QR）→ 2s 轮询 getLoginInfo → 成功兜底 setCookie + `WR.saveCookiesToConfig()`。
- `src/qrcodegen.hpp/.cpp` — vendored nayuki/QR-Code-generator（MIT）。**坑：`Ecc::LOW/HIGH` 与 Arduino `LOW/HIGH` 宏冲突** → main.cpp include 前 `#undef LOW/HIGH`；库用 throw → platformio.ini 加 `-fexceptions` + build_unflags `-fno-exceptions`。
- `src/storage.h/.cpp` — 存储抽象：配置 SD 优先、SPIFFS 回退（`open_config_read/write`）。配网/登录/串口 CFG 三个写配置入口都走它。

### 启动流程（main.cpp setup）
```
无配置 → 5s 串口 CFG 窗口 → 配网门户
WiFi 失败 → 配网门户（boot_error 显示原因）
书架 -2012 → do_qr_login()（失败/超时等点按重试，2 分钟无操作放弃）
其它书架错误 → 点按重启
```

### ⚠️ 新坑（都是真机踩出来的）
1. **AP 起来但客户端自分配 169.254.x.x（DHCP 没发地址）**：进门户前必须 `WiFi.disconnect()` 停掉 STA 残留连接尝试 + `softAPConfig` 显式配网段 + `delay(500)` 等 dhcpd。
2. **-2012 字段名有两种**：`errcode` / `errCode`（实测服务器返回的是驼峰 errCode）→ 检查要兼容两种；且书架 filter 里**两个都要加**，漏了会被过滤掉导致误判。
3. captive 弹窗在 Mac 上 `networksetup -setairportnetwork` 找不到中文 SSID 的热点属正常（编码/扫描缓存），手机端正常。

---

## 十四、交接速览（给下一个 agent）

**一句话现状**：零电脑可用——开机自动连 WiFi（无配置/失败开热点网页配网，手机扫码登录微信读书），书架触摸选书、正文分页快刷翻页、章节缓存 SD。剩下的是体验增强（目录上屏 / 排版 / 进度记忆 / 睡眠唤醒）。

**立即可做**：
```bash
cd ~/papers3-weread && export PATH="$HOME/.local/bin:$PATH"
# 串口命令：n/p 翻页、s 回书架、wifi 进配网门户、login 进扫码登录、38:4 选书选章
python3 -c "import serial,time; s=serial.Serial('/dev/cu.usbmodem2101',115200,timeout=1); time.sleep(0.5); s.write(b'38:4\n'); s.flush(); time.sleep(2); [print(s.readline().decode('utf-8','replace').rstrip()) for _ in range(60) if s.in_waiting or time.sleep(0.1) is None]"
```

**关键文件**：
- `src/main.cpp` — 启动流程/UI/渲染/触摸/分页/缓存（drawTextEDC 是核心，黑白映射已修正）
- `src/provision.cpp` — 配网门户（AP + captive DNS + Web 页）
- `src/weread_login.cpp` + `src/qrcodegen.*` — 扫码登录 + QR 生成
- `src/weread_client.cpp` / `src/storage.cpp` — HTTP/cookie（PSRAM 规则！）/ SD 配置存储
- `weread_qr_login.py` — 电脑端登录备用（cookie 存 weread_cookies.json）

**当前有效登录态**：2026-07-17 设备端扫码登录成功，cookie 存 SD `/weread/config.json`（约 30 天有效）

**已解决的坑**：① 字体黑白反转 ② 书架接口域名（i.weread vs weread.qq.com/web）③ 响应体在 PSRAM 不在 `r.body` ④ AP DHCP 自分配 169.254 ⑤ -2012 字段名 errcode/errCode + filter 过滤。详见第四、五、十三节。
