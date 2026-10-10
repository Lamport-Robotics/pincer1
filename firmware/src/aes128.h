/* Minimal AES-128 (FIPS 197): single-block ECB encrypt/decrypt plus CBC and CMAC (NIST SP 800-38B /
 * RFC 4493) helpers. Written for Pincer1; no external licence. Not constant-time (table lookups): it
 * only ever protects the SCP03 link between this controller and the SE050, both inside the robot. */
#ifndef PINCER_AES128_H
#define PINCER_AES128_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t rk[11][16]; /* expanded round keys */
} aes128_t;

void aes128_init(aes128_t *c, const uint8_t key[16]);
void aes128_encrypt_block(const aes128_t *c, const uint8_t in[16], uint8_t out[16]);
void aes128_decrypt_block(const aes128_t *c, const uint8_t in[16], uint8_t out[16]);
/* CBC over whole blocks (len % 16 == 0). `in` and `out` may alias. */
void aes128_cbc_encrypt(const aes128_t *c, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
void aes128_cbc_decrypt(const aes128_t *c, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t len);
/* AES-CMAC, 16-byte tag. Two message pieces so callers can prepend a chaining value without copying. */
void aes128_cmac2(const aes128_t *c, const uint8_t *m1, size_t l1, const uint8_t *m2, size_t l2, uint8_t tag[16]);
void aes128_cmac(const aes128_t *c, const uint8_t *m, size_t len, uint8_t tag[16]);
void aes128_clear(aes128_t *c);
#endif
