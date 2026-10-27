/* Pincer1 - software ed25519 signer (dev boards without the SE050). Uses orlp/ed25519 (zlib).
 * The seeds live in RAM and in the plain flash record (signer_blob): this mode is NOT secure and
 * `hello` reports se050:false so the daemon refuses production use. The PIN is a SHA-256 of the code
 * compared in software; the attempt counter is the mirror in consent.c (no chip to enforce it). */
#include <string.h>

#include "ed25519.h"
#include "secp256k1.h"
#include "sha256.h"
#include "signer.h"
#include "util.h"

static uint8_t g_seed[SIGNER_SLOTS][32];
static uint8_t g_pub[SIGNER_SLOTS][32];
static uint8_t g_priv[SIGNER_SLOTS][64];
static bool g_present[SIGNER_SLOTS];
static uint8_t g_pin_hash[32];
static bool g_pin_set;
static bool g_pin_session;
static uint8_t g_duress_hash[32], g_duress_salt[16];
static bool g_duress_set;
static bool g_duress_session; /* a duress rhythm opened the session: only the decoy slot may sign */
static uint8_t g_secp[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32];
static uint8_t g_secp_pub[SIGNER_SLOTS][SIGNER_SECP_CHAINS][64];
static bool g_secp_present[SIGNER_SLOTS][SIGNER_SECP_CHAINS];

static void expand(uint8_t slot)
{
    ed25519_create_keypair(g_pub[slot], g_priv[slot], g_seed[slot]);
    g_present[slot] = true;
}

static void pin_hash(const uint8_t code[SIGNER_PIN_LEN], uint8_t out[32])
{
    static const uint8_t tag[] = "pincer1-soft-pin";
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, tag, sizeof tag);
    sha256_update(&c, code, SIGNER_PIN_LEN);
    sha256_final(&c, out);
}

static void duress_hash(const uint8_t code[SIGNER_PIN_LEN], const uint8_t salt[16], uint8_t out[32])
{
    static const uint8_t tag[] = "pincer1-soft-duress";
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, tag, sizeof tag);
    sha256_update(&c, salt, 16);
    sha256_update(&c, code, SIGNER_PIN_LEN);
    sha256_final(&c, out);
}

/* The chip's rule, mirrored: a real session signs for both slots, a duress session only for the decoy. */
static bool may_sign(uint8_t slot)
{
    if (!g_pin_set) return true; /* init flow: nothing to gate yet */
    if (g_pin_session) return true;
    return g_duress_session && slot == 1;
}

int signer_init(void) { return 0; }
bool signer_is_hardware(void) { return false; }
bool signer_policy_selftest(void) { return true; } /* no chip, no policy to prove */
bool signer_keys_from_otp(void) { return false; }

int signer_import(uint8_t slot, const uint8_t priv[32], uint8_t pubkey[32])
{
    if (slot >= SIGNER_SLOTS) return -1;
    memcpy(g_seed[slot], priv, 32);
    expand(slot);
    memcpy(pubkey, g_pub[slot], 32);
    return 0;
}

int signer_pubkey(uint8_t slot, uint8_t pubkey[32])
{
    if (slot >= SIGNER_SLOTS || !g_present[slot]) return -1;
    memcpy(pubkey, g_pub[slot], 32);
    return 0;
}

int signer_sign(uint8_t slot, const uint8_t *msg, size_t len, uint8_t sig[64])
{
    if (slot >= SIGNER_SLOTS || !g_present[slot]) return -1;
    if (!may_sign(slot)) return -1; /* same contract as the chip: no session, no signature */
    ed25519_sign(sig, msg, len, g_pub[slot], g_priv[slot]);
    return 0;
}

bool signer_slot_present(uint8_t slot) { return slot < SIGNER_SLOTS && g_present[slot]; }

int signer_import_secp256k1(uint8_t slot, uint8_t chain, const uint8_t priv[32], uint8_t pub[64])
{
    if (slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS) return -1;
    if (!secp256k1_pubkey(priv, g_secp_pub[slot][chain])) return -1;
    memcpy(g_secp[slot][chain], priv, 32);
    g_secp_present[slot][chain] = true;
    memcpy(pub, g_secp_pub[slot][chain], 64);
    return 0;
}

int signer_pubkey_secp256k1(uint8_t slot, uint8_t chain, uint8_t pub[64])
{
    if (slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS || !g_secp_present[slot][chain]) return -1;
    memcpy(pub, g_secp_pub[slot][chain], 64);
    return 0;
}

bool signer_secp256k1_present(uint8_t slot, uint8_t chain)
{
    return slot < SIGNER_SLOTS && chain < SIGNER_SECP_CHAINS && g_secp_present[slot][chain];
}

int signer_sign_secp256k1(uint8_t slot, uint8_t chain, const uint8_t digest[32], uint8_t sig65[65])
{
    uint8_t recid;
    if (slot >= SIGNER_SLOTS || chain >= SIGNER_SECP_CHAINS || !g_secp_present[slot][chain]) return -1;
    if (!may_sign(slot)) return -1;
    if (!secp256k1_sign(g_secp[slot][chain], digest, sig65, &recid)) return -1;
    if (!secp256k1_verify(g_secp_pub[slot][chain], digest, sig65)) return -1;
    sig65[64] = recid;
    return 0;
}

int signer_pin_set(const uint8_t code[SIGNER_PIN_LEN])
{
    pin_hash(code, g_pin_hash);
    g_pin_set = true;
    g_pin_session = false;
    return 0;
}

int signer_pin_verify(const uint8_t code[SIGNER_PIN_LEN])
{
    uint8_t h[32];
    int rc;
    if (!g_pin_set) return -1;
    pin_hash(code, h);
    rc = ct_memcmp(h, g_pin_hash, 32) == 0 ? 0 : 1;
    secure_zero(h, sizeof h);
    g_pin_session = rc == 0;
    g_duress_session = false;
    return rc;
}

int signer_duress_pin_set(const uint8_t code[SIGNER_PIN_LEN], const uint8_t salt[16])
{
    if (!g_pin_set) return -1;
    memcpy(g_duress_salt, salt, 16);
    duress_hash(code, g_duress_salt, g_duress_hash);
    g_duress_set = true;
    return 0;
}

bool signer_duress_pin_present(void) { return g_duress_set; }

int signer_pin_verify2(const uint8_t code[SIGNER_PIN_LEN])
{
    uint8_t h[32];
    int d, r;
    if (!g_pin_set) return -1;
    /* both hashes, both compares, every time: no early exit */
    if (g_duress_set) duress_hash(code, g_duress_salt, h); else memset(h, 0, sizeof h);
    d = g_duress_set && ct_memcmp(h, g_duress_hash, 32) == 0;
    pin_hash(code, h);
    r = ct_memcmp(h, g_pin_hash, 32) == 0;
    secure_zero(h, sizeof h);
    g_pin_session = false;
    g_duress_session = false;
    if (d) { g_duress_session = true; return SIGNER_VERIFY_DURESS; }
    if (r) { g_pin_session = true; return SIGNER_VERIFY_REAL; }
    return SIGNER_VERIFY_WRONG;
}

void signer_pin_close(void) { g_pin_session = false; g_duress_session = false; }
bool signer_pin_present(void) { return g_pin_set; }

int signer_wipe(void)
{
    secure_zero(g_seed, sizeof g_seed);
    secure_zero(g_priv, sizeof g_priv);
    secure_zero(g_pub, sizeof g_pub);
    secure_zero(g_pin_hash, sizeof g_pin_hash);
    secure_zero(g_duress_hash, sizeof g_duress_hash);
    secure_zero(g_duress_salt, sizeof g_duress_salt);
    g_duress_set = false;
    g_duress_session = false;
    secure_zero(g_secp, sizeof g_secp);
    secure_zero(g_secp_pub, sizeof g_secp_pub);
    memset(g_secp_present, 0, sizeof g_secp_present);
    memset(g_present, 0, sizeof g_present);
    g_pin_set = false;
    g_pin_session = false;
    return 0;
}

void signer_soft_export(uint8_t blob[SIGNER_SLOTS][32], uint8_t pin_hash_out[32])
{
    memcpy(blob, g_seed, sizeof g_seed);
    if (g_pin_set) memcpy(pin_hash_out, g_pin_hash, 32);
    else memset(pin_hash_out, 0, 32);
}

void signer_soft_load(const uint8_t blob[SIGNER_SLOTS][32], const uint8_t pin_hash_in[32])
{
    static const uint8_t zero[32] = {0};
    uint8_t s;
    for (s = 0; s < SIGNER_SLOTS; s++) {
        if (memcmp(blob[s], zero, 32) == 0) continue;
        memcpy(g_seed[s], blob[s], 32);
        expand(s);
    }
    if (memcmp(pin_hash_in, zero, 32) != 0) {
        memcpy(g_pin_hash, pin_hash_in, 32);
        g_pin_set = true;
    }
}

void signer_soft_export_secp(uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32])
{
    memcpy(blob, g_secp, sizeof g_secp);
}

void signer_soft_load_secp(const uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32])
{
    static const uint8_t zero[32] = {0};
    uint8_t s, ch;
    for (s = 0; s < SIGNER_SLOTS; s++)
        for (ch = 0; ch < SIGNER_SECP_CHAINS; ch++) {
            if (memcmp(blob[s][ch], zero, 32) == 0) continue;
            if (!secp256k1_pubkey(blob[s][ch], g_secp_pub[s][ch])) continue;
            memcpy(g_secp[s][ch], blob[s][ch], 32);
            g_secp_present[s][ch] = true;
        }
}

void signer_soft_export_duress(uint8_t hash[32], uint8_t salt[16])
{
    if (g_duress_set) { memcpy(hash, g_duress_hash, 32); memcpy(salt, g_duress_salt, 16); }
    else { memset(hash, 0, 32); memset(salt, 0, 16); }
}

void signer_soft_load_duress(const uint8_t hash[32], const uint8_t salt[16])
{
    static const uint8_t zero[32] = {0};
    if (memcmp(hash, zero, 32) == 0) return;
    memcpy(g_duress_hash, hash, 32);
    memcpy(g_duress_salt, salt, 16);
    g_duress_set = true;
}
