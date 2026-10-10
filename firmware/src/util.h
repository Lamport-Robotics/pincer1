/* Pincer1 consent controller - small portable helpers (no hardware). */
#ifndef PINCER_UTIL_H
#define PINCER_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Base58 (Bitcoin/Solana alphabet). Returns number of chars written (no NUL) or -1 if cap too small. */
int base58_encode(const uint8_t *in, size_t in_len, char *out, size_t cap);
/* Decodes into out; returns decoded length or -1 on bad char / overflow. */
int base58_decode(const char *in, size_t in_len, uint8_t *out, size_t cap);
/* Convenience: exactly 32 bytes in, NUL-terminated string out (cap >= 45). */
int pubkey_to_b58(const uint8_t key[32], char *out, size_t cap);
/* Convenience: decode a base58 string that must be exactly 32 bytes. */
bool pubkey_from_b58(const char *s, uint8_t out[32]);

/* Standard base64 (with padding). */
int base64_encode(const uint8_t *in, size_t in_len, char *out, size_t cap);
int base64_decode(const char *in, size_t in_len, uint8_t *out, size_t cap);

/* u64 <-> decimal string. Returns false on overflow / junk. */
bool u64_from_dec(const char *s, uint64_t *out);
int u64_to_dec(uint64_t v, char *out, size_t cap);

/* Little-endian readers (unaligned safe). */
uint16_t rd_le16(const uint8_t *p);
uint32_t rd_le32(const uint8_t *p);
uint64_t rd_le64(const uint8_t *p);
void wr_le16(uint8_t *p, uint16_t v);
void wr_le32(uint8_t *p, uint32_t v);
void wr_le64(uint8_t *p, uint64_t v);

/* CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final xor). */
uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n);
uint32_t crc32_buf(const uint8_t *p, size_t n);

/* Constant-time compare; 0 if equal. */
int ct_memcmp(const void *a, const void *b, size_t n);
/* Zeroise that the optimiser may not remove. */
void secure_zero(void *p, size_t n);

/* Saturating add for lamport accounting. */
uint64_t u64_sat_add(uint64_t a, uint64_t b);

#endif
