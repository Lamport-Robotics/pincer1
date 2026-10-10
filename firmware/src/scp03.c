#include "scp03.h"

#include <string.h>

#include "util.h"

/* Derivation constants (GP Amd D 4.1.5, nxScp03_Const.h). */
#define DD_CARD_CRYPTOGRAM 0x00
#define DD_HOST_CRYPTOGRAM 0x01
#define DD_SENC 0x04
#define DD_SMAC 0x06
#define DD_SRMAC 0x07
#define DD_L_64 0x0040
#define DD_L_128 0x0080
#define DD_LABEL_LEN 12
#define CLA_GP 0x80
#define CLA_SECURE 0x04
#define INS_INITIALIZE_UPDATE 0x50
#define INS_EXTERNAL_AUTHENTICATE 0x82
#define SECLVL_ALL 0x33
#define PAD_BYTE 0x80

void scp03_init(scp03_t *s) { memset(s, 0, sizeof *s); }

void scp03_set_applet_version(scp03_t *s, uint32_t applet_version)
{
    /* fsl_sss_se05x_apis.c: appletVersion >= 0x04030000 -> kSSS_AuthType_AESKey counting */
    s->count_every_cmd = applet_version >= 0x04030000u;
}

/* nxScp03_setDerivationData: 11 zero bytes, constant, 00, L (2 bytes), counter 01, context. */
void scp03_kdf(const aes128_t *key, uint8_t constant, uint16_t bits, const uint8_t context[16], uint8_t out[16])
{
    uint8_t dd[DD_LABEL_LEN + 4 + 16];
    memset(dd, 0, DD_LABEL_LEN - 1);
    dd[DD_LABEL_LEN - 1] = constant;
    dd[DD_LABEL_LEN] = 0x00;
    dd[DD_LABEL_LEN + 1] = (uint8_t)(bits >> 8);
    dd[DD_LABEL_LEN + 2] = (uint8_t)bits;
    dd[DD_LABEL_LEN + 3] = 0x01;
    memcpy(dd + DD_LABEL_LEN + 4, context, 16);
    aes128_cmac(key, dd, sizeof dd, out);
}

size_t scp03_build_initialize_update(const uint8_t host_challenge[8], uint8_t key_version,
                                     uint8_t out[SCP03_INIT_UPDATE_APDU_LEN])
{
    out[0] = CLA_GP;
    out[1] = INS_INITIALIZE_UPDATE;
    out[2] = key_version;
    out[3] = 0x00;
    out[4] = SCP03_CHALLENGE_LEN;
    memcpy(out + 5, host_challenge, SCP03_CHALLENGE_LEN);
    return SCP03_INIT_UPDATE_APDU_LEN;
}

int scp03_handshake(scp03_t *s, const uint8_t enc[16], const uint8_t mac[16], const uint8_t host_challenge[8],
                    const uint8_t *iu_rsp, size_t iu_rsp_len, uint8_t ext_auth[SCP03_EXT_AUTH_APDU_LEN])
{
    aes128_t k_enc, k_mac;
    uint8_t context[16], tmp[16], card_cg[16];
    const uint8_t *card_challenge, *card_cryptogram;
    uint16_t sw;
    const uint8_t zero_mcv[16] = {0};
    int rc = -1;

    s->open = false;
    if (iu_rsp_len != SCP03_INIT_UPDATE_RSP_MIN && iu_rsp_len != SCP03_INIT_UPDATE_RSP_MAX) return -1;
    sw = (uint16_t)((iu_rsp[iu_rsp_len - 2] << 8) | iu_rsp[iu_rsp_len - 1]);
    if (sw != 0x9000) return -1;
    card_challenge = iu_rsp + 10 + 3;
    card_cryptogram = card_challenge + 8;

    memcpy(context, host_challenge, 8);
    memcpy(context + 8, card_challenge, 8);
    aes128_init(&k_enc, enc);
    aes128_init(&k_mac, mac);
    scp03_kdf(&k_enc, DD_SENC, DD_L_128, context, tmp);
    aes128_init(&s->s_enc, tmp);
    scp03_kdf(&k_mac, DD_SMAC, DD_L_128, context, tmp);
    aes128_init(&s->s_mac, tmp);
    scp03_kdf(&k_mac, DD_SRMAC, DD_L_128, context, tmp);
    aes128_init(&s->s_rmac, tmp);

    /* card cryptogram: first 8 bytes of KDF(S-MAC, 0x00, 64 bits) */
    scp03_kdf(&s->s_mac, DD_CARD_CRYPTOGRAM, DD_L_64, context, card_cg);
    if (ct_memcmp(card_cg, card_cryptogram, SCP03_CRYPTOGRAM_LEN) != 0) goto out;

    /* EXTERNAL AUTHENTICATE: 84 82 33 00 10 | host cryptogram | MAC(MCV=0, first 13 bytes) */
    ext_auth[0] = CLA_GP | CLA_SECURE;
    ext_auth[1] = INS_EXTERNAL_AUTHENTICATE;
    ext_auth[2] = SECLVL_ALL;
    ext_auth[3] = 0x00;
    ext_auth[4] = 0x10; /* Lc as if the MAC were already appended */
    scp03_kdf(&s->s_mac, DD_HOST_CRYPTOGRAM, DD_L_64, context, tmp);
    memcpy(ext_auth + 5, tmp, SCP03_CRYPTOGRAM_LEN);
    aes128_cmac2(&s->s_mac, zero_mcv, 16, ext_auth, 13, s->mcv);
    memcpy(ext_auth + 13, s->mcv, SCP03_MAC_LEN);
    memset(s->counter, 0, 16);
    s->counter[15] = 0x01;
    rc = 0;
out:
    aes128_clear(&k_enc);
    aes128_clear(&k_mac);
    secure_zero(tmp, sizeof tmp);
    secure_zero(card_cg, sizeof card_cg);
    secure_zero(context, sizeof context);
    if (rc != 0) scp03_close(s);
    return rc;
}

int scp03_finish(scp03_t *s, uint16_t sw)
{
    if (sw != 0x9000) {
        scp03_close(s);
        return -1;
    }
    s->open = true;
    return 0;
}

static void counter_inc(uint8_t c[16])
{
    int i = 15;
    while (i > 0) {
        if (c[i] < 255) { c[i]++; break; }
        c[i] = 0;
        i--;
    }
}

static void counter_dec(uint8_t c[16])
{
    int i = 15;
    while (i > 0) {
        if (c[i] == 0) { c[i] = 0xFF; i--; }
        else { c[i]--; break; }
    }
}

int scp03_wrap(scp03_t *s, const uint8_t hdr[4], const uint8_t *data, size_t len, bool hasle, uint8_t *out,
               size_t cap, size_t *out_len)
{
    uint8_t padded[SCP03_MAX_PLAIN + 16];
    uint8_t icv[16], mac[16];
    const uint8_t zero_iv[16] = {0};
    size_t enc_len = 0, lc, i = 0, mac_start;
    if (!s->open || len > SCP03_MAX_PLAIN) return -1;
    if (len > 0) {
        /* pad 80 00.. to a block, encrypt with ICV = AES(S-ENC, counter) */
        memcpy(padded, data, len);
        padded[len] = PAD_BYTE;
        enc_len = len + 1;
        while (enc_len % 16) padded[enc_len++] = 0x00;
        aes128_cbc_encrypt(&s->s_enc, zero_iv, s->counter, icv, 16);
        aes128_cbc_encrypt(&s->s_enc, icv, padded, padded, enc_len);
    }
    lc = enc_len + SCP03_MAC_LEN;
    if (4 + 3 + enc_len + SCP03_MAC_LEN + 2 > cap) return -1;
    out[i++] = (uint8_t)(hdr[0] | CLA_SECURE);
    out[i++] = hdr[1];
    out[i++] = hdr[2];
    out[i++] = hdr[3];
    if (lc < 0xFF && !hasle) {
        out[i++] = (uint8_t)lc;
    } else {
        out[i++] = 0x00;
        out[i++] = (uint8_t)(lc >> 8);
        out[i++] = (uint8_t)lc;
    }
    memcpy(out + i, padded, enc_len);
    i += enc_len;
    mac_start = i;
    /* C-MAC over MCV || header || Lc || encrypted data; the full 16-byte MAC becomes the new MCV */
    aes128_cmac2(&s->s_mac, s->mcv, 16, out, mac_start, mac);
    memcpy(s->mcv, mac, 16);
    memcpy(out + i, mac, SCP03_MAC_LEN);
    i += SCP03_MAC_LEN;
    if (hasle) {
        out[i++] = 0x00;
        out[i++] = 0x00;
    }
    *out_len = i;
    secure_zero(padded, sizeof padded);
    secure_zero(icv, sizeof icv);
    secure_zero(mac, sizeof mac);
    return 0;
}

int scp03_unwrap(scp03_t *s, size_t plain_cmd_len, uint8_t *rsp, size_t *rsp_len)
{
    uint8_t sw[2], rmac[16], icv[16], ctr[16];
    const uint8_t zero_iv[16] = {0};
    size_t n = *rsp_len, data_len;
    uint16_t status;
    if (!s->open || n < 2) return -1;
    sw[0] = rsp[n - 2];
    sw[1] = rsp[n - 1];
    status = (uint16_t)((sw[0] << 8) | sw[1]);
    if (status != 0x9000) {
        /* se05x_DeCrypt: error responses carry no MAC; advance the counter as the middleware does */
        if (s->count_every_cmd || plain_cmd_len != 8) counter_inc(s->counter);
        rsp[0] = sw[0];
        rsp[1] = sw[1];
        *rsp_len = 2;
        return 0;
    }
    if (n < SCP03_MAC_LEN + 2) return -1; /* fail closed: a 9000 without an R-MAC is not acceptable */
    data_len = n - SCP03_MAC_LEN - 2;
    /* R-MAC = CMAC(S-RMAC, MCV || data || SW); the MCV is the one set by the command. aes128_cmac2
     * takes two pieces, so the SW is temporarily written over the received MAC to make data||SW
     * contiguous, then the MAC is restored for the comparison. */
    {
        uint8_t saved[SCP03_MAC_LEN];
        memcpy(saved, rsp + data_len, SCP03_MAC_LEN);
        rsp[data_len] = sw[0];
        rsp[data_len + 1] = sw[1];
        aes128_cmac2(&s->s_rmac, s->mcv, 16, rsp, data_len + 2, rmac);
        memcpy(rsp + data_len, saved, SCP03_MAC_LEN);
        if (ct_memcmp(rmac, rsp + data_len, SCP03_MAC_LEN) != 0) {
            secure_zero(rmac, sizeof rmac);
            return -1;
        }
    }
    if (data_len > 0) {
        size_t i;
        bool ok = false;
        if (data_len % 16) return -1;
        memcpy(ctr, s->counter, 16);
        if (!s->count_every_cmd && plain_cmd_len == 0) counter_dec(ctr);
        ctr[0] = PAD_BYTE;
        aes128_cbc_encrypt(&s->s_enc, zero_iv, ctr, icv, 16);
        aes128_cbc_decrypt(&s->s_enc, icv, rsp, rsp, data_len);
        /* strip 80 00.. padding, which must lie within the last block */
        i = data_len;
        while (i > 1 && i > data_len - 16) {
            if (rsp[i - 1] == 0x00) { i--; continue; }
            if (rsp[i - 1] == PAD_BYTE) { ok = true; i--; }
            break;
        }
        if (!ok) return -1;
        rsp[i] = sw[0];
        rsp[i + 1] = sw[1];
        *rsp_len = i + 2;
    } else {
        rsp[0] = sw[0];
        rsp[1] = sw[1];
        *rsp_len = 2;
    }
    if (s->count_every_cmd || plain_cmd_len > 0) counter_inc(s->counter);
    secure_zero(rmac, sizeof rmac);
    secure_zero(icv, sizeof icv);
    return 0;
}

void scp03_close(scp03_t *s)
{
    bool mode = s->count_every_cmd;
    secure_zero(s, sizeof *s);
    s->count_every_cmd = mode;
}
