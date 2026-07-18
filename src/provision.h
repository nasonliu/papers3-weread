#pragma once
#include <Arduino.h>
// 启动配网门户（阻塞，成功后自动重启设备，永不返回）：
// 开 AP "PaperS3-阅读器"（开放无密码）+ captive DNS + Web 配网页 http://192.168.4.1
// ui: 墨水屏 3 行文本状态回调（main.cpp 提供）；boot_error: 进入门户的原因（可空，先显示一屏）
void run_provisioning_portal(void (*ui)(const String&, const String&, const String&), const char* boot_error);
