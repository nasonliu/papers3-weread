#!/usr/bin/env python3
"""
ttf2edcbook.py — 把 TTF/OTF 矢量字体预生成 EDCBook .bin 点阵字体（多字号）
用法: python3 ttf2edcbook.py <ttf文件> <输出目录> <字号1> [字号2] [字号3] ...
例: python3 ttf2edcbook.py LXGWWenKai-Regular.ttf output 16 24 32 48
"""
import sys, os, struct
import freetype

def huffman_encode(pixels, w, h):
    """像素（0-15 灰度）→ 霍夫曼位流（EDCBook 格式：0=白，10=黑，11+4bit=灰）"""
    bits = []
    for y in range(h):
        for x in range(w):
            g = pixels[y * w + x]
            if g == 15:
                bits.append(0)  # 白
            elif g == 0:
                bits.extend([1, 0])  # 黑
            else:
                bits.extend([1, 1])  # 灰
                for i in range(3, -1, -1):  # 4bit MSB 先
                    bits.append((g >> i) & 1)
    # 打包成字节（MSB 先读）
    out = bytearray()
    for i in range(0, len(bits), 8):
        byte = 0
        for j in range(8):
            if i + j < len(bits):
                byte |= bits[i + j] << (7 - j)
        out.append(byte)
    return bytes(out)

def generate_bin(ttf_path, out_path, font_height, charset):
    """生成 EDCBook .bin（指定字号 + 字符集）"""
    face = freetype.Face(ttf_path)
    face.set_pixel_sizes(0, font_height)
    entries = []
    bitmaps = bytearray()
    offset = 0

    for cp in charset:
        face.load_char(cp, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
        glyph = face.glyph
        bmp = glyph.bitmap
        w, h = bmp.width, bmp.rows
        # 灰度转换：freetype 输出 0-255，转成 0-15（0=黑，15=白）
        pixels = []
        if w > 0 and h > 0:
            for y in range(h):
                for x in range(w):
                    v = bmp.buffer[y * bmp.pitch + x]  # 0-255（0=黑，255=白）
                    g = 15 - (v * 15 // 255)  # 反转并缩放到 0-15
                    pixels.append(g)
            bmp_data = huffman_encode(pixels, w, h)
        else:
            bmp_data = b''
        # advance = 步进宽度（26.6 定点 → 整数像素）
        advance = glyph.advance.x >> 6
        # x_offset/y_offset（对齐 EDCBook 格式）
        xo = glyph.bitmap_left
        yo = font_height - glyph.bitmap_top
        entries.append((cp, advance, w, h, xo, yo, offset, len(bmp_data)))
        bitmaps.extend(bmp_data)
        offset += len(bmp_data)

    # 写文件
    with open(out_path, 'wb') as f:
        f.write(struct.pack('<IB', len(entries), font_height))
        for cp, adv, w, h, xo, yo, off, size in entries:
            f.write(struct.pack('<HHBBbbIII', cp, adv, w, h, xo, yo, off, size, 0))
        f.write(bitmaps)
    print(f'{out_path}: {len(entries)} 字符, {os.path.getsize(out_path)} bytes')

def load_charset():
    """常用中文字符集（现代汉语常用字 3500 + ASCII + 常用标点，覆盖 99% 阅读场景）"""
    charset = set(range(0x20, 0x7F))  # ASCII
    # 现代汉语常用字表 3500 字（0x4E00-0x9FA5 中的高频部分，按字频排序取前 3500）
    # 简化：取 0x4E00-0x9FA5 范围中的常用子集（实际 3500 字散布在整个区间）
    # 这里用 GB2312 一级汉字（3755 字）近似，覆盖 99% 以上文本
    for cp in range(0x4E00, 0x9FA6):
        # GB2312 一级汉字大致范围（按拼音排序，高频字集中在 0x4E00-0x7FFF）
        # 简化：全取 0x4E00-0x9FA5，生成时再按字频过滤（这里先全取，生成脚本可优化）
        charset.add(cp)
    # 常用标点
    for cp in [0x3000, 0x3001, 0x3002, 0xFF0C, 0xFF1A, 0xFF1B, 0xFF01, 0xFF1F,
               0x201C, 0x201D, 0x2018, 0x2019, 0x2026, 0x2014, 0xFF08, 0xFF09]:
        charset.add(cp)
    return sorted(charset)

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)
    ttf, outdir = sys.argv[1], sys.argv[2]
    sizes = [int(x) for x in sys.argv[3:]]
    os.makedirs(outdir, exist_ok=True)
    charset = load_charset()
    print(f'字符集: {len(charset)} 字符')
    for size in sizes:
        base = os.path.splitext(os.path.basename(ttf))[0]
        out = os.path.join(outdir, f'{base}_{size}.bin')
        generate_bin(ttf, out, size, charset)
