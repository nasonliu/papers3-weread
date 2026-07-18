// config.h — 编译期配置与全局常量
#pragma once
#include <Arduino.h>

// ---------------- 屏幕（PaperS3 竖屏逻辑分辨率） ----------------
// ED047TC1 物理 960x540（横），阅读用竖屏 540x960
#define SCREEN_W 540
#define SCREEN_H 960

// ---------------- 版心边距 ----------------
#define MARGIN_LEFT   36
#define MARGIN_RIGHT  36
#define MARGIN_TOP    56
#define MARGIN_BOTTOM 56
#define TEXT_AREA_W   (SCREEN_W - MARGIN_LEFT - MARGIN_RIGHT)
#define TEXT_AREA_H   (SCREEN_H - MARGIN_TOP - MARGIN_BOTTOM)

// ---------------- 正文字号（VLW 字体需配套） ----------------
#define BODY_FONT_SIZE 26
#define LINE_HEIGHT    44   // 行距 = 字号 * 1.7 左右

// ---------------- 网络 ----------------
#define WEREAD_HOST_I   "https://i.weread.qq.com"
#define WEREAD_HOST_WEB "https://weread.qq.com"
extern const char* WEREAD_USER_AGENT; // 在 config.cpp 定义

#define HTTP_TIMEOUT_MS 20000

// ---------------- 路径 ----------------
#define PATH_CONFIG   "/spiffs/config.json"   // WiFi + cookie 持久化
#define PATH_BOOKSHELF "/shelf.json"
#define CACHE_DIR     "/sdcard/weread"

// ---------------- 颜色（16 灰阶） ----------------
#define COL_BLACK   0
#define COL_GRAY    7
#define COL_WHITE   15
