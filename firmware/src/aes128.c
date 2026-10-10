#include "aes128.h"

#include <string.h>

#include "util.h"

static const uint8_t SBOX[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static uint8_t INV_SBOX[256];
static int inv_ready;

static void build_inverse(void)
{
    int i;
    if (inv_ready) return;
    for (i = 0; i < 256; i++) INV_SBOX[SBOX[i]] = (uint8_t)i;
    inv_ready = 1;
}

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static uint8_t gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    int i;
    for (i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

void aes128_init(aes128_t *c, const uint8_t key[16])
{
    uint8_t rcon = 1;
    int r, i;
    build_inverse();
    memcpy(c->rk[0], key, 16);
    for (r = 1; r <= 10; r++) {
        uint8_t t[4];
        const uint8_t *prev = c->rk[r - 1];
        uint8_t *cur = c->rk[r];
        t[0] = SBOX[prev[13]] ^ rcon;
        t[1] = SBOX[prev[14]];
        t[2] = SBOX[prev[15]];
        t[3] = SBOX[prev[12]];
        rcon = xtime(rcon);
        for (i = 0; i < 4; i++) cur[i] = prev[i] ^ t[i];
        for (i = 4; i < 16; i++) cur[i] = prev[i] ^ cur[i - 4];
    }
}

static void add_round_key(uint8_t s[16], const uint8_t rk[16])
{
    int i;
    for (i = 0; i < 16; i++) s[i] ^= rk[i];
}

static void sub_bytes(uint8_t s[16], const uint8_t *box)
{
    int i;
    for (i = 0; i < 16; i++) s[i] = box[s[i]];
}

/* State is column-major: s[c*4 + r]. ShiftRows rotates row r left by r. */
static void shift_rows(uint8_t s[16])
{
    uint8_t t;
    t = s[1]; s[1] = s[5]; s[5] = s[9]; s[9] = s[13]; s[13] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[15]; s[15] = s[11]; s[11] = s[7]; s[7] = s[3]; s[3] = t;
}

static void inv_shift_rows(uint8_t s[16])
{
    uint8_t t;
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    t = s[2]; s[2] = s[10]; s[10] = t; t = s[6]; s[6] = s[14]; s[14] = t;
    t = s[3]; s[3] = s[7]; s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

static void mix_columns(uint8_t s[16])
{
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t *col = s + 4 * c;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        col[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        col[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        col[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

static void inv_mix_columns(uint8_t s[16])
{
    int c;
    for (c = 0; c < 4; c++) {
        uint8_t *col = s + 4 * c;
        uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        col[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
        col[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
        col[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
        col[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
    }
}

void aes128_encrypt_block(const aes128_t *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    int r;
    memcpy(s, in, 16);
    add_round_key(s, c->rk[0]);
    for (r = 1; r < 10; r++) {
        sub_bytes(s, SBOX);
        shift_rows(s);
        mix_columns(s);
        add_round_key(s, c->rk[r]);
    }
    sub_bytes(s, SBOX);
    shift_rows(s);
    add_round_key(s, c->rk[10]);
    memcpy(out, s, 16);
}

void aes128_decrypt_block(const aes128_t *c, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    int r;
    memcpy(s, in, 16);
    add_round_key(s, c->rk[10]);
    for (r = 9; r >= 1; r--) {
        inv_shift_rows(s);
        sub_bytes(s, INV_SBOX);
        add_round_key(s, c->rk[r]);
        inv_mix_columns(s);
    }
    inv_shift_rows(s);
    sub_bytes(s, INV_SBOX);
    add_round_key(s, c->rk[0]);
    memcpy(out, s, 16);
}

void aes128_cbc_encrypt(const aes128_t *c, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t chain[16], blk[16];
    size_t i;
    memcpy(chain, iv, 16);
    for (i = 0; i + 16 <= len; i += 16) {
        int k;
        for (k = 0; k < 16; k++) blk[k] = in[i + k] ^ chain[k];
        aes128_encrypt_block(c, blk, chain);
        memcpy(out + i, chain, 16);
    }
}

void aes128_cbc_decrypt(const aes128_t *c, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len)
{
    uint8_t chain[16], next[16], blk[16];
    size_t i;
    memcpy(chain, iv, 16);
    for (i = 0; i + 16 <= len; i += 16) {
        int k;
        memcpy(next, in + i, 16);
        aes128_decrypt_block(c, next, blk);
        for (k = 0; k < 16; k++) out[i + k] = blk[k] ^ chain[k];
        memcpy(chain, next, 16);
    }
}

static void shift_left_1(const uint8_t in[16], uint8_t out[16])
{
    int i;
    uint8_t carry = 0;
    for (i = 15; i >= 0; i--) {
        uint8_t b = in[i];
        out[i] = (uint8_t)((b << 1) | carry);
        carry = (uint8_t)(b >> 7);
    }
}

void aes128_cmac2(const aes128_t *c, const uint8_t *m1, size_t l1, const uint8_t *m2, size_t l2, uint8_t tag[16])
{
    uint8_t L[16], K1[16], K2[16], x[16], blk[16];
    const uint8_t zero[16] = {0};
    size_t total = l1 + l2, nblocks, i, j;
    int last_complete;
    aes128_encrypt_block(c, zero, L);
    shift_left_1(L, K1);
    if (L[0] & 0x80) K1[15] ^= 0x87;
    shift_left_1(K1, K2);
    if (K1[0] & 0x80) K2[15] ^= 0x87;
    nblocks = (total + 15) / 16;
    if (nblocks == 0) nblocks = 1;
    last_complete = (total > 0) && (total % 16 == 0);
    memset(x, 0, 16);
    for (i = 0; i < nblocks; i++) {
        size_t off = i * 16;
        int is_last = (i + 1 == nblocks);
        for (j = 0; j < 16; j++) {
            size_t p = off + j;
            uint8_t b;
            if (p < l1) b = m1[p];
            else if (p < total) b = m2[p - l1];
            else b = (p == total) ? 0x80 : 0x00; /* 10* padding for the final partial block */
            blk[j] = b;
        }
        if (is_last) {
            const uint8_t *k = last_complete ? K1 : K2;
            for (j = 0; j < 16; j++) blk[j] ^= k[j];
        }
        for (j = 0; j < 16; j++) blk[j] ^= x[j];
        aes128_encrypt_block(c, blk, x);
    }
    memcpy(tag, x, 16);
    secure_zero(L, sizeof L);
    secure_zero(K1, sizeof K1);
    secure_zero(K2, sizeof K2);
}

void aes128_cmac(const aes128_t *c, const uint8_t *m, size_t len, uint8_t tag[16])
{
    aes128_cmac2(c, m, len, NULL, 0, tag);
}

void aes128_clear(aes128_t *c) { secure_zero(c, sizeof *c); }
