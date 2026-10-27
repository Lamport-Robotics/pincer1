/* Pincer1 - device secrets: the three static Platform SCP03 keys (ENC/MAC/DEK) that bind this
 * controller to its SE050.
 *
 * Production: 48 bytes in RP2350 OTP, rows SECRETS_OTP_ROW .. +23 (ECC mode, 16 bits per row), read
 * through the bootrom (rom_func_otp_access, pico/bootrom.h). Written once by scripts/provision-otp.sh.
 * Bench fallback: -DPINCER_DEV_SCP03_KEYS="<96 hex chars = ENC||MAC||DEK>" compiles the keys into the
 * image; it is used only when the OTP rows are blank, and `hello` reports dev_keys:true.
 * On RP2040 (no OTP) only the dev fallback exists. */
#ifndef PINCER_SECRETS_H
#define PINCER_SECRETS_H
#include <stdbool.h>
#include <stdint.h>

#define SECRETS_OTP_ROW 0x0C0u /* first user row (page 3); 0x080..0x0BF are the boot keys */
#define SECRETS_OTP_ROWS 24u   /* 48 bytes / 2 bytes per ECC row */

typedef struct {
    uint8_t enc[16];
    uint8_t mac[16];
    uint8_t dek[16];
} scp03_keys_t;

typedef enum { SECRETS_NONE = 0, SECRETS_OTP, SECRETS_DEV } secrets_source_t;

/* Fills `out`; returns where the keys came from (SECRETS_NONE = no keys, SE050 cannot be opened). */
secrets_source_t secrets_get_scp03_keys(scp03_keys_t *out);
/* Exposed for tests: 96 hex chars -> keys; false on any other input. */
bool secrets_parse_hex_keys(const char *hex, scp03_keys_t *out);
/* Key for the flash record MAC: HMAC-SHA256 over a fixed label under the three SCP03 keys, so a record is
 * bound to this controller's secrets (OTP in production). Without any keys `out` is all zero and the
 * return is SECRETS_NONE; the records are then only corruption-checked, like the chip they would unlock. */
secrets_source_t secrets_storage_key(uint8_t out[32]);
#endif
