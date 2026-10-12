#include "keccak.h"

#include <string.h>

#include "sha3.h"
#include "util.h"

#define RATE 136 /* 1600 - 2*256 bits */

void keccak256_init(keccak_ctx *c) { memset(c, 0, sizeof *c); }

void keccak256_update(keccak_ctx *c, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        c->st.b[c->pt++] ^= p[i];
        if (c->pt >= RATE) {
            sha3_keccakf(c->st.q);
            c->pt = 0;
        }
    }
}

void keccak256_final(keccak_ctx *c, uint8_t out[32])
{
    c->st.b[c->pt] ^= 0x01;
    c->st.b[RATE - 1] ^= 0x80;
    sha3_keccakf(c->st.q);
    memcpy(out, c->st.b, 32);
    secure_zero(c, sizeof *c);
}

void keccak256(const uint8_t *p, size_t n, uint8_t out[32])
{
    keccak_ctx c;
    keccak256_init(&c);
    keccak256_update(&c, p, n);
    keccak256_final(&c, out);
}
