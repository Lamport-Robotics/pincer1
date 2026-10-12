#include "ripemd160.h"

#include <string.h>

#include "sha256.h"
#include "util.h"

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static const uint8_t R1[80] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
                               3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12, 1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
                               4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13};
static const uint8_t R2[80] = {5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12, 6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
                               15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13, 8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
                               12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11};
static const uint8_t S1[80] = {11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8, 7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
                               11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5, 11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
                               9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6};
static const uint8_t S2[80] = {8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6, 9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
                               9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5, 15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
                               8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11};
static const uint32_t K1[5] = {0x00000000u, 0x5A827999u, 0x6ED9EBA1u, 0x8F1BBCDCu, 0xA953FD4Eu};
static const uint32_t K2[5] = {0x50A28BE6u, 0x5C4DD124u, 0x6D703EF3u, 0x7A6D76E9u, 0x00000000u};

static uint32_t f(int j, uint32_t x, uint32_t y, uint32_t z)
{
    switch (j / 16) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
    }
}

static void compress(uint32_t h[5], const uint8_t blk[64])
{
    uint32_t X[16];
    uint32_t a1, b1, c1, d1, e1, a2, b2, c2, d2, e2, t;
    int j;
    for (j = 0; j < 16; j++) X[j] = rd_le32(blk + 4 * j);
    a1 = a2 = h[0]; b1 = b2 = h[1]; c1 = c2 = h[2]; d1 = d2 = h[3]; e1 = e2 = h[4];
    for (j = 0; j < 80; j++) {
        t = ROL(a1 + f(j, b1, c1, d1) + X[R1[j]] + K1[j / 16], S1[j]) + e1;
        a1 = e1; e1 = d1; d1 = ROL(c1, 10); c1 = b1; b1 = t;
        t = ROL(a2 + f(79 - j, b2, c2, d2) + X[R2[j]] + K2[j / 16], S2[j]) + e2;
        a2 = e2; e2 = d2; d2 = ROL(c2, 10); c2 = b2; b2 = t;
    }
    t = h[1] + c1 + d2;
    h[1] = h[2] + d1 + e2;
    h[2] = h[3] + e1 + a2;
    h[3] = h[4] + a1 + b2;
    h[4] = h[0] + b1 + c2;
    h[0] = t;
}

void ripemd160(const uint8_t *p, size_t n, uint8_t out[20])
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint8_t blk[64];
    uint64_t bits = (uint64_t)n * 8;
    size_t i, rem;
    for (i = 0; i + 64 <= n; i += 64) compress(h, p + i);
    rem = n - i;
    memset(blk, 0, sizeof blk);
    memcpy(blk, p + i, rem);
    blk[rem] = 0x80;
    if (rem >= 56) {
        compress(h, blk);
        memset(blk, 0, sizeof blk);
    }
    wr_le64(blk + 56, bits);
    compress(h, blk);
    for (i = 0; i < 5; i++) wr_le32(out + 4 * i, h[i]);
    secure_zero(blk, sizeof blk);
}

void hash160(const uint8_t *p, size_t n, uint8_t out[20])
{
    uint8_t h[32];
    sha256(p, n, h);
    ripemd160(h, 32, out);
    secure_zero(h, sizeof h);
}
