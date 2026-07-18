// edcbook_font.h — EDCBook .bin 字体解析与渲染（PaperS3 / LovyanGFX）
// 格式（由 EDCBook_FontTool_1.1.py 逆向得出）：
//   [头] uint32 char_count + uint8 font_height
//   [条目×N] u16 unicode, u16 advance, u8 w, u8 h, i8 xo, i8 yo,
//            u32 bmp_off, u32 bmp_size, u32 cached
//   [位图] 霍夫曼位流(MSB first): "0"=白(15) "10"=黑(0) "11"+4bit=灰(1..14)
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <vector>
#include <map>

class EdcFont {
public:
    bool load(const char* path);          // 从 SD 加载 .bin（索引进 PSRAM）
    bool loaded() const { return loaded_; }
    int  fontHeight() const { return font_height_; }

    // 查找字符条目；返回索引或 -1
    int findGlyph(uint16_t cp) const;

    // 解码一个字符的灰度位图到 out（调用方保证 w*h 大小）。返回是否成功。
    // out 每像素 0..15（0=黑，15=白），行优先。
    bool decodeGlyph(int idx, std::vector<uint8_t>& out, int& w, int& h, int& xo, int& yo, int& adv);

    // 测量一行 UTF-8 文本的像素宽度
    int textWidth(const String& utf8);

    // UTF-8 解码下一个码点（渲染时用）
    static uint32_t nextCodepoint(const uint8_t*& p);

private:
    struct Entry {
        uint16_t cp;
        uint16_t adv;
        uint8_t  w, h;
        int8_t   xo, yo;
        uint32_t off, size;
    };
    // 字形解码缓存（PSRAM）：每页 ~600 次 SD 读是翻页慢的主因，缓存后命中即返回
    struct GCEnt {
        int w, h, xo, yo, adv;
        uint8_t* pix; // PSRAM，w*h 字节
    };
    std::map<uint16_t, GCEnt> cache_;
    size_t cache_bytes_ = 0;
    static const size_t CACHE_CAP = 1536 * 1024; // 超出即整体清空（书是顺序读的，偶发清空无伤）
    bool loaded_ = false;
    int  font_height_ = 0;
    uint32_t char_count_ = 0;
    std::vector<Entry> entries_;   // 索引放 PSRAM
    File file_;                     // 保持文件句柄读位图
    String path_;

    // 读文件指定偏移到 buf
    bool readAt(uint32_t off, uint8_t* buf, size_t n);
};

extern EdcFont EDCFONT;
