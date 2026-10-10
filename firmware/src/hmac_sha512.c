#include "hmac_sha512.h"

#include <string.h>

#include "sha512.h"
#include "util.h"

static void hmac_start(sha512_context *inner, sha512_context *outer, const uint8_t *key, size_t key_len)
{
    uint8_t k[128], pad[128];
    size_t i;
    memset(k, 0, sizeof k);
    if (key_len > 128) {
        sha512(key, key_len, k);
    } else {
        memcpy(k, key, key_len);
    }
    for (i = 0; i < 128; i++) pad[i] = k[i] ^ 0x36;
    sha512_init(inner);
    sha512_update(inner, pad, 128);
    for (i = 0; i < 128; i++) pad[i] = k[i] ^ 0x5c;
    sha512_init(outer);
    sha512_update(outer, pad, 128);
    secure_zero(k, sizeof k);
    secure_zero(pad, sizeof pad);
}

static void hmac_finish(sha512_context *inner, sha512_context *outer, uint8_t out[64])
{
    uint8_t ih[64];
    sha512_final(inner, ih);
    sha512_update(outer, ih, 64);
    sha512_final(outer, out);
    secure_zero(ih, sizeof ih);
}

void hmac_sha512_2(const uint8_t *key, size_t key_len, const uint8_t *m1, size_t l1, const uint8_t *m2, size_t l2,
                   uint8_t out[64])
{
    sha512_context in, outc;
    hmac_start(&in, &outc, key, key_len);
    if (l1) sha512_update(&in, m1, l1);
    if (l2) sha512_update(&in, m2, l2);
    hmac_finish(&in, &outc, out);
    secure_zero(&in, sizeof in);
    secure_zero(&outc, sizeof outc);
}

void hmac_sha512(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[64])
{
    hmac_sha512_2(key, key_len, msg, msg_len, NULL, 0, out);
}

void pbkdf2_hmac_sha512(const uint8_t *pw, size_t pw_len, const uint8_t *salt, size_t salt_len, uint32_t iters,
                        uint8_t *out, size_t out_len)
{
    sha512_context in0, out0;
    uint32_t blk = 1;
    hmac_start(&in0, &out0, pw, pw_len);
    while (out_len) {
        uint8_t u[64], t[64], be[4];
        sha512_context in, outc;
        size_t take = out_len < 64 ? out_len : 64;
        uint32_t it;
        int i;
        be[0] = (uint8_t)(blk >> 24); be[1] = (uint8_t)(blk >> 16); be[2] = (uint8_t)(blk >> 8); be[3] = (uint8_t)blk;
        in = in0; outc = out0;
        sha512_update(&in, salt, salt_len);
        sha512_update(&in, be, 4);
        hmac_finish(&in, &outc, u);
        memcpy(t, u, 64);
        for (it = 1; it < iters; it++) {
            in = in0; outc = out0;
            sha512_update(&in, u, 64);
            hmac_finish(&in, &outc, u);
            for (i = 0; i < 64; i++) t[i] ^= u[i];
        }
        memcpy(out, t, take);
        out += take;
        out_len -= take;
        blk++;
        secure_zero(u, sizeof u);
        secure_zero(t, sizeof t);
    }
    secure_zero(&in0, sizeof in0);
    secure_zero(&out0, sizeof out0);
}
