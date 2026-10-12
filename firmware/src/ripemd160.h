/* RIPEMD-160 (Dobbertin, Bosselaers, Preneel 1996) for Bitcoin hash160. Written for Pincer1; no external licence. */
#ifndef PINCER_RIPEMD160_H
#define PINCER_RIPEMD160_H
#include <stddef.h>
#include <stdint.h>

void ripemd160(const uint8_t *p, size_t n, uint8_t out[20]);
/* RIPEMD160(SHA256(p)) */
void hash160(const uint8_t *p, size_t n, uint8_t out[20]);
#endif
