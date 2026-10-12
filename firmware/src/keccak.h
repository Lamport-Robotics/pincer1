/* Keccak-256 (the pre-FIPS padding Ethereum uses, 0x01 not 0x06) on top of the vendored tiny_sha3
 * permutation (MIT, Markku-Juhani O. Saarinen). */
#ifndef PINCER_KECCAK_H
#define PINCER_KECCAK_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    union {
        uint8_t b[200];
        uint64_t q[25];
    } st;
    size_t pt;
} keccak_ctx;

void keccak256_init(keccak_ctx *c);
void keccak256_update(keccak_ctx *c, const uint8_t *p, size_t n);
void keccak256_final(keccak_ctx *c, uint8_t out[32]);
void keccak256(const uint8_t *p, size_t n, uint8_t out[32]);
#endif
