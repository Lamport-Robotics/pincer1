/* Pincer1 - GlobalPlatform SCP03 secure channel to the SE050 (Platform SCP03, key set 0x0B).
 * PURE LOGIC over aes128.c: no transport. signer_se050.c moves the bytes.
 *
 * Byte formats mirror NXP Plug & Trust v04.08.02, which is what the SE050 applet expects:
 *   hostlib/hostLib/libCommon/nxScp/nxScp03_Com.c        (C-ENC / C-MAC / R-MAC / R-ENC, counters)
 *   sss/src/se05x/fsl_sss_se05x_scp03.c                  (INITIALIZE UPDATE, EXTERNAL AUTHENTICATE,
 *                                                         session-key KDF, cryptograms)
 *   hostlib/hostLib/se05x/src/se05x_tlv.c se05x_Transform_scp (session-less wrapping of an APDU)
 * Security level is always C-MAC + C-ENC + R-MAC + R-ENC (0x33).
 *
 * Counter rule ("SE050 mode", applet < 04.03.00, kSSS_AuthType_SCP03 in the middleware): the command
 * counter advances only after a successful response to a command that carried data; a data-less
 * command decrypts its response with counter-1; an error response to a command whose plain length
 * was not 8 also advances it. "SE051 mode" (applet >= 04.03.00, kSSS_AuthType_AESKey): advance after
 * every response, no decrement. scp03_set_applet_version() picks the mode like the middleware does. */
#ifndef PINCER_SCP03_H
#define PINCER_SCP03_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "aes128.h"

#define SCP03_KEY_LEN 16
#define SCP03_CHALLENGE_LEN 8
#define SCP03_CRYPTOGRAM_LEN 8
#define SCP03_MAC_LEN 8
#define SCP03_KEY_VERSION_SE050 0x0B /* platform key set of the SE050 (EX_SSS_AUTH_SE05X_KEY_VERSION_NO) */
#define SCP03_INIT_UPDATE_APDU_LEN 13 /* 80 50 kv 00 08 <8 host challenge> */
#define SCP03_INIT_UPDATE_RSP_MIN 31  /* 10 div + 3 info + 8 card ch + 8 cryptogram + SW */
#define SCP03_INIT_UPDATE_RSP_MAX 34  /* + 3 byte sequence counter */
#define SCP03_EXT_AUTH_APDU_LEN 21    /* 84 82 33 00 10 <8 cryptogram> <8 mac> */
#define SCP03_MAX_PLAIN 1024          /* largest plain command body we wrap */

typedef struct {
    bool open;
    bool count_every_cmd; /* SE051 mode, see header comment */
    aes128_t s_enc, s_mac, s_rmac;
    uint8_t mcv[16];     /* MAC chaining value */
    uint8_t counter[16]; /* command counter, big-endian */
} scp03_t;

void scp03_init(scp03_t *s);
/* Applet version as 0xMMmmpp00 (SELECT response bytes 0..2). Default: SE050 mode. */
void scp03_set_applet_version(scp03_t *s, uint32_t applet_version);

/* ---- handshake (the caller sends the APDUs plain) ---- */
size_t scp03_build_initialize_update(const uint8_t host_challenge[8], uint8_t key_version,
                                     uint8_t out[SCP03_INIT_UPDATE_APDU_LEN]);
/* Parses the INITIALIZE UPDATE response, derives the session keys and checks the card cryptogram.
 * Returns 0 and fills `ext_auth` (ready to send) on success; -1 on a bad length, SW or cryptogram. */
int scp03_handshake(scp03_t *s, const uint8_t enc[16], const uint8_t mac[16], const uint8_t host_challenge[8],
                    const uint8_t *iu_rsp, size_t iu_rsp_len, uint8_t ext_auth[SCP03_EXT_AUTH_APDU_LEN]);
/* Call with the EXTERNAL AUTHENTICATE status word (0x9000 opens the channel). */
int scp03_finish(scp03_t *s, uint16_t sw);

/* ---- data phase ---- */
/* Wraps header(4) + plain data into a protected APDU in `out`. `hasle` appends Le 00 00.
 * Returns 0 or -1 (not open / too long). */
int scp03_wrap(scp03_t *s, const uint8_t hdr[4], const uint8_t *data, size_t len, bool hasle, uint8_t *out,
               size_t cap, size_t *out_len);
/* Verifies and decrypts a response in place. `plain_cmd_len` is the length passed to scp03_wrap for
 * the command this answers. On return `rsp` holds plain data + SW. Returns 0, or -1 when the R-MAC
 * or padding is wrong (the response must then be discarded). An error SW (not 0x9000) is returned
 * as 0 with the 2-byte SW only; the counter is advanced per the mode rules. */
int scp03_unwrap(scp03_t *s, size_t plain_cmd_len, uint8_t *rsp, size_t *rsp_len);
void scp03_close(scp03_t *s);

/* Exposed for tests. */
void scp03_kdf(const aes128_t *key, uint8_t constant, uint16_t bits, const uint8_t context[16], uint8_t out[16]);
#endif
