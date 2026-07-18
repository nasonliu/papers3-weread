// weread_crypto.h — 微信读书 Web 协议加密/解码三件套
// 移植自 finlater/weread.koplugin 的 lib/weread.lua 与 scripts/fetch_weread_epub.py
#pragma once
#include <Arduino.h>
#include <vector>
#include <utility>

namespace weread {

// MD5 hex（小写 32 字符）
String md5_hex(const uint8_t* data, size_t len);
inline String md5_hex(const String& s) { return md5_hex((const uint8_t*)s.c_str(), s.length()); }

// URL 编码（对应 JS encodeURIComponent：保留 A-Z a-z 0-9 - _ . ~）
String url_encode(const String& s);

// _e(value) 哈希：对 bookId/chapterUid/时间戳的确定性编码
//   数字串 → type '3'，按 9 位分段转 hex
//   非数字 → type '4'，逐字节 hex
String e(const String& value);
inline String e(long v) { return e(String(v)); }

// 签名 s：对按 key 排序的 query 串做自定义 hash（初始 0x15051505）
String sign(const String& query);

// 把参数表按 key 排序拼成 query 串（不含 s），并返回；调用方随后 sign 之
String sorted_query(const std::vector<std::pair<String,String>>& params, String& out_query_for_sign);

// 构造正文分片请求参数（b/c/r/ct/ps/pc/sc/prevChapter/st/s）
// psvts 来自 reader HTML；style=true 用于 e_2（CSS）
// 返回可直接 JSON 序列化的 key=value 列表（已含 s）
std::vector<std::pair<String,String>> make_content_params(
    const String& book_id, const String& chapter_uid, const String& psvts,
    bool style = false, int sc = 1);

// ---- 响应解码 ----
// 校验并去掉 32 位 MD5 前缀；失败返回空串
String checked_body(const String& shard);

// 正文解码：拼接 e0+e1+e3（或 t0+t1）的 body，去首字符、字符交换还原、base64url 解码
// 返回 UTF-8 明文字符串
// 注意：结果超 ~64KB 时 Arduino String 会损坏，大章节请用下面的 PSRAM 版
String decode_content_shards(const String& s0, const String& s1, const String& s2);

// PSRAM 版：算法同上，但中间过程与结果全部在 PSRAM 缓冲进行，不受 String 64KB 限制
// 输入为三个分片的原始数据（指针+长度，可在 PSRAM；nullptr/长度≤32 的片跳过），
// 不允许用 Arduino String 持有分片——单片超 ~64KB 时 String "长度虚高内容空洞"，
// MD5 校验会因此失败丢片，残缺 payload 解码出局部错乱内容（真机事故）
// 成功返回 PSRAM 明文缓冲（heap_caps_malloc，调用方负责 heap_caps_free），
// *out_len 为明文长度（不含结尾 \0，但缓冲已补 \0 便于字符串扫描）；失败返回 nullptr
char* decode_content_shards_psram(const char* s0, size_t n0,
                                  const char* s1, size_t n1,
                                  const char* s2, size_t n2, size_t* out_len);

} // namespace weread
