# 准备流程（新手上路）

从零到能读，共 4 步，约 10 分钟。

## 1. 硬件

- M5Stack PaperS3（Type-C 供电）
- TF 卡一张（≥1GB，FAT32 格式）

## 2. TF 卡准备（只需做一次）

在 TF 卡根目录建 `font` 文件夹，放入字体文件：

```
TF 卡根目录/
└── font/
    ├── 霞鹜文楷_大.bin        ← 必需（UI 字体，缺它中文不显示）
    └── （可选）其它 .bin 字体   ← 正文字体，阅读页里可切换
```

**字体从哪来**（任选）：
- **GitHub 直接下载**（推荐）：见本仓库 Release 的 `fonts/` 资源
- 从 EDC Book 阅读器的 TF 卡直接拷（同格式通用）

字体是 EDCBook 点阵格式（.bin），**UI 固定用 `霞鹜文楷_大.bin`**，必须有；
其余字体可选（霞鹜文楷30/36、霞鹜新晰黑、霞鹜新致宋、思源宋体、仓耳今楷、汉仪空山楷等，
文件名带字号数字，阅读页内可实时切换）。

## 3. 烧录固件

方式 A（推荐，M5Burner）：
1. 装 M5Burner（M5Stack 官网下载）
2. 本仓库 Release 下载 `papers3-weread-full.bin`
3. M5Burner 选「自定义固件/Local」选这个 bin，选设备 COM 口，Burn

方式 B（命令行）：
```bash
esptool.py --chip esp32s3 write_flash 0x0 papers3-weread-full.bin
```
（也可以只烧应用：`write_flash 0x10000 papers3-weread-app.bin`）

## 4. 首次开机

1. 插入 TF 卡，开机 → 显示微信读书图标
2. 无配置时自动开热点 **`PaperS3-阅读器`**（手机连上自动弹配网页，或浏览器开 `192.168.4.1`）
3. 页面里刷新扫描 → 选你家 WiFi → 输密码 → 连接，成功自动重启
4. 重启后自动连 WiFi → 屏幕弹微信读书二维码 → **微信扫码** → 进入书架

以后开机即用。cookie 失效自动续期，续不上才会再弹码。

## 目录结构速查

| 位置 | 内容 |
|------|------|
| `font/*.bin` | 字体（必需霞鹜文楷_大） |
| `weread/config.json` | WiFi + cookie（配网/扫码后自动生成） |
| `weread/cache/` | 各书的目录/详情/正文/进度缓存 |
| `weread/img/` | 封面插图缓存 |

更多操作说明见 [README.md](README.md)。
