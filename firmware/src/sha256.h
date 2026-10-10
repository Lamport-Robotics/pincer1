/* Minimal SHA-256 (FIPS 180-4). Written for Pincer1; no external licence. */
#ifndef PINCER_SHA256_H
#define PINCER_SHA256_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t buf_len;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const uint8_t *p, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const uint8_t *p, size_t n, uint8_t out[32]);
/* SHA256(SHA256(p)): Bitcoin txids and sighashes. */
void sha256d(const uint8_t *p, size_t n, uint8_t out[32]);

/* HMAC-SHA256 (RFC 2104): RFC 6979 nonces. */
typedef struct {
    sha256_ctx inner, outer;
} hmac_sha256_ctx;
void hmac_sha256_init(hmac_sha256_ctx *h, const uint8_t *key, size_t key_len);
void hmac_sha256_update(hmac_sha256_ctx *h, const uint8_t *p, size_t n);
void hmac_sha256_final(hmac_sha256_ctx *h, uint8_t out[32]);
void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *p, size_t n, uint8_t out[32]);
#endif
