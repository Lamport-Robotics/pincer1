// ChaCha20-Poly1305 AEAD, RFC 8439 construction. Pure C, no dependencies, host-tested
// against the RFC's vectors (test/test_packet). 32-byte key, 12-byte nonce, 16-byte tag.
//
// This is a minimal, straightforward implementation: no SIMD, no secret-dependent branches,
// 32-bit arithmetic only, so it runs the same on the ESP32-S3, the ESP32 and the host.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEAD_KEY_LEN   32
#define AEAD_NONCE_LEN 12
#define AEAD_TAG_LEN   16

// ChaCha20 block function (RFC 8439 §2.3). 64 bytes of keystream for (key, nonce, counter).
void chacha20_block(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]);

// ChaCha20 stream cipher (§2.4): out = in XOR keystream starting at block `counter`. in/out may alias.
void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                  const uint8_t *in, uint8_t *out, size_t len);

// Poly1305 one-shot MAC (§2.5) with a 32-byte one-time key.
void poly1305_mac(const uint8_t key[32], const uint8_t *msg, size_t len, uint8_t tag[16]);

// AEAD encrypt (§2.8). ct has pt_len bytes, tag 16 bytes. pt/ct may alias.
void aead_encrypt(const uint8_t key[32], const uint8_t nonce[12],
                  const uint8_t *aad, size_t aad_len,
                  const uint8_t *pt, size_t pt_len,
                  uint8_t *ct, uint8_t tag[16]);

// AEAD decrypt. Returns 1 and writes pt on a valid tag; 0 (pt untouched) otherwise. Constant-time tag compare.
int aead_decrypt(const uint8_t key[32], const uint8_t nonce[12],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *ct, size_t ct_len, const uint8_t tag[16],
                 uint8_t *pt);

#ifdef __cplusplus
}
#endif
