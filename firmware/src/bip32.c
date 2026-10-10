#include "bip32.h"

#include <string.h>

#include "hmac_sha512.h"
#include "secp256k1.h"
#include "util.h"

const uint32_t BIP32_PATH_EVM[5] = {44u | BIP32_HARDENED, 60u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u, 0u};
const uint32_t BIP32_PATH_BTC[5] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u, 0u};
const uint32_t BIP32_PATH_BTC_ACCT[3] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED};

bool bip32_master(const uint8_t seed[64], bip32_node_t *m)
{
    uint8_t I[64];
    bool ok;
    hmac_sha512((const uint8_t *)"Bitcoin seed", 12, seed, 64, I);
    memcpy(m->key, I, 32);
    memcpy(m->chain, I + 32, 32);
    ok = secp256k1_scalar_valid(m->key);
    secure_zero(I, sizeof I);
    if (!ok) secure_zero(m, sizeof *m);
    return ok;
}

bool bip32_ckd_priv(const bip32_node_t *parent, uint32_t index, bip32_node_t *child)
{
    uint8_t data[1 + 32 + 4], I[64], pub[64];
    size_t dl;
    bool ok;
    if (index & BIP32_HARDENED) {
        data[0] = 0;
        memcpy(data + 1, parent->key, 32);
        dl = 33;
    } else {
        if (!secp256k1_pubkey(parent->key, pub)) return false;
        secp256k1_compress(pub, data);
        dl = 33;
    }
    data[dl++] = (uint8_t)(index >> 24);
    data[dl++] = (uint8_t)(index >> 16);
    data[dl++] = (uint8_t)(index >> 8);
    data[dl++] = (uint8_t)index;
    hmac_sha512(parent->chain, 32, data, dl, I);
    ok = secp256k1_scalar_add(parent->key, I, child->key);
    if (ok) memcpy(child->chain, I + 32, 32);
    else secure_zero(child, sizeof *child);
    secure_zero(data, sizeof data);
    secure_zero(I, sizeof I);
    secure_zero(pub, sizeof pub);
    return ok;
}

bool bip32_derive(const uint8_t seed[64], const uint32_t *path, size_t depth, bip32_node_t *out)
{
    bip32_node_t cur, next;
    size_t i;
    bool ok = bip32_master(seed, &cur);
    for (i = 0; ok && i < depth; i++) {
        ok = bip32_ckd_priv(&cur, path[i], &next);
        cur = next;
    }
    if (ok) *out = cur;
    else secure_zero(out, sizeof *out);
    secure_zero(&cur, sizeof cur);
    secure_zero(&next, sizeof next);
    return ok;
}

bool bip32_xpub(const bip32_node_t *node, bip32_xpub_t *out)
{
    uint8_t pub[64];
    if (!secp256k1_pubkey(node->key, pub)) return false;
    secp256k1_compress(pub, out->pub);
    memcpy(out->chain, node->chain, 32);
    secure_zero(pub, sizeof pub);
    return true;
}

bool bip32_ckd_pub(const bip32_xpub_t *parent, uint32_t index, bip32_xpub_t *child)
{
    uint8_t data[33 + 4], I[64], pub[64], cpub[64];
    bool ok;
    if (index & BIP32_HARDENED) return false;
    memcpy(data, parent->pub, 33);
    data[33] = (uint8_t)(index >> 24);
    data[34] = (uint8_t)(index >> 16);
    data[35] = (uint8_t)(index >> 8);
    data[36] = (uint8_t)index;
    hmac_sha512(parent->chain, 32, data, sizeof data, I);
    ok = secp256k1_decompress(parent->pub, pub) && secp256k1_pubkey_add_scalar(pub, I, cpub);
    if (ok) {
        secp256k1_compress(cpub, child->pub);
        memcpy(child->chain, I + 32, 32);
    }
    secure_zero(I, sizeof I);
    return ok;
}
