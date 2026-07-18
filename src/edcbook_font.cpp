// edcbook_font.cpp — EDCBook .bin 字体解析实现
#include "edcbook_font.h"
#include <esp_heap_caps.h>

EdcFont EDCFONT;

// 条目在文件中的固定大小（struct.pack('<HHBBbbIII') = 2+2+1+1+1+1+4+4+4 = 20）
static const int ENTRY_SIZE = 20;
static const int HEADER_SIZE = 5; // u32 char_count + u8 font_height

uint32_t EdcFont::nextCodepoint(const uint8_t*& p) {
    uint32_t c = *p++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; c = (c << 6) | (*p++ & 0x3F); return c; }
    if ((c & 0xF0) == 0xE0) { c &= 0x0F; c = (c << 6) | (*p++ & 0x3F); c = (c << 6) | (*p++ & 0x3F); return c; }
    if ((c & 0xF8) == 0xF0) { c &= 0x07; c = (c << 6) | (*p++ & 0x3F); c = (c << 6) | (*p++ & 0x3F); c = (c << 6) | (*p++ & 0x3F); return c; }
    return c;
}

bool EdcFont::readAt(uint32_t off, uint8_t* buf, size_t n) {
    if (!file_) return false;
    if (!file_.seek(off)) return false;
    return file_.read(buf, n) == n;
}

bool EdcFont::load(const char* path) {
    // 重载必须清空旧条目和旧缓存：否则新条目追加在旧条目后，二分查找会命中旧字体（"换字体没效果"的根因）
    entries_.clear();
    for (auto& kv : cache_) heap_caps_free(kv.second.pix);
    cache_.clear();
    cache_bytes_ = 0;
    loaded_ = false;
    path_ = path;
    file_ = SD.open(path, "r");
    if (!file_) { Serial.printf("[font] 打开失败 %s\n", path); return false; }

    uint8_t hdr[HEADER_SIZE];
    if (file_.read(hdr, HEADER_SIZE) != HEADER_SIZE) { Serial.println("[font] 头读取失败"); return false; }
    char_count_ = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    font_height_ = hdr[4];
    Serial.printf("[font] %s 字符数=%d 字高=%d\n", path, (int)char_count_, font_height_);

    // 条目索引读进 PSRAM
    size_t table_bytes = (size_t)char_count_ * ENTRY_SIZE;
    uint8_t* raw = (uint8_t*)heap_caps_malloc(table_bytes, MALLOC_CAP_SPIRAM);
    if (!raw) { Serial.println("[font] PSRAM 分配失败"); return false; }
    if (file_.read(raw, table_bytes) != table_bytes) {
        Serial.println("[font] 条目表读取失败");
        heap_caps_free(raw);
        return false;
    }
    entries_.reserve(char_count_);
    for (uint32_t i = 0; i < char_count_; i++) {
        const uint8_t* e = raw + i * ENTRY_SIZE;
        Entry en;
        en.cp   = e[0] | (e[1] << 8);
        en.adv  = e[2] | (e[3] << 8);
        en.w    = e[4];
        en.h    = e[5];
        en.xo   = (int8_t)e[6];
        en.yo   = (int8_t)e[7];
        en.off  = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        en.size = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
        entries_.push_back(en);
    }
    heap_caps_free(raw);
    loaded_ = true;
    Serial.printf("[font] 索引加载完成 %d 条\n", (int)entries_.size());
    return true;
}

int EdcFont::findGlyph(uint16_t cp) const {
    // 条目按 unicode 升序（工具按字符集顺序生成），二分查找
    int lo = 0, hi = (int)entries_.size() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (entries_[mid].cp == cp) return mid;
        if (entries_[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

bool EdcFont::decodeGlyph(int idx, std::vector<uint8_t>& out, int& w, int& h, int& xo, int& yo, int& adv) {
    if (idx < 0 || idx >= (int)entries_.size()) return false;
    const Entry& e = entries_[idx];

    // 命中字形缓存：免 SD 读 + 免霍夫曼解码（翻页提速主力）
    auto it = cache_.find(e.cp);
    if (it != cache_.end()) {
        const GCEnt& g = it->second;
        w = g.w; h = g.h; xo = g.xo; yo = g.yo; adv = g.adv;
        out.assign(g.pix, g.pix + (size_t)w * h);
        return true;
    }

    w = e.w; h = e.h; xo = e.xo; yo = e.yo; adv = e.adv;
    out.assign((size_t)w * h, 15); // 默认白
    if (e.size == 0 || w == 0 || h == 0) return true; // 空白字符（无 SD 读，无需缓存）

    uint8_t* bmp = (uint8_t*)heap_caps_malloc(e.size, MALLOC_CAP_SPIRAM);
    if (!bmp) return false;
    bool okr = readAt(e.off, bmp, e.size);
    if (!okr) { heap_caps_free(bmp); return false; }

    // 霍夫曼位流解码（MSB first）：
    //   "0"       → 白(15)
    //   "10"      → 黑(0)
    //   "11"+4bit → 灰度(1..14)
    int total = w * h;
    int px = 0;
    size_t bytepos = 0;
    int bitpos = 7;
    auto readBit = [&]() -> int {
        if (bytepos >= e.size) return -1;
        int b = (bmp[bytepos] >> bitpos) & 1;
        if (--bitpos < 0) { bitpos = 7; bytepos++; }
        return b;
    };
    while (px < total) {
        int b0 = readBit();
        if (b0 < 0) break;
        if (b0 == 0) { out[px++] = 15; continue; }
        int b1 = readBit();
        if (b1 < 0) break;
        if (b1 == 0) { out[px++] = 0; continue; }
        // "11" + 4bit 灰度
        int g = 0;
        for (int k = 0; k < 4; k++) { int bb = readBit(); if (bb < 0) break; g = (g << 1) | bb; }
        out[px++] = (uint8_t)g;
    }
    heap_caps_free(bmp);

    // 写入缓存（预算超出整体清空）
    if (cache_bytes_ + (size_t)w * h > CACHE_CAP) {
        for (auto& kv : cache_) heap_caps_free(kv.second.pix);
        cache_.clear();
        cache_bytes_ = 0;
    }
    uint8_t* p = (uint8_t*)heap_caps_malloc((size_t)w * h, MALLOC_CAP_SPIRAM);
    if (p) {
        memcpy(p, out.data(), (size_t)w * h);
        cache_[e.cp] = GCEnt{w, h, xo, yo, adv, p};
        cache_bytes_ += (size_t)w * h;
    }
    return true;
}

int EdcFont::textWidth(const String& utf8) {
    const uint8_t* p = (const uint8_t*)utf8.c_str();
    int width = 0;
    while (*p) {
        uint32_t cp = nextCodepoint(p);
        if (cp == 0) break;
        int idx = findGlyph((uint16_t)cp);
        width += (idx >= 0) ? entries_[idx].adv : font_height_ / 2;
    }
    return width;
}
