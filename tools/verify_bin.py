#!/usr/bin/env python3
# 验证 ttf2edcbook 生成的 .bin 文件格式正确（用现有解码器逻辑解析）
import struct, sys

def decode_huffman(data, w, h):
    """EDCBook 霍夫曼解码（0=白15, 10=黑0, 11+4bit=灰1-14）"""
    pixels = []
    bytepos, bitpos = 0, 7
    total = w * h
    for _ in range(total):
        # 读 1 bit
        if bytepos >= len(data): return pixels
        bit = (data[bytepos] >> bitpos) & 1
        bitpos -= 1
        if bitpos < 0: bitpos, bytepos = 7, bytepos + 1
        if bit == 0:
            pixels.append(15)  # 白
        else:
            # 读第 2 bit
            if bytepos >= len(data): return pixels
            bit2 = (data[bytepos] >> bitpos) & 1
            bitpos -= 1
            if bitpos < 0: bitpos, bytepos = 7, bytepos + 1
            if bit2 == 0:
                pixels.append(0)  # 黑
            else:
                # 读 4 bit 灰度
                g = 0
                for _ in range(4):
                    if bytepos >= len(data): return pixels
                    g = (g << 1) | ((data[bytepos] >> bitpos) & 1)
                    bitpos -= 1
                    if bitpos < 0: bitpos, bytepos = 7, bytepos + 1
                pixels.append(g)
    return pixels

def verify_bin(path):
    with open(path, 'rb') as f:
        hdr = f.read(5)
        char_count, font_height = struct.unpack('<IB', hdr)
        print(f'{path}: {char_count} 字符, 字高 {font_height}')
        # 读前 3 个条目
        for i in range(min(3, char_count)):
            entry = f.read(20)
            cp, adv, w, h, xo, yo, off, size, cached = struct.unpack('<HHBBbbIII', entry)
            print(f'  [{i}] U+{cp:04X} adv={adv} {w}x{h} xo={xo} yo={yo} off={off} size={size}')
            if size > 0:
                # 读位图并解码验证
                cur = f.tell()
                f.seek(5 + char_count * 20 + off)
                bmp = f.read(size)
                f.seek(cur)
                pixels = decode_huffman(bmp, w, h)
                if len(pixels) == w * h:
                    print(f'      位图解码 OK ({len(pixels)} 像素)')
                else:
                    print(f'      位图解码失败 ({len(pixels)}/{w*h})')

if __name__ == '__main__':
    verify_bin(sys.argv[1])
