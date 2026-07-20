// storage.cpp — 实现
#include "storage.h"
#include <SD.h>
#include <SPIFFS.h>
#include <Arduino.h>

#define SD_DEBUG_LOG_PATH "/weread/debug.log"
#define SD_DEBUG_LOG_MAX  (64 * 1024) // 超了清零重写，防无限膨胀

static bool g_sd = false;

void storage_note_sd(bool mounted) { g_sd = mounted; }
bool storage_sd_ok() { return g_sd; }

File open_config_read() {
    if (g_sd) {
        File f = SD.open(SD_CONFIG_PATH, "r");
        if (f) return f; // SD 上有就用 SD 的
    }
    return SPIFFS.open(SPIFFS_CONFIG_PATH, "r");
}

File open_config_write() {
    if (g_sd) {
        SD.mkdir(SD_WEREAD_DIR);
        File f = SD.open(SD_CONFIG_PATH, "w");
        if (f) return f;
    }
    return SPIFFS.open(SPIFFS_CONFIG_PATH, "w");
}

void storage_debug_log(const String& line) {
    if (!g_sd) return;
    SD.mkdir(SD_WEREAD_DIR);
    size_t sz = 0;
    File r = SD.open(SD_DEBUG_LOG_PATH, "r");
    if (r) { sz = r.size(); r.close(); }
    File w = SD.open(SD_DEBUG_LOG_PATH, sz > SD_DEBUG_LOG_MAX ? "w" : FILE_APPEND);
    if (!w) return;
    w.printf("[%lu] %s\n", (unsigned long)millis(), line.c_str()); // ms 时间戳（开机起）
    w.close();
}
