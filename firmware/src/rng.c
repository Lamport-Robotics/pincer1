#include "rng.h"

#include <string.h>

#include "hardware/adc.h"
#include "pico/rand.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "sha256.h"
#include "util.h"

#define RNG_ADC_GPIO 26 /* ADC0, left floating on the board */

void rng_init(void)
{
    adc_init();
    adc_gpio_init(RNG_ADC_GPIO);
    adc_select_input(0);
}

void rng_fill(uint8_t *out, size_t len)
{
    static uint32_t counter;
    while (len) {
        sha256_ctx c;
        uint8_t h[32];
        pico_unique_board_id_t id;
        uint64_t t;
        size_t take;
        int i;
        sha256_init(&c);
        sha256_update(&c, (const uint8_t *)&counter, sizeof counter);
        counter++;
        for (i = 0; i < 16; i++) {
            uint32_t r = get_rand_32(); /* RP2350: TRNG-fed (pico_rand); RP2040: ROSC + bus counters + timer */
            sha256_update(&c, (const uint8_t *)&r, sizeof r);
        }
        for (i = 0; i < 32; i++) {
            uint16_t a = adc_read();
            sha256_update(&c, (const uint8_t *)&a, sizeof a);
            busy_wait_us(23 + (a & 0x1f)); /* data-dependent jitter between samples */
        }
        t = time_us_64();
        sha256_update(&c, (const uint8_t *)&t, sizeof t);
        pico_get_unique_board_id(&id);
        sha256_update(&c, id.id, sizeof id.id);
        sha256_final(&c, h);
        take = len < 32 ? len : 32;
        memcpy(out, h, take);
        out += take;
        len -= take;
        secure_zero(h, sizeof h);
    }
}
