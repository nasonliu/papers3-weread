// weread_crypto.cpp — 实现
#include "weread_crypto.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h> // PSRAM 版解码用 heap_caps_malloc/free

namespace weread {

String md5_hex(const uint8_t* data, size_t len) {
    uint8_t out[16];
    mbedtls_md5(data, len, out);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[32] = 0;
    return String(hex);
}

String url_encode(const String& s) {
    String out;
    const char* p = s.c_str();
    char buf[4];
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
        p++;
    }
    return out;
}

static bool is_digit_string(const String& s) {
    if (s.length() == 0) return false;
    for (size_t i = 0; i < s.length(); i++) if (!isDigit(s[i])) return false;
    return true;
}

// 逐字节 hex（无填充）
static String byte_hex(const String& s) {
    String out;
    for (size_t i = 0; i < s.length(); i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%x", (unsigned char)s[i]);
        out += buf;
    }
    return out;
}

String e(const String& value) {
    String h = md5_hex(value);
    String result = h.substring(0, 3);
    String type_flag;
    std::vector<String> chunks;

    if (is_digit_string(value)) {
        type_flag = "3";
        size_t i = 0;
        while (i < value.length()) {
            String part = value.substring(i, min(i + 9, value.length()));
            // 转 64 位整数再 %x，避免大数问题（9 位十进制 < 2^30，安全）
            unsigned long long v = strtoull(part.c_str(), nullptr, 10);
            char buf[20];
            snprintf(buf, sizeof(buf), "%llx", v);
            chunks.push_back(String(buf));
            i += 9;
        }
    } else {
        type_flag = "4";
        chunks.push_back(byte_hex(value));
    }

    result += type_flag;
    result += "2" + h.substring(h.length() - 2);

    for (size_t i = 0; i < chunks.size(); i++) {
        char lenbuf[4];
        snprintf(lenbuf, sizeof(lenbuf), "%02x", (unsigned int)chunks[i].length());
        result += lenbuf;
        result += chunks[i];
        if (i + 1 < chunks.size()) result += "g";
    }

    while (result.length() < 20) {
        result += h.substring(0, 20 - result.length());
    }

    result += md5_hex(result).substring(0, 3);
    return result;
}

String sign(const String& query) {
    int64_t a = 0x15051505;
    int64_t b = a;
    int length = (int)query.length();
    int i = length; // 1-based index in Lua; we use i-1 / i-2 for 0-based

    while (i > 1) {
        unsigned char ci   = (unsigned char)query[i - 1];
        unsigned char ci_1 = (unsigned char)query[i - 2];
        a = (a ^ ((int64_t)ci   << ((length - i + 1) % 30))) & 0x7fffffff;
        b = (b ^ ((int64_t)ci_1 << ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)(a + b));
    String s(buf);
    s.toLowerCase();
    return s;
}

// 参数按 key 升序，拼 key=urlencode(value)，用 & 连接；跳过 key=="s"
String sorted_query(const std::vector<std::pair<String,String>>& params, String& out_query_for_sign) {
    std::vector<std::pair<String,String>> sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<String,String>& x, const std::pair<String,String>& y){ return x.first < y.first; });
    String q;
    bool first = true;
    for (auto& kv : sorted) {
        if (kv.first == "s") continue;
        if (!first) q += "&";
        q += kv.first + "=" + url_encode(kv.second);
        first = false;
    }
    out_query_for_sign = q;
    return q;
}

std::vector<std::pair<String,String>> make_content_params(
    const String& book_id, const String& chapter_uid, const String& psvts,
    bool style, int sc) {
    long ct = (long)time(nullptr);
    if (e(String(ct)) == psvts) ct += 1;

    std::vector<std::pair<String,String>> p;
    p.push_back({"b", e(book_id)});
    p.push_back({"c", e(chapter_uid)});
    // r = random(0..9999)^2
    long rv = random(0, 10000);
    p.push_back({"r", String(rv * rv)});
    p.push_back({"ct", String(ct)});
    p.push_back({"ps", psvts});
    p.push_back({"pc", e(String(ct))});
    p.push_back({"sc", String(sc)});
    p.push_back({"prevChapter", "false"});
    p.push_back({"st", style ? "1" : "0"});

    String q;
    sorted_query(p, q);
    p.push_back({"s", sign(q)});
    return p;
}

String checked_body(const String& shard) {
    if (shard.length() <= 32) return "";
    String expected = shard.substring(0, 32);
    String body = shard.substring(32);
    String actual = md5_hex(body);
    actual.toUpperCase();
    if (actual != expected) return "";
    return body;
}

// --- 字符交换还原 ---
static std::vector<int> swap_positions(const String& enc) {
    std::vector<int> result;
    int length = enc.length();
    if (length < 4) return result;
    if (length < 11) { result.push_back(0); result.push_back(2); return result; }

    int n = min(4, (length + 9) / 10);
    String tmp;
    for (int i = length - 1; i >= length - n; i--) {
        // 原算法（weread.lua）：tonumber(bin(byte), 4)——二进制数字符串按四进制解读，
        // 等价 bit 展开：第 b 位为 1 累加 4^b = 2^(2b)（见指针版注释）
        uint8_t v = (uint8_t)enc[i];
        uint32_t val = 0;
        for (int b = 0; b < 8; b++)
            if ((v >> b) & 1) val += (1U << (2 * b));
        tmp += String(val);
    }

    int m = length - n - 2;
    if (m <= 0) return result;
    int step = String(m).length();
    int i = 0;
    while ((int)result.size() < 10 && i + step < (int)tmp.length()) {
        int v1 = tmp.substring(i, i + step).toInt() % m;
        int v2 = tmp.substring(i + 1, i + 1 + step).toInt() % m;
        result.push_back(v1);
        result.push_back(v2);
        i += step;
    }
    return result;
}

static String reverse_swaps(const String& enc, const std::vector<int>& pos) {
    String chars = enc;
    int plen = (int)pos.size();
    for (int i = plen - 1; i > 0; i -= 2) {
        for (int k = 1; k >= 0; k--) {
            int left = pos[i] + k;
            int right = pos[i - 1] + k;
            if (left >= 0 && right >= 0 && left < (int)chars.length() && right < (int)chars.length()) {
                char t = chars[left];
                chars[left] = chars[right];
                chars[right] = t;
            }
        }
    }
    return chars;
}

String decode_content_shards(const String& s0, const String& s1, const String& s2) {
    String payload = checked_body(s0) + checked_body(s1) + checked_body(s2);
    if (payload.length() == 0) return "";
    // 去首字符
    String enc = payload.substring(1);
    String reordered = reverse_swaps(enc, swap_positions(enc));

    // base64url → base64，滤掉非法字符
    String b64;
    for (size_t i = 0; i < reordered.length(); i++) {
        char c = reordered[i];
        if (c == '-') b64 += '+';
        else if (c == '_') b64 += '/';
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')
            b64 += c;
    }
    while (b64.length() % 4) b64 += '=';

    size_t out_len = 0;
    // 先求解码后长度
    if (mbedtls_base64_decode(nullptr, 0, &out_len, (const uint8_t*)b64.c_str(), b64.length()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && out_len == 0)
        return "";
    uint8_t* buf = (uint8_t*)ps_malloc(out_len + 1);
    if (!buf) buf = (uint8_t*)malloc(out_len + 1);
    if (!buf) return "";
    size_t written = 0;
    if (mbedtls_base64_decode(buf, out_len, &written, (const uint8_t*)b64.c_str(), b64.length()) != 0) {
        free(buf);
        return "";
    }
    buf[written] = 0;
    String result = String((char*)buf, written); // 已是 UTF-8
    free(buf);
    return result;
}

// --- PSRAM 版解码（大章节用：Arduino String 超 ~64KB 会损坏） ---

// swap_positions 的指针版（逻辑与 String 版一致，避免复制大缓冲进 DRAM）
static std::vector<int> swap_positions_ptr(const char* enc, int length) {
    std::vector<int> result;
    if (length < 4) return result;
    if (length < 11) { result.push_back(0); result.push_back(2); return result; }

    int n = min(4, (length + 9) / 10);
    String tmp;
    for (int i = length - 1; i >= length - n; i--) {
        // 原算法（weread.lua）：tonumber(bin(byte), 4)——把字节的二进制数字符串按四进制解读，
        // 数学上等价于 bit 展开：第 b 位为 1 则累加 4^b = 2^(2b)。
        // （之前错实现成"每 2 位二进制转一个四进制数字再拼接"，是 URL/正文个别字节错位的根因）
        uint8_t v = (uint8_t)enc[i];
        uint32_t val = 0;
        for (int b = 0; b < 8; b++)
            if ((v >> b) & 1) val += (1U << (2 * b));
        tmp += String(val);
    }

    int m = length - n - 2;
    if (m <= 0) return result;
    int step = String(m).length();
    int i = 0;
    while ((int)result.size() < 10 && i + step < (int)tmp.length()) {
        int v1 = tmp.substring(i, i + step).toInt() % m;
        int v2 = tmp.substring(i + 1, i + 1 + step).toInt() % m;
        result.push_back(v1);
        result.push_back(v2);
        i += step;
    }
    return result;
}

char* decode_content_shards_psram(const char* s0, size_t n0,
                                  const char* s1, size_t n1,
                                  const char* s2, size_t n2, size_t* out_len) {
    // 1. PSRAM 拼接三个分片 body（各去 32 字节 MD5 前缀并校验，校验逻辑同 checked_body）
    size_t cap = n0 + n1 + n2;
    if (!cap) return nullptr;
    char* buf = (char*)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    size_t n = 0;
    const char* shards[3] = {s0, s1, s2};
    size_t lens[3] = {n0, n1, n2};
    for (int k = 0; k < 3; k++) {
        const char* shard = shards[k];
        size_t slen = lens[k];
        if (!shard || slen <= 32) continue;
        // MD5 校验：body 的 md5(大写) 须等于前 32 字节；任一不等丢弃该片（同 checked_body）
        String actual = md5_hex((const uint8_t*)shard + 32, slen - 32);
        actual.toUpperCase();
        if (actual.length() != 32 || memcmp(shard, actual.c_str(), 32) != 0) continue;
        memcpy(buf + n, shard + 32, slen - 32);
        n += slen - 32;
    }
    if (!n) { heap_caps_free(buf); return nullptr; }

    // 2. 去首字符（指针偏移，不复制）
    char* enc = buf + 1;
    int enc_len = (int)n - 1;

    // 3. 字符交换还原（原地交换，与 reverse_swaps 语义等价）
    std::vector<int> pos = swap_positions_ptr(enc, enc_len);
    int plen = (int)pos.size();
    for (int i = plen - 1; i > 0; i -= 2) {
        for (int k = 1; k >= 0; k--) {
            int left = pos[i] + k;
            int right = pos[i - 1] + k;
            if (left >= 0 && right >= 0 && left < enc_len && right < enc_len) {
                char t = enc[left];
                enc[left] = enc[right];
                enc[right] = t;
            }
        }
    }

    // 4. base64url → base64，非法字符原地过滤压缩
    int m = 0;
    for (int i = 0; i < enc_len; i++) {
        char c = enc[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '+' || c == '/')) continue;
        enc[m++] = c;
    }
    while (m % 4) enc[m++] = '=';

    // 5. base64 解码到第二个 PSRAM 缓冲（输出缓冲）
    size_t olen = 0;
    if (mbedtls_base64_decode(nullptr, 0, &olen, (const uint8_t*)enc, m) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL && olen == 0) {
        heap_caps_free(buf);
        return nullptr;
    }
    char* out = (char*)heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM);
    if (!out) { heap_caps_free(buf); return nullptr; }
    size_t written = 0;
    if (mbedtls_base64_decode((uint8_t*)out, olen, &written, (const uint8_t*)enc, m) != 0) {
        heap_caps_free(out);
        heap_caps_free(buf);
        return nullptr;
    }
    heap_caps_free(buf);
    out[written] = 0; // 补结尾便于字符串扫描
    *out_len = written;
    return out;
}

} // namespace weread
