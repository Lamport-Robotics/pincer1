#include "util.h"

#include <string.h>

static const char B58[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

int base58_encode(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    /* Max output = in_len * 138/100 + 1. Work buffer sized for 64-byte inputs. */
    uint8_t digits[96];
    size_t ndig = 0, zeros = 0, i, j;
    if (in_len > 64) return -1;
    while (zeros < in_len && in[zeros] == 0) zeros++;
    for (i = zeros; i < in_len; i++) {
        uint32_t carry = in[i];
        for (j = 0; j < ndig; j++) {
            carry += (uint32_t)digits[j] << 8;
            digits[j] = (uint8_t)(carry % 58);
            carry /= 58;
        }
        while (carry) {
            if (ndig >= sizeof digits) return -1;
            digits[ndig++] = (uint8_t)(carry % 58);
            carry /= 58;
        }
    }
    if (zeros + ndig + 1 > cap) return -1;
    for (i = 0; i < zeros; i++) out[i] = '1';
    for (i = 0; i < ndig; i++) out[zeros + i] = B58[digits[ndig - 1 - i]];
    out[zeros + ndig] = 0;
    return (int)(zeros + ndig);
}

int base58_decode(const char *in, size_t in_len, uint8_t *out, size_t cap)
{
    uint8_t bytes[96];
    size_t nb = 0, zeros = 0, i, j;
    if (in_len > 128) return -1;
    while (zeros < in_len && in[zeros] == '1') zeros++;
    for (i = zeros; i < in_len; i++) {
        const char *p = strchr(B58, in[i]);
        uint32_t carry;
        if (!p || in[i] == 0) return -1;
        carry = (uint32_t)(p - B58);
        for (j = 0; j < nb; j++) {
            carry += (uint32_t)bytes[j] * 58;
            bytes[j] = (uint8_t)(carry & 0xff);
            carry >>= 8;
        }
        while (carry) {
            if (nb >= sizeof bytes) return -1;
            bytes[nb++] = (uint8_t)(carry & 0xff);
            carry >>= 8;
        }
    }
    if (zeros + nb > cap) return -1;
    for (i = 0; i < zeros; i++) out[i] = 0;
    for (i = 0; i < nb; i++) out[zeros + i] = bytes[nb - 1 - i];
    return (int)(zeros + nb);
}

int pubkey_to_b58(const uint8_t key[32], char *out, size_t cap)
{
    return base58_encode(key, 32, out, cap);
}

bool pubkey_from_b58(const char *s, uint8_t out[32])
{
    size_t n = s ? strlen(s) : 0;
    if (n < 32 || n > 44) return false;
    return base58_decode(s, n, out, 32) == 32;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap)
{
    size_t o = 0, i;
    if (((in_len + 2) / 3) * 4 + 1 > cap) return -1;
    for (i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = (i + 1 < in_len) ? B64[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < in_len) ? B64[v & 63] : '=';
    }
    out[o] = 0;
    return (int)o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap)
{
    size_t o = 0, i;
    uint32_t acc = 0;
    int bits = 0;
    for (i = 0; i < in_len; i++) {
        int v;
        if (in[i] == '=') break;
        if (in[i] == '\r' || in[i] == '\n') continue;
        v = b64val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= cap) return -1;
            out[o++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    return (int)o;
}

bool u64_from_dec(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!s || !*s) return false;
    for (; *s; s++) {
        uint64_t d;
        if (*s < '0' || *s > '9') return false;
        d = (uint64_t)(*s - '0');
        if (v > (UINT64_MAX - d) / 10) return false;
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

int u64_to_dec(uint64_t v, char *out, size_t cap)
{
    char tmp[21];
    int n = 0, i;
    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    if ((size_t)n + 1 > cap) return -1;
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

uint16_t rd_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint64_t rd_le64(const uint8_t *p) { return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32); }
void wr_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void wr_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
void wr_le64(uint8_t *p, uint64_t v) { wr_le32(p, (uint32_t)v); wr_le32(p + 4, (uint32_t)(v >> 32)); }

uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    size_t i;
    int k;
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

uint32_t crc32_buf(const uint8_t *p, size_t n) { return crc32_update(0, p, n); }

int ct_memcmp(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t d = 0;
    size_t i;
    for (i = 0; i < n; i++) d |= (uint8_t)(x[i] ^ y[i]);
    return d;
}

void secure_zero(void *p, size_t n)
{
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

uint64_t u64_sat_add(uint64_t a, uint64_t b) { return (a > UINT64_MAX - b) ? UINT64_MAX : a + b; }
