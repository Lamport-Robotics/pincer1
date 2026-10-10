/* Pincer1 - key generation. PURE LOGIC: BIP-39 (24 words) + SLIP-0010 ed25519 m/44'/501'/0'/0' (Solana)
 * + BIP32 secp256k1 m/44'/60'/0'/0/0 (EVM) and m/84'/0'/0'/0/0 (Bitcoin native segwit). */
#ifndef PINCER_KEYGEN_H
#define PINCER_KEYGEN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bip32.h"

#define KEYGEN_ENTROPY_LEN 32
#define KEYGEN_WORDS 24
#define KEYGEN_MNEMONIC_CAP (KEYGEN_WORDS * 9 + 1) /* longest word is 8 letters + separators + NUL */

/* 32 bytes of entropy -> 24 words separated by single spaces, NUL-terminated. Returns 0 or -1 (cap). */
int keygen_mnemonic_from_entropy(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *out, size_t cap);
/* Returns true when every word is in the list and the 8-bit checksum word matches. */
bool keygen_mnemonic_valid(const char *mnemonic);
/* BIP-39 seed: PBKDF2-HMAC-SHA512(mnemonic, "mnemonic" || passphrase, 2048) -> 64 bytes. */
void keygen_seed_from_mnemonic(const char *mnemonic, const char *passphrase, uint8_t seed[64]);
/* SLIP-0010 ed25519 derivation of m/44'/501'/0'/0' from a BIP-39 seed. */
void keygen_slip10_solana(const uint8_t seed[64], uint8_t priv[32]);
/* Generic SLIP-0010 hardened path (indexes without the hardened bit). */
void keygen_slip10_ed25519(const uint8_t seed[64], const uint32_t *path, size_t depth, uint8_t priv[32],
                           uint8_t chain[32]);
/* All-in-one: entropy -> mnemonic + 32-byte ed25519 private key; every intermediate is zeroised. */
int keygen_derive(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *mnemonic, size_t cap, uint8_t priv[32]);

/* Every key the controller holds, derived from one seed. */
typedef struct {
    uint8_t sol_priv[32];     /* SLIP-0010 ed25519 m/44'/501'/0'/0' */
    uint8_t evm_priv[32];     /* BIP32 m/44'/60'/0'/0/0 */
    uint8_t btc_priv[32];     /* BIP32 m/84'/0'/0'/0/0 */
    bip32_xpub_t btc_account; /* public m/84'/0'/0' (not secret: recognises change outputs) */
} keygen_keys_t;
/* False only in the astronomically unlikely invalid-key case; the caller zeroises `k`. */
bool keygen_keys_from_seed(const uint8_t seed[64], keygen_keys_t *k);
/* entropy -> mnemonic + all keys; every intermediate zeroised. 0 or -1. */
int keygen_derive_all(const uint8_t entropy[KEYGEN_ENTROPY_LEN], char *mnemonic, size_t cap, keygen_keys_t *k);
#endif
