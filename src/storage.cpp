// storage.cpp — 实现
#include "storage.h"
#include <SD.h>
#include <SPIFFS.h>

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
