/* HMAC-SHA512 and PBKDF2-HMAC-SHA512 on top of the vendored orlp/ed25519 sha512 (zlib licence). */
#ifndef PINCER_HMAC_SHA512_H
#define PINCER_HMAC_SHA512_H
#include <stddef.h>
#include <stdint.h>

void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[64]);
/* Multi-part HMAC: two message pieces hashed as one (avoids a concat buffer for SLIP-0010). */
void hmac_sha512_2(const uint8_t *key, size_t key_len, const uint8_t *m1, size_t l1, const uint8_t *m2, size_t l2,
                   uint8_t out[64]);
void pbkdf2_hmac_sha512(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iters,
                        uint8_t *out, size_t out_len);
#endif
