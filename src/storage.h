// storage.h — 存储路径抽象：配置优先写 SD 卡，SD 不在时回退 SPIFFS
// SD 卡目录：/weread（config.json、cache/ 章节缓存）
#pragma once
#include <FS.h>

#define SD_WEREAD_DIR   "/weread"
#define SD_CACHE_DIR    "/weread/cache"
#define SD_CONFIG_PATH  "/weread/config.json"
#define SPIFFS_CONFIG_PATH "/config.json"

// 由 main.cpp 在 SD.begin 后调用，告知 SD 是否可用
void storage_note_sd(bool mounted);
bool storage_sd_ok();

// 打开配置文件：读时 SD 优先、缺失回退 SPIFFS（兼容旧配置）；写时 SD 在就只写 SD
File open_config_read();
File open_config_write();

// 追加一行诊断日志到 SD 卡 /weread/debug.log（SD 不在时静默跳过）。
// 超 64KB 自动清零重写。用于抓用户现场：登录/书架等失败时把关键信息落卡，
// 用户把 TF 卡里的这个文件发回来即可定位，不用接串口。
void storage_debug_log(const String& line);
