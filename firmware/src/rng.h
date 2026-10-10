/* Pincer1 - entropy source (target only). */
#ifndef PINCER_RNG_H
#define PINCER_RNG_H
#include <stddef.h>
#include <stdint.h>
void rng_init(void);
/* Fills `out` with `len` bytes. Each 32-byte block is SHA-256 over: a counter, 16 words from the
 * pico_rand generator (on the RP2350 seeded and mixed from the hardware TRNG, PICO_RAND_ENTROPY_SRC_TRNG;
 * on the RP2040 ROSC + bus counters + timer), 32 jittered ADC samples of a floating input (GP26/ADC0),
 * the microsecond clock and the unique board id. Never returns without filling the buffer. */
void rng_fill(uint8_t *out, size_t len);
#endif
