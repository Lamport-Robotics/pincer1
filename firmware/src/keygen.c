#include "keygen.h"

#include <string.h>

#include "bip32.h"
#include "bip39_words.h"
#include "hmac_sha512.h"
#include "sha256.h"
#include "util.h"

/* 256 bits entropy + 8 bits checksum = 264 bits = 24 * 11. */
int keygen_mnemonic_from_entropy(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *out, size_t cap)
{
    uint8_t bits[33];
    uint8_t cs[32];
    size_t o = 0;
    int w;
    memcpy(bits, entropy, 32);
    sha256(entropy, 32, cs);
    bits[32] = cs[0];
    for (w = 0; w < KEYGEN_WORDS; w++) {
        unsigned idx = 0;
        int b;
        const char *word;
        size_t wl;
        for (b = 0; b < 11; b++) {
            int bit = w * 11 + b;
            idx = (idx << 1) | ((bits[bit / 8] >> (7 - (bit % 8))) & 1u);
        }
        word = BIP39_WORDS[idx];
        wl = strlen(word);
        if (o + wl + 2 > cap) {
            secure_zero(bits, sizeof bits);
            return -1;
        }
        if (w) out[o++] = ' ';
        memcpy(out + o, word, wl);
        o += wl;
    }
    out[o] = 0;
    secure_zero(bits, sizeof bits);
    secure_zero(cs, sizeof cs);
    return 0;
}

static int word_index(const char *w, size_t n)
{
    int lo = 0, hi = 2047;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strncmp(w, BIP39_WORDS[mid], n);
        if (c == 0 && BIP39_WORDS[mid][n] != 0) c = -1;
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

bool keygen_mnemonic_valid(const char *mnemonic)
{
    uint8_t bits[33] = {0};
    uint8_t cs[32];
    int nw = 0;
    const char *p = mnemonic;
    if (!p) return false;
    while (*p) {
        const char *s;
        int idx, b;
        while (*p == ' ') p++;
        if (!*p) break;
        s = p;
        while (*p && *p != ' ') p++;
        if (nw >= KEYGEN_WORDS) return false;
        idx = word_index(s, (size_t)(p - s));
        if (idx < 0) return false;
        for (b = 0; b < 11; b++) {
            int bit = nw * 11 + b;
            if ((idx >> (10 - b)) & 1) bits[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
        }
        nw++;
    }
    if (nw != KEYGEN_WORDS) return false;
    sha256(bits, 32, cs);
    return cs[0] == bits[32];
}

void keygen_seed_from_mnemonic(const char *mnemonic, const char *passphrase, uint8_t seed[64])
{
    uint8_t salt[8 + 64];
    size_t pl = passphrase ? strlen(passphrase) : 0;
    if (pl > 64) pl = 64;
    memcpy(salt, "mnemonic", 8);
    if (pl) memcpy(salt + 8, passphrase, pl);
    pbkdf2_hmac_sha512((const uint8_t *)mnemonic, strlen(mnemonic), salt, 8 + pl, 2048, seed, 64);
    secure_zero(salt, sizeof salt);
}

void keygen_slip10_ed25519(const uint8_t seed[64], const uint32_t *path, size_t depth, uint8_t priv[32],
                           uint8_t chain[32])
{
    uint8_t I[64];
    size_t d;
    hmac_sha512((const uint8_t *)"ed25519 seed", 12, seed, 64, I);
    for (d = 0; d < depth; d++) {
        uint8_t data[1 + 32 + 4];
        uint32_t idx = path[d] | 0x80000000u;
        data[0] = 0;
        memcpy(data + 1, I, 32);
        data[33] = (uint8_t)(idx >> 24); data[34] = (uint8_t)(idx >> 16);
        data[35] = (uint8_t)(idx >> 8); data[36] = (uint8_t)idx;
        hmac_sha512(I + 32, 32, data, sizeof data, I);
        secure_zero(data, sizeof data);
    }
    memcpy(priv, I, 32);
    if (chain) memcpy(chain, I + 32, 32);
    secure_zero(I, sizeof I);
}

void keygen_slip10_solana(const uint8_t seed[64], uint8_t priv[32])
{
    static const uint32_t path[4] = {44, 501, 0, 0};
    keygen_slip10_ed25519(seed, path, 4, priv, NULL);
}

int keygen_derive(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *mnemonic, size_t cap, uint8_t priv[32])
{
    uint8_t seed[64];
    if (keygen_mnemonic_from_entropy(entropy, mnemonic, cap) != 0) return -1;
    keygen_seed_from_mnemonic(mnemonic, "", seed);
    keygen_slip10_solana(seed, priv);
    secure_zero(seed, sizeof seed);
    return 0;
}

bool keygen_keys_from_seed(const uint8_t seed[64], keygen_keys_t *k)
{
    bip32_node_t n;
    bool ok;
    memset(k, 0, sizeof *k);
    keygen_slip10_solana(seed, k->sol_priv);
    ok = bip32_derive(seed, BIP32_PATH_EVM, 5, &n);
    if (ok) memcpy(k->evm_priv, n.key, 32);
    ok = ok && bip32_derive(seed, BIP32_PATH_BTC_ACCT, 3, &n) && bip32_xpub(&n, &k->btc_account);
    ok = ok && bip32_ckd_priv(&n, 0, &n) && bip32_ckd_priv(&n, 0, &n);
    if (ok) memcpy(k->btc_priv, n.key, 32);
    secure_zero(&n, sizeof n);
    if (!ok) secure_zero(k, sizeof *k);
    return ok;
}

int keygen_derive_all(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *mnemonic, size_t cap, keygen_keys_t *k)
{
    uint8_t seed[64];
    bool ok;
    if (keygen_mnemonic_from_entropy(entropy, mnemonic, cap) != 0) return -1;
    keygen_seed_from_mnemonic(mnemonic, "", seed);
    ok = keygen_keys_from_seed(seed, k);
    secure_zero(seed, sizeof seed);
    return ok ? 0 : -1;
}
