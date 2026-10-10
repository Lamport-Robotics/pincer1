/* Pincer1 - 256-bit unsigned integers for EVM amounts (wei, ERC-20 units, gas fees). PURE LOGIC.
 * Limb 0 is the least significant. Written for Pincer1; no external licence. */
#ifndef PINCER_U256_H
#define PINCER_U256_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t w[4];
} u256_t;

#define U256_DEC_MAX 79 /* 78 digits + NUL */

void u256_zero(u256_t *v);
void u256_from_u64(u256_t *v, uint64_t x);
/* Big-endian bytes, 0..32 of them (shorter inputs are right-aligned). Returns false when len > 32. */
bool u256_from_be(u256_t *v, const uint8_t *be, size_t len);
void u256_to_be32(const u256_t *v, uint8_t out[32]);
/* -1 / 0 / +1 */
int u256_cmp(const u256_t *a, const u256_t *b);
bool u256_is_zero(const u256_t *v);
bool u256_is_max(const u256_t *v);          /* 2^256 - 1 */
bool u256_fits_u64(const u256_t *v);
uint64_t u256_low64(const u256_t *v);
/* a + b, saturating at 2^256 - 1; returns true when it saturated. */
bool u256_add_sat(u256_t *out, const u256_t *a, const u256_t *b);
/* a * b; returns false (and sets out to max) on overflow. */
bool u256_mul(u256_t *out, const u256_t *a, const u256_t *b);
/* Two's complement negation (for EIP-712 intN values). */
void u256_neg(u256_t *v);
/* Decimal string (digits only, no sign) -> value. False on junk / overflow / empty. */
bool u256_from_dec(const char *s, u256_t *out);
/* "0x" + 1..64 hex digits -> value. False otherwise. */
bool u256_from_hex(const char *s, u256_t *out);
/* Decimal string, NUL-terminated. Returns length or -1 when cap is too small. */
int u256_to_dec(const u256_t *v, char *out, size_t cap);
#endif
