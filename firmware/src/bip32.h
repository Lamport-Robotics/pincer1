/* Pincer1 - BIP32 private child key derivation on secp256k1 (own implementation over hmac_sha512 +
 * secp256k1.h). PURE LOGIC. Paths used by the controller:
 *   EVM      m/44'/60'/0'/0/0    (BIP44, coin type 60)
 *   Bitcoin  m/84'/0'/0'/0/0     (BIP84 native segwit, account m/84'/0'/0' kept as xpub for change detection) */
#ifndef PINCER_BIP32_H
#define PINCER_BIP32_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BIP32_HARDENED 0x80000000u

typedef struct {
    uint8_t key[32];   /* private key */
    uint8_t chain[32]; /* chain code */
} bip32_node_t;

typedef struct {
    uint8_t pub[33];   /* compressed public key */
    uint8_t chain[32];
} bip32_xpub_t;

/* Master node from a BIP39 seed (64 bytes): HMAC-SHA512("Bitcoin seed", seed). False for an invalid key. */
bool bip32_master(const uint8_t seed[64], bip32_node_t *m);
/* One CKDpriv step; `index` carries BIP32_HARDENED for hardened children. False on the (2^-127) invalid case. */
bool bip32_ckd_priv(const bip32_node_t *parent, uint32_t index, bip32_node_t *child);
/* Full path from the seed. */
bool bip32_derive(const uint8_t seed[64], const uint32_t *path, size_t depth, bip32_node_t *out);
/* Public part of a private node. */
bool bip32_xpub(const bip32_node_t *node, bip32_xpub_t *out);
/* One CKDpub step (non-hardened index only). */
bool bip32_ckd_pub(const bip32_xpub_t *parent, uint32_t index, bip32_xpub_t *child);

extern const uint32_t BIP32_PATH_EVM[5];      /* 44' 60' 0' 0 0 */
extern const uint32_t BIP32_PATH_BTC[5];      /* 84' 0' 0' 0 0 */
extern const uint32_t BIP32_PATH_BTC_ACCT[3]; /* 84' 0' 0' */
#endif
