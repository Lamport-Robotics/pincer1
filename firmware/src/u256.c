#include "u256.h"

#include <string.h>

void u256_zero(u256_t *v) { memset(v, 0, sizeof *v); }

void u256_from_u64(u256_t *v, uint64_t x)
{
    u256_zero(v);
    v->w[0] = x;
}

bool u256_from_be(u256_t *v, const uint8_t *be, size_t len)
{
    size_t i;
    u256_zero(v);
    if (len > 32) return false;
    for (i = 0; i < len; i++) {
        size_t bit_from_lsb = (len - 1 - i) * 8;
        v->w[bit_from_lsb / 64] |= (uint64_t)be[i] << (bit_from_lsb % 64);
    }
    return true;
}

void u256_to_be32(const u256_t *v, uint8_t out[32])
{
    int i;
    for (i = 0; i < 32; i++) out[i] = (uint8_t)(v->w[3 - i / 8] >> (56 - 8 * (i % 8)));
}

int u256_cmp(const u256_t *a, const u256_t *b)
{
    int i;
    for (i = 3; i >= 0; i--) {
        if (a->w[i] < b->w[i]) return -1;
        if (a->w[i] > b->w[i]) return 1;
    }
    return 0;
}

bool u256_is_zero(const u256_t *v) { return (v->w[0] | v->w[1] | v->w[2] | v->w[3]) == 0; }
bool u256_is_max(const u256_t *v) { return (v->w[0] & v->w[1] & v->w[2] & v->w[3]) == UINT64_MAX; }
bool u256_fits_u64(const u256_t *v) { return (v->w[1] | v->w[2] | v->w[3]) == 0; }
uint64_t u256_low64(const u256_t *v) { return v->w[0]; }

static void set_max(u256_t *v) { v->w[0] = v->w[1] = v->w[2] = v->w[3] = UINT64_MAX; }

bool u256_add_sat(u256_t *out, const u256_t *a, const u256_t *b)
{
    u256_t r;
    uint64_t carry = 0;
    int i;
    for (i = 0; i < 4; i++) {
        uint64_t s = a->w[i] + b->w[i];
        uint64_t c1 = s < a->w[i];
        uint64_t s2 = s + carry;
        uint64_t c2 = s2 < s;
        r.w[i] = s2;
        carry = c1 | c2;
    }
    if (carry) {
        set_max(out);
        return true;
    }
    *out = r;
    return false;
}

static void mul64(uint64_t a, uint64_t b, uint64_t *lo, uint64_t *hi)
{
    uint64_t a0 = a & 0xffffffffu, a1 = a >> 32, b0 = b & 0xffffffffu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xffffffffu) + (p10 & 0xffffffffu);
    *lo = (mid << 32) | (p00 & 0xffffffffu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

bool u256_mul(u256_t *out, const u256_t *a, const u256_t *b)
{
    uint64_t r[8] = {0};
    int i, j;
    bool overflow = false;
    for (i = 0; i < 4; i++) {
        uint64_t carry = 0;
        for (j = 0; j < 4; j++) {
            uint64_t lo, hi, s;
            mul64(a->w[i], b->w[j], &lo, &hi);
            s = r[i + j] + lo;
            hi += s < lo;
            s += carry;
            hi += s < carry;
            r[i + j] = s;
            carry = hi;
        }
        r[i + 4] += carry; /* cannot overflow: the slot was untouched (< 2^64 - 1 total) */
    }
    for (i = 4; i < 8; i++)
        if (r[i]) overflow = true;
    if (overflow) {
        set_max(out);
        return false;
    }
    for (i = 0; i < 4; i++) out->w[i] = r[i];
    return true;
}

void u256_neg(u256_t *v)
{
    uint64_t carry = 1;
    int i;
    for (i = 0; i < 4; i++) {
        uint64_t x = ~v->w[i] + carry;
        carry = (carry && x == 0) ? 1 : 0;
        v->w[i] = x;
    }
}

static bool mul_small_add(u256_t *v, uint64_t m, uint64_t add)
{
    uint64_t carry = add;
    int i;
    for (i = 0; i < 4; i++) {
        uint64_t lo, hi;
        mul64(v->w[i], m, &lo, &hi);
        lo += carry;
        hi += lo < carry;
        v->w[i] = lo;
        carry = hi;
    }
    return carry == 0;
}

bool u256_from_dec(const char *s, u256_t *out)
{
    u256_t v;
    size_t n = 0;
    u256_zero(&v);
    if (!s || !*s) return false;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9' || n > 78) return false;
        if (!mul_small_add(&v, 10, (uint64_t)(*s - '0'))) return false;
    }
    *out = v;
    return true;
}

bool u256_from_hex(const char *s, u256_t *out)
{
    u256_t v;
    size_t n = 0;
    u256_zero(&v);
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || !s[2]) return false;
    for (s += 2; *s; s++, n++) {
        unsigned d;
        if (n >= 64) return false;
        if (*s >= '0' && *s <= '9') d = (unsigned)(*s - '0');
        else if (*s >= 'a' && *s <= 'f') d = (unsigned)(*s - 'a' + 10);
        else if (*s >= 'A' && *s <= 'F') d = (unsigned)(*s - 'A' + 10);
        else return false;
        mul_small_add(&v, 16, d);
    }
    *out = v;
    return true;
}

int u256_to_dec(const u256_t *v, char *out, size_t cap)
{
    char tmp[U256_DEC_MAX];
    u256_t cur = *v;
    size_t n = 0, i;
    do {
        /* long division by 10 over eight 32-bit limbs (no 128-bit type needed on Cortex-M) */
        uint64_t rem = 0;
        int k;
        for (k = 7; k >= 0; k--) {
            uint64_t limb = (cur.w[k / 2] >> (32 * (k % 2))) & 0xffffffffu;
            uint64_t v = (rem << 32) | limb;
            uint64_t q = v / 10;
            rem = v % 10;
            cur.w[k / 2] &= ~((uint64_t)0xffffffffu << (32 * (k % 2)));
            cur.w[k / 2] |= q << (32 * (k % 2));
        }
        tmp[n++] = (char)('0' + rem);
    } while (!u256_is_zero(&cur) && n < sizeof tmp);
    if (n + 1 > cap) return -1;
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return (int)n;
}
