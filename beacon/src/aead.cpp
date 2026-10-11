#include "aead.h"
#include <string.h>

// ---------------------------------------------------------------- ChaCha20

#define ROTL(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
#define QR(a, b, c, d)                                       \
    a += b; d ^= a; d = ROTL(d, 16);                         \
    c += d; b ^= c; b = ROTL(b, 12);                         \
    a += b; d ^= a; d = ROTL(d, 8);                          \
    c += d; b ^= c; b = ROTL(b, 7);

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

void chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]) {
    uint32_t s[16], x[16];
    s[0] = 0x61707865; s[1] = 0x3320646e; s[2] = 0x79622d32; s[3] = 0x6b206574;
    for (int i = 0; i < 8; i++) s[4 + i] = ld32(key + 4 * i);
    s[12] = counter;
    s[13] = ld32(nonce); s[14] = ld32(nonce + 4); s[15] = ld32(nonce + 8);
    memcpy(x, s, sizeof x);
    for (int i = 0; i < 10; i++) {
        QR(x[0], x[4], x[8], x[12]) QR(x[1], x[5], x[9], x[13])
        QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
        QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12])
        QR(x[2], x[7], x[8], x[13]) QR(x[3], x[4], x[9], x[14])
    }
    for (int i = 0; i < 16; i++) st32(out + 4 * i, x[i] + s[i]);
}

void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                  const uint8_t *in, uint8_t *out, size_t len) {
    uint8_t ks[64];
    while (len) {
        chacha20_block(key, nonce, counter++, ks);
        size_t n = len < 64 ? len : 64;
        for (size_t i = 0; i < n; i++) out[i] = in[i] ^ ks[i];
        in += n; out += n; len -= n;
    }
    memset(ks, 0, sizeof ks);
}

// ---------------------------------------------------------------- Poly1305
// 26-bit limb representation (the classic portable "donna-32" layout), 32x32→64 products.

typedef struct {
    uint32_t r[5], h[5], pad[4];
    uint8_t  buf[16];
    size_t   buflen;
} poly_st;

static void poly_init(poly_st *st, const uint8_t key[32]) {
    uint32_t t0 = ld32(key), t1 = ld32(key + 4), t2 = ld32(key + 8), t3 = ld32(key + 12);
    st->r[0] = t0 & 0x3ffffff;
    st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
    st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
    st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
    st->r[4] = (t3 >> 8) & 0x00fffff;
    memset(st->h, 0, sizeof st->h);
    for (int i = 0; i < 4; i++) st->pad[i] = ld32(key + 16 + 4 * i);
    st->buflen = 0;
}

static void poly_blocks(poly_st *st, const uint8_t *m, size_t len, uint32_t hibit) {
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
    while (len >= 16) {
        uint32_t t0 = ld32(m), t1 = ld32(m + 4), t2 = ld32(m + 8), t3 = ld32(m + 12);
        h0 += t0 & 0x3ffffff;
        h1 += ((t0 >> 26) | (t1 << 6)) & 0x3ffffff;
        h2 += ((t1 >> 20) | (t2 << 12)) & 0x3ffffff;
        h3 += ((t2 >> 14) | (t3 << 18)) & 0x3ffffff;
        h4 += (t3 >> 8) | hibit;
        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
        m += 16; len -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly_finish(poly_st *st, uint8_t tag[16]) {
    if (st->buflen) {                                   // final partial block: append 1, zero-pad, no hibit
        st->buf[st->buflen] = 1;
        memset(st->buf + st->buflen + 1, 0, 16 - st->buflen - 1);
        poly_blocks(st, st->buf, 16, 0);
        st->buflen = 0;
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4], c;
    c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    // compute h + -p
    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);
    // select h if h < p, or h + -p if h >= p
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2; h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
    // h = h % 2^128
    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
    // mac = (h + pad) % 2^128
    uint64_t f;
    f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
    st32(tag, h0); st32(tag + 4, h1); st32(tag + 8, h2); st32(tag + 12, h3);
    memset(st, 0, sizeof *st);
}

// Streaming update: full 16-byte blocks are absorbed as they complete, the tail waits in buf.
static void poly_update(poly_st *st, const uint8_t *m, size_t len) {
    if (st->buflen) {
        size_t take = 16 - st->buflen;
        if (take > len) take = len;
        memcpy(st->buf + st->buflen, m, take);
        st->buflen += take; m += take; len -= take;
        if (st->buflen < 16) return;
        poly_blocks(st, st->buf, 16, 1u << 24);
        st->buflen = 0;
    }
    size_t full = len & ~(size_t)15;
    if (full) poly_blocks(st, m, full, 1u << 24);
    m += full; len -= full;
    if (len) { memcpy(st->buf, m, len); st->buflen = len; }
}

void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]) {
    poly_st st;
    poly_init(&st, key);
    poly_update(&st, msg, len);
    poly_finish(&st, tag);
}

// ---------------------------------------------------------------- AEAD (RFC 8439 §2.8)

static void poly_pad16(poly_st *st, size_t len) {
    static const uint8_t z[16] = {0};
    if (len % 16) poly_update(st, z, 16 - (len % 16));
}

static void aead_tag(const uint8_t key[32], const uint8_t nonce[12],
                     const uint8_t *aad, size_t aad_len,
                     const uint8_t *ct, size_t ct_len, uint8_t tag[16]) {
    uint8_t otk[64];
    chacha20_block(key, nonce, 0, otk);   // poly1305 one-time key = first 32 bytes of block 0
    poly_st st;
    poly_init(&st, otk);
    poly_update(&st, aad, aad_len); poly_pad16(&st, aad_len);
    poly_update(&st, ct, ct_len);   poly_pad16(&st, ct_len);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) { lens[i] = (uint8_t)((uint64_t)aad_len >> (8 * i)); lens[8 + i] = (uint8_t)((uint64_t)ct_len >> (8 * i)); }
    poly_update(&st, lens, 16);
    poly_finish(&st, tag);
    memset(otk, 0, sizeof otk);
}

void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t *pt, size_t pt_len,
                  uint8_t *ct, uint8_t tag[16]) {
    chacha20_xor(key, nonce, 1, pt, ct, pt_len);
    aead_tag(key, nonce, aad, aad_len, ct, pt_len, tag);
}

int aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *ct, size_t ct_len, const uint8_t tag[16],
                 uint8_t *pt) {
    uint8_t calc[16];
    aead_tag(key, nonce, aad, aad_len, ct, ct_len, calc);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= calc[i] ^ tag[i];
    memset(calc, 0, sizeof calc);
    if (diff) return 0;
    chacha20_xor(key, nonce, 1, ct, pt, ct_len);
    return 1;
}
