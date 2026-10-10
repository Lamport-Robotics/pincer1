#include "secrets.h"

#include <string.h>

#include "util.h"

#if defined(PICO_RP2350) && PICO_RP2350
#include "pico/bootrom.h"
#endif

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool secrets_parse_hex_keys(const char *hex, scp03_keys_t *out)
{
    uint8_t raw[48];
    size_t i;
    if (!hex || strlen(hex) != 96) return false;
    for (i = 0; i < 48; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        raw[i] = (uint8_t)((hi << 4) | lo);
    }
    memcpy(out->enc, raw, 16);
    memcpy(out->mac, raw + 16, 16);
    memcpy(out->dek, raw + 32, 16);
    secure_zero(raw, sizeof raw);
    return true;
}

static bool all_same(const uint8_t *p, size_t n, uint8_t v)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i] != v) return false;
    return true;
}

#if defined(PICO_RP2350) && PICO_RP2350
static bool read_otp_keys(scp03_keys_t *out)
{
    uint8_t buf[SECRETS_OTP_ROWS * 2];
    otp_cmd_t cmd;
    int rc;
    cmd.flags = (SECRETS_OTP_ROW & OTP_CMD_ROW_BITS) | OTP_CMD_ECC_BITS; /* read, 2 bytes per row */
    rc = rom_func_otp_access(buf, sizeof buf, cmd);
    if (rc != BOOTROM_OK) return false;
    /* Blank OTP reads as zeros; treat all-zero or all-ones as "not provisioned". */
    if (all_same(buf, sizeof buf, 0x00) || all_same(buf, sizeof buf, 0xFF)) return false;
    memcpy(out->enc, buf, 16);
    memcpy(out->mac, buf + 16, 16);
    memcpy(out->dek, buf + 32, 16);
    secure_zero(buf, sizeof buf);
    return true;
}
#endif

secrets_source_t secrets_get_scp03_keys(scp03_keys_t *out)
{
    memset(out, 0, sizeof *out);
#if defined(PICO_RP2350) && PICO_RP2350
    if (read_otp_keys(out)) return SECRETS_OTP;
#endif
#ifdef PINCER_DEV_SCP03_KEYS
    if (secrets_parse_hex_keys(PINCER_DEV_SCP03_KEYS, out)) {
        /* a dev key set that is all zeros would be a configuration mistake, not a key */
        if (!all_same(out->enc, 16, 0) || !all_same(out->mac, 16, 0)) return SECRETS_DEV;
    }
#else
    (void)all_same;
#endif
    memset(out, 0, sizeof *out);
    return SECRETS_NONE;
}
