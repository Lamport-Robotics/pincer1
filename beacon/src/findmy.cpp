#include "findmy.h"
#include "sha256.h"
#include <string.h>

// secp224r1 order n = FFFFFFFFFFFFFFFFFFFFFFFFFFFF16A2E0B8F03E13DD29455C5C2A3D
const uint8_t FINDMY_P224_N[28] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x16, 0xa2, 0xe0, 0xb8, 0xf0, 0x3e, 0x13, 0xdd, 0x29, 0x45, 0x5c, 0x5c, 0x2a, 0x3d};

// ---------------------------------------------------------------- KDF

void findmy_x963_kdf(const uint8_t *z, size_t zlen, const char *info, uint8_t *out, size_t outlen) {
    uint32_t counter = 1;
    size_t ilen = strlen(info);
    while (outlen) {
        sha256_ctx c;
        uint8_t cb[4] = {(uint8_t)(counter >> 24), (uint8_t)(counter >> 16), (uint8_t)(counter >> 8), (uint8_t)counter};
        uint8_t h[32];
        sha256_init(&c);
        sha256_update(&c, z, zlen);
        sha256_update(&c, cb, 4);
        sha256_update(&c, (const uint8_t *)info, ilen);
        sha256_final(&c, h);
        size_t n = outlen < 32 ? outlen : 32;
        memcpy(out, h, n);
        out += n; outlen -= n; counter++;
    }
}

// ---------------------------------------------------------------- 256-bit helpers
// Values are 8 little-endian 32-bit limbs; all inputs stay below 2^224, so a doubling fits.

typedef struct { uint32_t v[8]; } u256;

static void u_from_be(u256 *r, const uint8_t *p, size_t len) {   // len ≤ 32
    memset(r, 0, sizeof *r);
    for (size_t i = 0; i < len; i++) {
        size_t byte = len - 1 - i;                                   // byte index from the end
        r->v[i / 4] |= (uint32_t)p[byte] << (8 * (i % 4));
    }
}
static void u_to_be28(const u256 *r, uint8_t out[28]) {
    for (int i = 0; i < 28; i++) out[27 - i] = (uint8_t)(r->v[i / 4] >> (8 * (i % 4)));
}
static int u_cmp(const u256 *a, const u256 *b) {
    for (int i = 7; i >= 0; i--) {
        if (a->v[i] > b->v[i]) return 1;
        if (a->v[i] < b->v[i]) return -1;
    }
    return 0;
}
static void u_sub(u256 *a, const u256 *b) {   // a -= b, requires a ≥ b
    uint64_t borrow = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t d = (uint64_t)a->v[i] - b->v[i] - borrow;
        a->v[i] = (uint32_t)d;
        borrow = (d >> 32) & 1;
    }
}
static void u_add(u256 *a, const u256 *b) {   // a += b (no overflow past 2^256 for our inputs)
    uint64_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint64_t s = (uint64_t)a->v[i] + b->v[i] + carry;
        a->v[i] = (uint32_t)s;
        carry = s >> 32;
    }
}
static void u_shl1(u256 *a) {
    uint32_t carry = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t nc = a->v[i] >> 31;
        a->v[i] = (a->v[i] << 1) | carry;
        carry = nc;
    }
}
static void u_reduce_once(u256 *a, const u256 *m) { if (u_cmp(a, m) >= 0) u_sub(a, m); }

void findmy_mod_be(const uint8_t *in, size_t len, const uint8_t m[28], uint8_t out[28]) {
    u256 mod, r;
    u_from_be(&mod, m, 28);
    memset(&r, 0, sizeof r);
    for (size_t i = 0; i < len; i++) {
        for (int b = 7; b >= 0; b--) {
            u_shl1(&r);
            r.v[0] |= (in[i] >> b) & 1;
            u_reduce_once(&r, &mod);
        }
    }
    u_to_be28(&r, out);
}

void findmy_muladd_mod(const uint8_t a[28], const uint8_t b[28], const uint8_t c[28], const uint8_t m[28], uint8_t out[28]) {
    u256 A, B, C, M, R;
    u_from_be(&A, a, 28); u_from_be(&B, b, 28); u_from_be(&C, c, 28); u_from_be(&M, m, 28);
    memset(&R, 0, sizeof R);
    for (int i = 223; i >= 0; i--) {                 // double-and-add, MSB first
        u_shl1(&R); u_reduce_once(&R, &M);
        if ((A.v[i / 32] >> (i % 32)) & 1) { u_add(&R, &B); u_reduce_once(&R, &M); }
    }
    u_add(&R, &C); u_reduce_once(&R, &M);
    u_to_be28(&R, out);
    memset(&A, 0, sizeof A); memset(&B, 0, sizeof B); memset(&C, 0, sizeof C); memset(&R, 0, sizeof R);
}

// x mod (n−1) + 1, x given as len big-endian bytes
static void scalar_in_range(const uint8_t *x, size_t len, uint8_t out[28]) {
    uint8_t n1[28];
    memcpy(n1, FINDMY_P224_N, 28);
    n1[27] -= 1;                                      // n is odd, no borrow
    findmy_mod_be(x, len, n1, out);
    for (int i = 27; i >= 0; i--) { if (++out[i]) break; }   // + 1 (result < n−1 so it fits)
}

// ---------------------------------------------------------------- key schedule

void findmy_init(findmy_state_t *s, const uint8_t seed[FINDMY_SEED_LEN]) {
    uint8_t t[32];
    findmy_x963_kdf(seed, FINDMY_SEED_LEN, "pincer1-findmy-d0", t, 32);
    scalar_in_range(t, 32, s->d0);
    findmy_x963_kdf(seed, FINDMY_SEED_LEN, "pincer1-findmy-sk0", s->sk, FINDMY_SK_LEN);
    s->index = 0;
    memset(t, 0, sizeof t);
}

void findmy_advance(findmy_state_t *s) {
    uint8_t next[32];
    findmy_x963_kdf(s->sk, FINDMY_SK_LEN, "update", next, 32);
    memcpy(s->sk, next, 32);
    memset(next, 0, sizeof next);
    s->index++;
}

void findmy_init_at(findmy_state_t *s, const uint8_t seed[FINDMY_SEED_LEN], uint32_t index) {
    findmy_init(s, seed);
    while (s->index < index) findmy_advance(s);
}

void findmy_private_key(const findmy_state_t *s, uint8_t d[FINDMY_KEY_LEN]) {
    uint8_t uv[72], u[28], v[28];
    findmy_x963_kdf(s->sk, FINDMY_SK_LEN, "diversify", uv, 72);
    scalar_in_range(uv, 36, u);
    scalar_in_range(uv + 36, 36, v);
    findmy_muladd_mod(s->d0, u, v, FINDMY_P224_N, d);
    memset(uv, 0, sizeof uv); memset(u, 0, sizeof u); memset(v, 0, sizeof v);
}

// ---------------------------------------------------------------- advertisement

size_t findmy_adv_payload(const uint8_t pub[FINDMY_KEY_LEN], uint8_t status, uint8_t out[FINDMY_ADV_LEN]) {
    out[0] = 0x1e;             // length of the manufacturer-specific AD structure
    out[1] = 0xff;             // manufacturer specific data
    out[2] = 0x4c; out[3] = 0x00;   // Apple
    out[4] = 0x12;             // offline finding
    out[5] = 0x19;             // 25 bytes follow
    out[6] = status;
    memcpy(out + 7, pub + 6, 22);
    out[29] = pub[0] >> 6;     // top two bits of the key, the rest of byte 0 rides in the address
    out[30] = 0x00;            // hint
    return FINDMY_ADV_LEN;
}

void findmy_adv_address(const uint8_t pub[FINDMY_KEY_LEN], uint8_t addr[6]) {
    addr[0] = pub[0] | 0xc0;   // random static address marker
    memcpy(addr + 1, pub + 1, 5);
}

void findmy_hashed_adv_key(const uint8_t pub[FINDMY_KEY_LEN], uint8_t out[32]) {
    sha256(pub, FINDMY_KEY_LEN, out);
}

uint8_t findmy_status_for_mv(uint16_t mv) {
    if (mv >= 3800) return FINDMY_STATUS_BATT_FULL;
    if (mv >= 3600) return FINDMY_STATUS_BATT_MEDIUM;
    if (mv >= 3400) return FINDMY_STATUS_BATT_LOW;
    return FINDMY_STATUS_BATT_CRITICAL;
}
