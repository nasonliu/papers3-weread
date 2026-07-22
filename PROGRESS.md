# PaperS3 微信读书阅读器 — 项目进度 / 交接文档

> 最后更新：2026-07-22（UI 大改：状态栏图标统一 + 弹出菜单全局化 + 版式页胶囊样式 + 布局优化，v0.2.9）
> 状态：**功能全链路可用并已发布 v0.1.0**（GitHub: nasonliu/papers3-weread）。v0.2.9 完成 UI 全面改版（状态栏/弹出菜单/版式设置/书架布局/详情页布局）。**ESP32 访问 weread 接口间歇性 TCP 连接失败的底层根因仍未闭环；v0.2.8 已消除同步网络重试放大成数分钟界面"卡住"的问题**（见第〇节）。

---

## 〇、当前最大难点：设备端 weread 接口间歇性 TCP 连接失败

### 现象
- 设备访问 weread.qq.com 各接口（chapterInfos / reader / e_0/e_1/e_3 / book/info / review/list）**间歇性失败**：`HTTP_CLIENT: Connection failed, sock < 0`、`esp-tls: select() timeout`、`Software caused connection abort`、`delayed connect error`。
- 概率性：同一小时内有成功有失败；**时间维度上午正常、下午开始恶化**。
- 历史观察是 Mac 与 reMarkable 基本稳定，但本轮发现 Mac 开着本机 HTTP/HTTPS/SOCKS 代理（`127.0.0.1:7897`），即使 curl 禁用显式代理，目标路由仍经 `utun`/`198.18.0.1`。因此 Mac **不是与 PaperS3 同一条直连路径的有效对照**；本轮也没有确认 reMarkable 的实际出口路径，不能据此断言家用路由、出口 IP 或服务器端一定无关。

### 已排除的假设（都做过修复/验证）
| 假设 | 结论 |
|------|------|
| cookie 过期 | ❌ 设备会自动 renewal 续期，日志可见成功；且失败发生在 TCP 层（连接都没建立），未到鉴权 |
| 本地 DRAM 不足（mbedtls -0x7F00） | ✅ 已修：mbedtls_platform_set_calloc_free 改 PSRAM 分配，未再出现 |
| 本地 socket 耗尽（LWIP_MAX_SOCKETS=10） | ⚠️ 本轮未复现。生产 API 连续 20 次全部成功，内部 DRAM 在前几次下降后稳定，未见持续逐次下降；这只能排除本轮窗口内的持续泄漏，不能排除长时间运行后的状态问题 |
| 请求过快触发风控 | ❌ 已加节流（分片 ≥400ms、图片 ≥300ms、预取任务间隔 800ms），失败率无明显改善 |
| WiFi 省电模式吞包 | ✅ 已修：WiFi.setSleep(false)，但连接失败仍在 |
| TLS 会话被 housekeeping 误关 | ✅ 已修：keep-alive 会话加在途计数，仅在途=0 且闲置 30s 才关 |

### 与 reMarkable 项目的实现对照

| 项目 | 网络栈与调用方式 | 对本问题的意义 |
|------|------------------|----------------|
| reMarkable `rm webook` | 完整 Linux 网络栈；Qt 侧用 `iw`/`ip`/`wpa_cli` 管理网络，HTTP 内容请求由 KOReader Lua helper / Linux 系统栈完成 | 网络、DNS、TCP/TLS 资源余量大，单次失败通常不会冻结整个 UI；但本轮未确认它与 PaperS3 走相同出口 |
| PaperS3 | ESP-IDF 4.4.7 / Arduino-ESP32 2.0.17，lwIP + mbedTLS + `esp_http_client`；普通 API 每次新建/销毁 client，正文分片才复用 `ShardSession` keep-alive | 普通 API 频繁做 DNS/TCP/TLS，新握手慢、暴露故障窗口多；同步调用和叠加重试会直接阻塞 UI |

### 2026-07-18 诊断固件与实机实验

本轮只增加诊断入口，没有修改生产网络策略：`src/network_diag.cpp/.h`，以及 `src/main.cpp` 中两条串口命令。命令均持有 `g_net_mtx`，不会输出 cookie、正文或书架内容。

- `netdiag [N]`：测试 DNS、TCP:443、weread 新建 TLS、`www.qq.com` 对照、新建/复用 TLS 及堆内存。
- `netapi [N]`：调用现有账户下的 `/web/shelf/sync` 生产请求路径，只打印状态码、响应长度、耗时和内存。
- PlatformIO 编译通过：RAM 27.5%（89952/327680），Flash 14.2%（1994557/14090240）；诊断固件已成功烧录真机。

| 实验 | 实机结果 | 结论边界 |
|------|----------|----------|
| 冷启动基线 | WiFi 信道 9，RSSI 约 -44~-51 dBm，书架成功加载 115 本 | 本轮不支持“单纯信号弱”假设 |
| `netdiag 10` | weread DNS/TCP 10/10；对照域名 TCP 10/10；weread 新建 TLS 10/10；对照域名新建 TLS 10/10；weread 复用 TLS 10/10 | 本轮没有复现底层网络失败；新建 TLS 约 0.76~2.67s，复用后约 43~72ms，说明连接复用可显著缩短延迟和故障暴露窗口，但不能单独证明根因 |
| `netapi 20` | 20/20 成功；每次响应 157450 bytes，约 2.16~2.40s | 内部 DRAM 从约 59.7KB 降至约 56.4KB 后稳定，最大块 34KB；未发现持续逐请求泄漏或 socket 耗尽 |
| 3 本书末段全链路抽测 | 3/3 成功，耗时约 3.0s / 17.4s / 31.2s，相关 TCP/TLS 错误计数为 0 | 含缓存与真实网络混合样本，只能证明当时窗口可用 |
| 扩展矩阵 | 已完成的两个目标均成功（一个缓存命中 0s，一个约 17.5s），随后按用户要求停止 | 没有出现可用于定位的失败样本 |
| 下载《伊朗五百年》 | 28 章完成 27 章；新拉取 25 章；固件计时 165s（主机约 168s）；`shard_rebuild`、TCP/TLS/select timeout/software abort/delayed connect 均为 0 | 长连接下载窗口稳定。第 1 章失败属于“非 HTTP 分类”，未取得更细子类型，不能把它直接归为 TCP 失败 |
| 再次补缺章 | 120s 内没有收到下载完成，设备界面表现为“卡住” | 成功复现用户看到的长时间无响应，但串口重新打开会重置设备，未能在保持现场的情况下取得阻塞栈 |
| Mac curl 对照 | 20/20 成功，但默认 `remote_ip=127.0.0.1`；禁用显式代理后仍经系统 `utun`/`198.18.0.1` 路由 | 只能证明代理/TUN 路径当时稳定，不能证明 PaperS3 的直连路径稳定 |

### 为什么刚才都成功、后来又卡住

- **这轮没有修复生产网络逻辑。**“后来都成功”主要因为当时网络窗口本身稳定，正文走 keep-alive，而且多次串口重连都重置了设备，清掉了 TCP/TLS/session/堆的运行态；后续缓存命中也减少了真实请求。这些都会暂时降低失败率。
- macOS 用 pyserial 打开 `/dev/cu.usbmodem2101` 会触发 `rst:0x15 (USB_UART_CHIP_RESET)`，即使设置 `dtr=False` 也会重置。网络问题复现时必须**一次打开串口持续采集**；反复重连会改变现场并掩盖问题。
- “卡住”的放大机制已经由代码确认：单次 HTTP 超时是 20s；分片请求内部可重试一次（最多约 40s）；一章还会顺序请求 reader、e_0、e_1、e_3；`download_all()` 在失败后又等待 2s 并重试整章。取消只在章节之间检查，无法中断正在执行的 `esp_http_client_perform()`。因此底层只要连续超时，UI 就可能数分钟不响应。
- 串口重置后设备正常启动，WiFi、书架和阅读恢复均成功，`s` 命令可用；没有看到崩溃 backtrace，也没有删除账户或缓存数据。因此本次“卡住”更符合阻塞等待而非永久死机或数据损坏。

### 当前判断与下一步

- 历史上的 `mbedtls_ssl_setup -0x7F00` 本地 DRAM 问题已经由 PSRAM allocator 修复；本轮未再出现。
- ~~尚不能把剩余间歇性 TCP 失败归因到单一因素。当前较可信的方向是：ESP32 的旧版 lwIP/mbedTLS 直连路径，加上普通 API 频繁新建 TLS，放大了偶发路由/TCP/TLS失败；手机热点 A/B 仍是必要的路径对照。~~
- **v0.2.8 已修普通 API 连接复用**：`WereadClient` 改 keep-alive（同 host 复用一条 TLS），握手从每请求 1-2s 降到整会话 1 次；失败重建重试一次；闲置 30s 关闭；`in_use` 防护。netapi 10/10 全过（~1.6s/轮，纯传输时间，无握手抖动）。
- **v0.2.8 已修 UI 假死**：整章 90s 总预算（`g_chapter_deadline`）；`download_all` 去掉外层整章重试（内层 `request_retry` 已够）；点取消置 `g_net_cancel`——进行中的 HTTP 完成后后续请求不再发起（最坏堵 1 个 20s 超时，不再数分钟）。
- 下次真实失败窗口应先保持串口持续打开，依次跑 `netdiag 20`、`netapi 20` 并记录错误类别、耗时和内存，再决定是否重置。
- 再在相同固件、相同测试命令下做家用 WiFi vs 手机热点 A/B；热点稳定只能说明路径/路由相关，热点同样失败才继续集中查 ESP32 TLS/lwIP。

### 受影响的功能 vs 不受影响
- **不受影响（全好）**：SD 缓存过的书离线阅读、翻页（0.4-0.9s）、字体、休眠、配网门户、扫码登录、书架（缓存命中时）
- **已改善（v0.2.8）**：新书的目录/详情/正文/插图/下载整本/进度上传——网络窗口期仍可能失败，但**不再因叠加重试表现为长时间界面无响应**；下载整本点取消后 20s 内响应
- **仍受影响**：底层间歇性 TCP 失败根因未定位（待 A/B 热点对照）

---

## 一、项目目标

把微信读书 Web 接口移植到 **M5Stack PaperS3**（ESP32-S3 + 4.7 寸墨水屏 ED047TC1，960x540）：
配网 → 登录 → 书架 → 详情 → 正文 → 墨水屏中文渲染 → 进度同步。

协议蓝本：`ref/weread.koplugin`（KOReader 插件，注意：**上游无明确 LICENSE**，weread_crypto 是其移植）
固件参考：`ref/M5ReadPaper`、`ref/M5PaperS3-UserDemo`
发布仓库：https://github.com/nasonliu/papers3-weread （v0.1.0 已发，含 full.bin/app.bin；M5Burner 收录材料已备但官方收录仓库已归档，需论坛求收录）

---

## 二、当前已完成（里程碑）

| 模块 | 状态 | 说明 |
|------|------|------|
| 编译/烧录 | ✅ | PlatformIO，Flash 占 ~14% |
| 配网门户 | ✅ | 无配置/失败自动开 AP + captive 弹窗 + 网页选 WiFi，失败显示中文原因 |
| 扫码登录 | ✅ | 设备弹 QR 手机扫码；**令牌自动续期**（wr_rt 换 wr_skey，请求层 -2012 自动续+重发，开机主动续，轮换 cookie 定期落盘） |
| 书架 | ✅ | 封面 5 本/页、最近阅读排序（bookProgress.readUpdateTime）、进度百分比、相邻页封面预取 |
| 书籍详情 | ✅ | 封面/标题/作者/出版社/简介/热门评论（listType=3）；继续阅读（服务器章级+本地页级）；下载整本（进度条+取消）；**插图开关（按书记忆）** |
| 正文阅读 | ✅ | 图文混排（文本+图片块游标分页）、原书 h1-h4 样式（h1 2x+仿粗 h2-h4 仿粗）、首行缩进 2 中文字符、段间距 lh/4、版心居中（左右各 30px） |
| 图片 | ✅ | JPEG/PNG/GIF 解码灰度上屏（TJpg/PNGdec/AnimatedGIF）、SD 缓存、keep-alive 下载、负缓存（会话级）、**插图开关** |
| 目录屏 | ✅ | 正文左上角进入，翻页/选章/返回 |
| 快刷翻页 | ✅ | epd_fast 异步推送 ~260ms；字形缓存后翻页 0.4-0.9s；每 8 页全刷清残影 |
| 进度同步 | ✅ | 上传 POST /web/book/read（koplugin 签名 payload）；本地 SD progress.json；后台任务上传（30s 节流） |
| 后台任务 | ✅ | net_worker：预取下一章（bookId 校验防脏配对）、相邻页封面、本章插图、进度上传；g_net_mtx 递归锁 |
| 休眠 | ✅ | 浅睡（书架"睡眠"/闲置 5min，充电中不睡）+ 摸屏秒醒原地续跑；电源键硬重启自动恢复阅读页（state.json 每次翻页更新） |
| 字体 | ✅ | 双字体（UI 固定霞鹜文楷 / 正文可换）；字体页逐字体预览+整屏缓存；换字体即重排 |
| 版式设置 | ✅ | v0.2.9：行距/段距/首行缩进 3 档可调（黑色胶囊样式，× 按钮关闭才重排）；持久化复用 config.json |
| 全局状态栏 | ✅ | v0.2.9：右上角统一三图标（电源⏻/WiFi/电量），所有屏幕最右；点图标弹出全局菜单（睡眠/WiFi/登录） |
| 全局弹出菜单 | ✅ | v0.2.9：书架/阅读/详情/版式/目录 全屏通用，点状态栏图标弹出 |
| 缓存 | ✅ | 按书分目录 `/weread/cache/<bookId>/`（toc.json/detail.json/ch_uid.blk），B7 格式（含 style） |
| 测试 | ✅ | `test_matrix.py` 全流程矩阵（详情/目录/选章/打开/翻页/下载整本计时） |

---

## 三、硬件引脚（PaperS3）

- SD（SPI）：CS=47 SCK=39 MOSI=38 MISO=40
- 屏幕：物理 960x540，竖屏逻辑 540x960；M5GFX 自动识别 board_M5PaperS3
- 串口：/dev/cu.usbmodem2101 @115200
- 电源键：**软件不可读**（接 PMS150G 电源管理芯片，原理图+社区三方实证）；GPIO44 脉冲可主动断电；休眠只能用浅睡+触摸唤醒或断电+状态恢复
- USB 检测：GPIO5(USB_DET) **浮空不可靠**（实测恒读 0），充电检测用 `M5.Power.isCharging()`（GPIO4 CHG_STAT）

---

## 四、中文字体（双字体架构）

- **UI 字体** `g_ui_font`：固定 `/font/霞鹜文楷_大.bin`，菜单永不缺字
- **正文字体** `g_read_font`：配置项 `font`，阅读页内切换，切换即重排；加载失败回滚文楷
- 渲染核心 `drawTextFontScaled(font,...,scale)`（scale 1/2 放大）；`drawUI/drawRead/drawReadStyle`；仿粗体 = x+1 二次绘制
- **EdcFont::load 重载必须清空 entries_ 和字形缓存**（否则换字体无效）
- **SD `e.name()` 只返回裸文件名**（不带目录），拼路径必须自己加 `/font/` 前缀
- 字形解码缓存 1.5MB PSRAM（翻页提速主因：600 字/页 × 4ms SD 读 → 命中免读）
- 灰度映射：`0=黑..15=白`，直接 `v=g*255/15`，**不要反转**

---

## 五、微信读书接口（关键）

### 接口清单（全部实测，走 weread.qq.com/web/*，cookie 明文）
| 功能 | 方法 | URL |
|------|------|-----|
| 书架+进度 | GET | `/web/shelf/sync`（books[] + bookProgress[]） |
| 目录 | POST | `/web/book/chapterInfos` |
| 详情 | GET | `/web/book/info?bookId=`（无作者简介字段） |
| 评论 | GET | `/web/review/list?listType=3`（推荐长评；1=好友流 2=最新） |
| 进度上传 | POST | `/web/book/read`（koplugin 全套签名：appId/b/c/ci/co/sm/pr/rt/ts/rn/sg/ct/ps/pc+s） |
| 正文 | reader HTML 取 psvts → POST `/web/book/chapter/e_0/e_1/e_3`（epub）或 `t_0/t_1`（txt） |
| 续期 | POST | `/web/login/renewal`（body `{"rq":"%2Fweb%2Fbook%2Fread","ql":false}`） |
| 图片 | CDN 直链（cdn/res.weread.qq.com、myqcloud）；res.weread 需 cookie（401→302 签名 CDN） |

### 关键规则
- ❌ `i.weread.qq.com/*` 要额外签名（401），新接口全走 web 域名
- **-2012 字段名有 errcode/errCode 两种**（实测是驼峰），filter 里两个都要留
- **cookie 会被服务器轮换**：absorbSetCookie 检测变化置脏，主循环 60s 写回；wr_skey 短效（几小时）靠 wr_rt 续期
- **正文 `../Images/*` 相对图是 Web 阅读器 UI 装饰图**（tar 包里没有），跳过；CDN 绝对图才是插图
- **分片禁止用 String 持有**（单片可超 64KB 必失真），HTTP 接收→解码全程 PSRAM 指针（steal_response_data 零复制）

### ⚠️ swap 解码算法（血泪教训）
原算法（weread.lua）：`tonumber(bin(byte), 4)` = **bit 展开**（第 b 位 → 4^b = 2^(2b)）。曾错写成"2 位二进制转一个四进制数字拼接"，导致每章几字节错位（正文看不出，URL 被砸中就 404）。正确实现：`val += 1U << (2*b)`。Mac 侧用同一章真实分片验证过两种实现逐字节差异。

---

## 六、EPD 刷新与触摸架构（M5GFX 0.2.20）

- `epd_quality`(~31帧,16灰,有消影,~1.7s) / `epd_text`（同帧数别用） / `epd_fast`(~9帧,1bit,无消影,~650ms,翻页首选) / `epd_fastest`(~6帧)
- `setEpdMode` 必须在 `pushSprite` 之前调；pushSprite 自动异步刷新，别再调 display()；`waitDisplay` 等波形播完
- fast 无消影 → 每 8 页一次 quality 全刷
- **触摸：独立任务 10ms 采集**（GT911 超 128ms 不读丢数据），wasPressed 落指入队；坐标随 rotation 自动映射 540x960；跨屏 xQueueReset
- 0.2.20 的 display(x,y,w,h) 窗口不做旋转换算且不省时，只用无参刷新

---

## 七、休眠与恢复

- **浅睡**（主路径）：`M5.Power.lightSleep(0,true)`（GPIO48 触摸 INT 唤醒），EPD 断电图像保持；醒后 `M5.Display.wakeup()` + WiFi 重连 + NTP 重校时 + 重绘当前屏
- **断电恢复**：每次翻页写 `/weread/state.json`（bookId/c/p/title/cover/format），开机 `try_restore_reading()` 优先走 TOC/正文缓存秒回阅读页（不依赖书架）；显式回书架时删除标记
- **浅睡时主循环冻结**：定时器类逻辑（续期/落盘）不会跑，无唤醒耗电问题
- 闲置 5 分钟自动浅睡；**充电中（isCharging）不睡**

---

## 八、多任务与内存规则（连环崩溃的教训）

- **net_worker**（prio 1, stack 12K）：预取下一章（PrefetchJob 带 bookId 校验）/相邻页封面/本章插图/上传进度；与主任务共用 ShardSession/img 会话/psvts 缓存/g_blocks → **所有 weread_api/img_render 调用必须持 g_net_mtx 递归锁**
- **keep-alive 会话都有在途计数（in_use）**：housekeeping（主循环 5s）只在 in_use==0 且闲置 30s 才关——**曾经因为在途关句柄导致伊朗五百年预取崩溃循环**（addr2line 实锤）
- **进度屏只许主任务画**（g_stage_ui_ok 门）：曾两任务共画画布崩溃
- **mbedTLS 分配走 PSRAM**（platform_set_calloc_free，否则 DRAM 满 → -0x7F00 → 请求发不出）
- 大缓冲一律 PSRAM；String 超 64KB 会坏（大章 xhtml/解码全程 PSRAM，单文本块 ≤48KB）

---

## 九、缓存与测试

### SD 目录
```
/font/*.bin                    字体（EDCBook，UI 必需霞鹜文楷_大）
/weread/config.json            WiFi+cookie+font
/weread/progress.json          各书页级进度
/weread/state.json             上次阅读位置（重启/断电恢复）
/weread/imgoff.json            各书插图开关
/weread/cache/<bookId>/toc.json / detail.json / ch_<uid>.blk（B7）
/weread/img/                   封面插图缓存（md5 命名 + 会话负缓存）
```

### 串口命令（115200）
`n/p`翻页 `s`书架 `sp/sm`书架翻页 `t`目录屏 `tp/tm`目录翻页 `ts N`选章 `N`详情 `N:C`直读 `dl`下载整本 `wifi`配网 `login`扫码 `imgs`图片URL `fonts`字体自检 `books`打印书架 `usb`充电诊断 `imgoff`插图开关 `netdiag [N]`分层网络诊断 `netapi [N]`生产 API 重复诊断 `CFG {json}`写配置

> 调试注意：macOS 打开 `/dev/cu.usbmodem2101` 会触发 `USB_UART_CHIP_RESET`。取间歇性故障证据时应一次打开并持续记录，不要反复关闭/重开串口。诊断命令不输出 cookie 或正文。

### 测试脚本
`test_matrix.py`：就绪→书单→（详情/目录屏/选章打开计时/翻页×5计时/插图统计）×5 本书→下载整本计时→汇总。设备必须已开机联网。

---

## 十、遗留问题（按优先级）

1. **【P0 已定位机制】同步网络叠加重试造成界面长时间“卡住”**（见第〇节）——整章总超时、单一重试预算、等待过程可取消/有心跳
2. **【P0 底层根因未解】ESP32↔weread 间歇性 TCP 连接失败**——失败现场连续串口取证 + 家用 WiFi/手机热点同固件 A/B
3. **【P1 降低暴露窗口】普通 WereadClient 同 host 连接复用**——失败重建一次、空闲关闭、保留 in_use 防护
4. 插图多的章节下载慢（已缓解：imgoff 开关 + 后台懒加载 + 8s 超时 + 会话负缓存）
5. 个别小 PNG（weread 装饰图）PNGdec rc=7 拒画 → [图片] 占位，不影响正文
6. strip_xhtml 不去 <style> 块：epub 版权页前几页可能露一小段 CSS 文本
7. weread.koplugin 无上游 LICENSE，weread_crypto 是其移植——公开发布的法律注意项（REweread 项目同款问题，其处理是只发非协议代码）
8. M5Burner 官方收录仓库已归档只读，需 M5Stack 论坛求收录（m5burner.json+firmware/ 材料已备）
9. 简介/热门评论缓存已有；详情页剩余 ~4s 在线等待来自这两个接口（可再做后台刷新）

---

## 十一、快速恢复清单

1. 烧录异常/崩溃循环救砖：长按电源键进下载模式（红灯闪）再 pio upload
2. 编译卡死 → 删 `.pio/build/PaperS3/.sconsign*.dblite`
3. 登录态问题 → 先用 `netapi 1` 看 HTTP/接口结果；不要在日志或交接中导出 cookie，服务器轮换值以设备落盘状态为准
4. 目录/正文错乱（书对不上章）→ 清 `/weread/cache/` 对应 bookId 目录（脏配对历史坑，现有四层防护）
5. 屏幕字发虚 → 检查灰度映射回潮（g=0 应纯黑）
6. 点按没反应/连翻 → 检查触摸任务与阻塞刷新（翻页必须 epd_flush_async）
7. 文字全不显示 → 检查 UI 文字是否误绑 g_read_font
8. 换字体无效 → 检查 EdcFont::load 是否清空 entries_ + 字体路径是否带 /font/ 前缀

---

## 十二、交接速览（给下一个 agent）

**一句话现状**：产品形态完整可用并已发布（v0.1.0）；本轮分层诊断与长书下载大部分成功，仍没有抓到间歇性 TCP 失败的有效现场，不能宣称网络已修好；但已确认同步超时和双层重试会把偶发失败放大成数分钟界面无响应，应该先修这一 P0 体验与可恢复性问题。

**上手顺序**：
1. 读本文档第〇节（最大难点）+ 第五节（接口规则）+ 第八节（多任务内存规则，连环坑都在这）
2. 串口一次打开后持续记录；先发 `netdiag 20`、`netapi 20` 建立分层基线，不要反复重连导致设备重置
3. 复现网络问题：开没缓存的章节或下载长书；卡住时先记录时间、当前阶段和错误类别，能保持现场就不要先重置
4. 用同一版固件/命令做家用 WiFi 与手机热点 A/B；不要再把走代理/TUN 的 Mac 当作同路径对照
5. 别踩的坑：HttpResponse 必须用 data()/length()、swap 是 bit 展开、load 清缓存、会话看在途计数、SD e.name() 是裸名

**关键文件**：
- `src/main.cpp` — 全部 UI/启动流程/触摸/分页/缓存/休眠（最大文件，~1900 行）
- `src/weread_api.cpp` — 接口层（ShardSession keep-alive + PSRAM 管线 + psvts 缓存）
- `src/weread_crypto.cpp` — 签名/解码（PSRAM 版 + swap bit 展开）
- `src/img_render.cpp` — 图片三格式下载/解码/灰度上屏
- `src/provision.cpp` / `src/weread_login.cpp` — 配网/登录
- `src/network_diag.cpp/.h` — DNS/TCP/TLS/生产 API 分层诊断（不输出账户和内容）
- `test_matrix.py` — 全流程测试矩阵


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

---

## 十五、大书架截断修复 + 失败现场 SD 落盘（2026-07-20，v0.2.3）

用户反馈两类问题：① 配网后"拿不到 uid"（实为 `getLoginUid` 第一步就失败，二维码尚未生成，不是登录态问题）② 扫码后 `shelf JSON 解析失败: IncompleteInput`（书架响应被截断；其中一位用户 500+ 本书）。

**根因与修复**：
1. **512KB 接收缓冲静默截断**（`weread_client.cpp`）：书架 JSON 超 512KB 后字节被静默丢弃 → 解析必报 IncompleteInput。改为按需翻倍扩容（512KB→4MB 封顶）；到顶仍放不下置 overflow → 当传输失败报 `response too large`，绝不把半个 JSON 交给解析。注意扩容后回调可能搬移缓冲，接管所有权必须用 `rb.psbuf`（不是分配时的旧指针）。
2. **书架 JSON 文档固定 64KB**（`weread_api.cpp`）：115 本够用，500 本必 NoMemory。改为按响应长度一半给容量（64KB~1MB，实测过滤后约为输入一半：157KB→64KB）。
3. **截断自动重试**：`json_tail_complete()` 检查响应以 `}` 收尾（TCP 有序，尾巴在 = 一字节没丢），不完整重试一次。
4. **登录报错区分**（`weread_login.cpp`）：传输失败（"网络不通"）/ 服务器拒绝（带 HTTP 状态码）/ 返回异常 分开显示；"无 uid" 分支补上串口日志（旧版该分支无日志，三种失败共用一句"拿不到 uid"）。
5. **SD 诊断日志**：`storage_debug_log()`（`storage.cpp`）把失败现场追加到 SD `/weread/debug.log`（64KB 自转），含错误、len、head/tail 各 ~120 字节。用户拔卡把文件发回来即可定位，不用接串口。

**发布**：`m5burner.json` → 0.2.3；老用户只刷 `papers3-weread-v0.2.3-app.bin`（0x10000，配置/登录态在 SD 不受影响），新用户用 `papers3-weread-v0.2.3-full.bin`（0x0），`firmware/papers3_weread_0x10000.bin` 已同步刷新。编译：RAM 27.5%，Flash 14.2%。

**已知边界**：`shelf/sync` 无分页参数（koplugin API 文档 "Parameters: none"，`lastSort` 游标是 `/user/notebooks` 的），500 本约 1MB 只能全量拉（netapi 实测 ~70KB/s → 约 15s，20s 超时+重试可兜）；书架结果未落 SD 缓存（`PATH_BOOKSHELF` 定义了没用上），每次开机都全量拉取——弱网用户可加"拉取失败回退 SD 缓存"兜底（待做）。

### ⚠️ 本轮实踩的发布/烧录坑（别再犯）

1. **`merge_bin` 打 full.bin 绝不能显式指定 flash 模式**。PaperS3 是八线 PSRAM（qio_opi），flash 实际工作在 **DIO**（pio 产物头部第 2 字节 `0x02`）。`esptool.py merge_bin --flash_mode qio` 会把头部改写成 `0x00`(QIO) → 刷完看门狗死循环：`rst:0x7 (TG0WDT_SYS_RST)` 刷屏、应用零输出（bootloop 期约 1.2s/轮）。正确命令（keep 保留各段原始参数）：
   ```bash
   esptool.py --chip esp32s3 merge_bin -o papers3-weread-full.bin \
     --flash_mode keep --flash_freq keep --flash_size keep \
     0x0 .pio/build/PaperS3/bootloader.bin 0x8000 .pio/build/PaperS3/partitions.bin \
     0x10000 .pio/build/PaperS3/firmware.bin
   ```
   检查方法：`xxd -l 8 full.bin` 第 3 字节必须是 `02`（DIO），是 `00`（QIO）就是坏的。
2. **`write_flash` 显示 "Hash of data verified" ≠ 能启动**。hash 只证明写入与文件一致，不证明镜像可引导。发布前必须把 full.bin **原样刷回真机看一次开机**（本轮坏包就是只看了 hash 没看启动）。
3. **排查 bootloop 先排除合并产物**：直接刷 pio 三件套（`write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin`）能启动 → 问题在合并参数，不在应用代码。
4. **"清空机器"要清两个地方**：`erase_flash` 只擦 Flash（NVS/SPIFFS/app），**擦不到 TF 卡**；WiFi+cookie 在 SD `/weread/config.json`，须串口发 `CFG {}`（loop 里处理，写完回 `[cfg] 已写入`）。只 erase 不清卡，开机照样自动连 WiFi 登录。
5. **串口识别**：PaperS3 插 Mac 是 `/dev/cu.usbmodem2101`（USB Serial/JTAG，ROM 与应用日志同口）。原生 USB 打开串口**不一定复位**设备——设备在主循环静默时打开端口可能无任何输出，发 `s` 等命令试探即可，看到 ROM 刷屏才是在重启循环。
6. **Release 资产只放 `papers3-weread-full.bin`（整片包）**。M5Burner 自定义刷机只能刷单文件整片包，分开的 app.bin 对用户没用，以后 Release 不再放（v0.2.3 起定的规矩；v0.2.3 里那个 app.bin 是最后一批）。打包包法见上面第 1 条。

---

## 十六、500 本书吃光内部 DRAM + ps_malloc 恒 NULL 坑（2026-07-20，v0.2.4）

**症状**（500 本书用户）：书架能进，但封面全"无封面"、点书进不了详情页、进度上传失败——**持续必现**，与网络时段无关。该用户 v0.2.3 之前是"shelf JSON 解析失败"（截断），截断修好后书架终于能加载，才暴露出这个更深的雷。

**确诊路径**（netdiag 是内存类问题的第一指标）：
- `netdiag`：`internal heap=5463`（健康 ~60KB）、RSSI=0、DNS 全灭（仅缓存命中可解析）、TCP connect 瞬间 `EHOSTUNREACH`、WiFi 驱动狂刷 `m f null`（malloc 失败）
- 串口 `books`：书架 489 本在内存里 → 489 × 6 个 Arduino String 字段（书名/作者/封面URL/格式等，String 缓冲只走内部 DRAM）≈ 100-140KB，把 320KB DRAM 吃光 → lwIP/mbedTLS/WiFi 全饿死 → 一切网络操作全挂

**修复**：新增 `PsString`（`weread_api.h`），`BookEntry`（6 个字段）与 `ChapterEntry`（chapterUid/title/tar）全部改 PSRAM 存储；带隐式 `operator String()`，调用点几乎不用改。1281 章长书目录同样受益。

**⚠️ 大坑：Arduino `ps_malloc()` 在本构建（qio_opi）恒返回 NULL**。
真机现象：赋值后 `len=0`、PSRAM 余量纹丝不动（`esp32-hal-psram.c` 里 `ps_malloc` 被 `spiramDetected` 门控，该标志在本构建未置位；而 IDF 层 `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 一直正常——PsramAllocator、HTTP 接收缓冲都走它）。表现为 JSON 解析成功但循环 push 0/489 → "书架为空或解析无 books"。**PSRAM 分配一律用 IDF `heap_caps_malloc`，别用 Arduino 的 `ps_malloc`**（释放用 `heap_caps_free`）。

**真机验证（v0.2.4，500 本账号）**：489/489 入列；internal heap 回升到 78-82KB（比之前 115 本时的 ~60KB 还好，因为书架字符串全走了 PSRAM）；详情页正常打开；《第一序列》1281 章目录加载 OK；netdiag 全绿（DNS/TCP/TLS 全通）。

**版本**：m5burner.json → 0.2.4；发布物按新规矩只有整片 `papers3-weread-full.bin`（merge_bin keep 参数，头部 DIO）。

---

## 十七、错字排查结案 + 书架性能大修（2026-07-20，v0.2.5）

### 错字（个别字符被替换成别的字/拉丁字母）

**结论：不是现版固件的问题。** 用 koplugin Python 参考解码器 + 设备端生产路径（新增 `dbgch <bookId> <chapterUid>` 串口命令直接打印解码文本）双重验证，《抄写员巴托比》第 19 章《译后记》（用户截图来源）逐字正确。`weread_crypto.cpp` 的 swap 算法自 v0.1.0 起就是对的（旧错实现的注释在 v0.1.0 已写入）。**用户侧要么是 pre-v0.1.0 的旧固件，要么 SD 章节缓存是旧固件时写下的坏副本**（`/weread/cache/<bookId>/` 删掉重拉即可）。

### 书架翻页慢（3-5s、点按不响应）—— 五层根因，逐层修掉

1. **show_shelf 同步下载封面**：翻页路径每张封面都现下（每张 TLS ~1-2s）→ 改 `img_render::cached()` 只读缓存，缺失画占位，后台预取补齐后防抖重绘（每批最多一次）。
2. **每次翻页都 1.7s 全刷**：→ 翻页改 epd_fast（~0.7s），每 8 次快刷插一次全刷清残影。
3. **netjob 封面预取长占资源**：整批 21 张封面持 net_lock ~40s（点书必等）+ TLS 连发饿死 CPU 触发 task_wdt → 改逐张加锁 + 每张 delay(400) 让出 CPU。
4. **每张封面渲染都全尺寸解码 ~0.7s**（TJpgDec 过全 MCU）：→ 下载时一次性生成 ≤128×192 RGB565 缩略图 `.thb` 落 SD，书架小图走块搬 ~95ms/张；老缓存在 draw 时懒生成。`g_dc` 不可重入，主任务绘制与 netjob 生成之间加了绘制互斥锁。
5. **FAT 大目录 open 线性扫 ~200ms/次**（`/weread/img` 几百个文件，连子目录路径也要先扫它）：→ 新缓存落 `/weread/img2/<md5前两位>/` 分片，老文件 `cached()` 命中时 rename 懒迁移；再加 RAM 负缓存（miss 过一次就不再扫，否则一次 miss 要扫 6 个路径 ~2s）。

**修后实测（500 本账号）**：翻页总耗时 ~2-3.8s（渲染 1.2-3s + 快刷 0.7s），点按不再丢失；之前是 4-18s 且频繁触发 task_wdt。

### 唤醒后 WiFi 连不上就废机

浅睡唤醒时 `WR.connectWiFi()` 返回值被忽略，重连失败一次就永久断网。改为重试到连上（3 次失败显示"点按重试"）。

### 排障工具沉淀

- `dbgch <bookId> <chapterUid>`：拉指定书章走生产解码并打印文本（错字/内容问题定位）
- `netdiag [N]` 的 `internal=` 是内存类问题第一指标
- 计时插桩法：怀疑哪里慢就在哪里打 `[t]` 毫秒点，一轮就能分层（本轮封面 open/decode/blit 三层全靠它拆开）

**版本**：m5burner.json → 0.2.5；发布物只有整片 `papers3-weread-full.bin`（keep 参数合并，头部 DIO 已验）。

### 同日追加：目录获取失败（chapterInfos）三重根因

用户报"所有书目录获取失败"。curl 对照实验 + netdiag 定位出三个独立问题：

1. **WiFi 中途掉线无人管**：netdiag 显示 `wifi=down`，断网后只有重启/浅睡唤醒才重连，期间一切请求 `HTTP -1`（传输失败）。→ loop() 加 WiFi 看门狗：断网 10s 后异步 `WiFi.begin` 重连，30s 一次不阻塞主循环，恢复后重新校时。
2. **续期防抖初始值 bug**：`last_renew_fail_` 初值 0，`millis() - 0 > 600000` 在开机前 10 分钟恒 false——**开机 10 分钟内 -2012 永远不会自动续期**。改为 `!last_renew_fail_ || ...`。
3. **续期失败不分类型**：断网导致的 renewal 失败和 wr_rt 真死同等对待，一律 10 分钟冻结续期 → 用户看到的"一直失败"。现在：传输失败 30s 后可再试；服务器拒绝才置 `rt_dead_`（10 分钟防抖）。**wr_rt 死亡时详情页直接引导扫码登录**（`登录已过期，点按屏幕扫码登录`），登录成功自动重试进详情。

另：getChapterInfos 显式识别 -2012（之前报"无 data[]"误导）；curl 对照法再次证明好用——服务器响应在 Mac 上能复现就先别怀疑设备。

**追加（v0.2.6）**：大书 chapterInfos（数百 KB）在弱网下载尾部断流 → `解析失败`。同书架方案：响应不以 `}` 收尾就重试一次；错误信息带 derr 细节。教训：修一类问题（截断）要全接口排查，别只修被报告的那一个（书架修了 chapterInfos 漏了）。另实测澄清：ArduinoJson 7 的 BasicJsonDocument 池可 realloc 增长，"64KB 容量"只是初始块，之前担心的 NoMemory 不存在。

---

## 十八、页间断行根因 + 长书 reader 截断 + 一批边角修复（2026-07-20，v0.2.7）

### 页脚吞行/第二页接不上（用户报"严重 bug"）

**根因**：`paginate_current` 对空行（trim 后为空的行）完全跳过不占高度，而 `render_page` 对空行也加段距（`if (para && y > 80) y += pg_gap` 无条件执行）——每条空段首行让渲染多积 11px，每页因此比分页器早 ~1 行满，分页器算进本页的最后 1 行渲染没画，而下页游标又从那行之后开始 → **整行凭空消失**（每页末一行）。复现工具：`dbgpages`（渲染走行 vs 分页游标对比，直接打印被跳过的文本）、`dbgtrace`（双走行逐行对拍，定位到首个空行分歧点）。修法：render_page 文本分支改为与分页器同序（先算行、trim、非空才计高加段距）。**教训：两套"同款"走行逻辑必须逐行对拍验证，不能靠目测"看起来一样"。**

### 长书打不开（"未提取到 psvts"）

**根因**：长书的 reader HTML 巨大（1281 章实测 981KB，psvts 在 863KB 处），ShardRespBuf 固定 768KB 静默截断 → 页尾 psvts 被切。修法：ShardRespBuf 改动态扩容（256KB→2MB，同 WereadClient 策略），超上限置 overflow 报错不静默。**教训加深：全项目搜"容量上限+静默丢弃"，一处都不能留（WereadClient / ShardSession / 各处 buffer 已齐）。另注意 `is_txt = ... || n0 == 0`：e_0 传输失败会误判成 TXT 书走 t 分片——本次没改，留意。**

### 其他修复

- **chapterInfos NoMemory**：ArduinoJson 池初始 64KB 靠 realloc 扩，PSRAM 碎片下扩失败 → NoMemory。改初始池按响应长一半给（64KB~1MB），避免中途扩。
- **封面章零块**：`<h1><img/></h1>` 结构里图片被标题分支当文本吞 → 整章 0 块报"无内容块"。切块器先出标题内图块。
- **整本已下载**：下载完成写 `done.flag`，详情页按钮显示"已下载"且防重复下载；目录变多自动失效。
- **skey 短命**：wr_skey Max-Age 只有 5400s（90 分钟），靠续期链路兜底，不能假设它长寿。

### 排障工具沉淀

`dbgch <bookId> <chapterUid>`（章节解码打印）、`dbgpages`（页边界断行检查）、`time`（时钟/签名）、`netdiag/netapi`（网络分层）、`[reader]/[txt]/[shelf]` 失败现场打印（hlen/head/MD5）。**方法论：服务端响应先用 curl/Python 参考实现拿"标准答案"，再决定怀疑设备哪一层。**

---

## 十九、WereadClient keep-alive 复用 + UI 假死修复（2026-07-21，v0.2.8）

### WereadClient 普通 API 连接复用

**背景**：书架/目录/详情/评论/续期/进度上传每次请求都新建 esp_http_client（DNS+TCP+TLS 握手 1-2s/次），故障暴露窗口大，也是间歇性 TCP 失败的放大器之一。

**方案**（照搬 ShardSession 已验证的模式）：
- host 固定 `weread.qq.com` 的请求走 keep-alive 会话：open 一次后 `set_url` 换路径复用同一 TLS 连接
- `i.weread.qq.com` 等异 host 自动回退一次性 client（fallback 路径）
- 失败 close 重建重试一次（与 ShardSession 一致）
- `sess_in_use_` RAII 计数 + `sess_last_use_` 闲置 30s 由 `WR.housekeeping()` 关闭（防几条 keep-alive 长连接挤爆 DRAM）
- 响应缓冲复用现有 `RespBuf`（PSRAM 512KB→4MB 扩容 + overflow 报错），会话级固定缓冲 `g_sess_rb`（esp_http_client 的 user_data 在 init 时绑定无法按请求改，故用静态缓冲每次请求重置，成功后转所有权——与 ShardSession 同规）

**实测**：netapi 10/10 全过，每轮 ~1.6s（157KB 书架 JSON 纯传输时间），无握手抖动；internal ~82KB 稳定。

**坑**：ESP-IDF 4.4.7 的 esp_http_client **没有 `esp_http_client_set_user_data`**（init 后 user_data 不可改）。不能用"每次请求换 user_data 指向当次 RespBuf"的方案，只能会话级固定缓冲。

### UI 假死修复

**背景**：单章最多串 4 个请求（reader + e_0/e_1/e_3），每个 20s 超时 × 内层 `request_retry` × 外层 `download_all` 整章重试，最坏堵数分钟；取消只在章节边界检查，无法中断进行中的 `esp_http_client_perform()`。

**修复**：
1. **整章 90s 总预算**：`fetch_chapter_raw` 开头设 `g_chapter_deadline = millis() + 90000`，每个请求前 `chapter_aborted()` 检查，超预算返回"整章超时"
2. **去掉外层整章重试**：`download_all` 里 `delay(2000)` + 整章重试删掉（内层 `request_retry` 已够；外层重试是数分钟卡死的放大器）
3. **取消在网络等待中生效**：全局 `volatile bool g_net_cancel`（weread_api 命名空间，main.cpp 点取消置位）
   - `ShardSession::request_retry` 每次请求前检查，置位则不再发起新请求（status=-2）
   - `fetch_chapter_raw` 每章开始清零（上次取消不残留）
   - 进行中的 perform 无法打断，但后续请求不再发起——**最坏堵 1 个 HTTP 超时 20s，不再数分钟**
   - `download_all` 点取消后置位 + 放锁等网络层退出（最多 25s）+ 恢复锁

**边界**：ESP32 的 esp_http_client_perform 是阻塞调用，无法异步取消；本方案是"停止后续请求"而非"打断当前请求"，最坏情况仍需等当前请求超时（20s）。

### 排障工具沉淀（复用）

`netapi 10` 验证 keep-alive 效果：连续 10 轮书架请求全部 ~1.6s（无握手抖动即复用成功；若每次新建会有 1-2s TLS 抖动）。
