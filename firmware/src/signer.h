/* Pincer1 - signing back-end interface. Two key slots: 0 = real key, 1 = decoy (duress).
 *
 * PIN: an 8-byte code (touch_rhythm_encode) that the back-end verifies itself. With the SE050 it is a
 * UserID authentication object with a 5-attempt limit; the key objects carry a policy that allows
 * signing only inside a session authenticated with it, so signer_sign() works only between a
 * successful signer_pin_verify() and signer_pin_close(). */
#ifndef PINCER_SIGNER_H
#define PINCER_SIGNER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIGNER_SLOTS 2
#define SIGNER_PIN_LEN 8
#define SIGNER_SECP_CHAINS 2
#define SIGNER_CHAIN_EVM 0 /* m/44'/60'/0'/0/0 */
#define SIGNER_CHAIN_BTC 1 /* m/84'/0'/0'/0/0 */

int signer_init(void);                 /* 0 ok */
bool signer_is_hardware(void);         /* true = SE050, false = software fallback */
/* True when the SE050 link is protected by Platform SCP03 keys read from OTP (false: dev keys or soft). */
bool signer_keys_from_otp(void);
/* Imports a 32-byte ed25519 private key (SLIP-0010 output) into `slot` as a non-exportable key and
 * returns its public key. The caller zeroises `priv` afterwards; the implementation must not keep
 * a copy outside the secure element (software signer: see signer_soft.c caveats). */
int signer_import(uint8_t slot, const uint8_t priv[32], uint8_t pubkey[32]);
int signer_pubkey(uint8_t slot, uint8_t pubkey[32]);
int signer_sign(uint8_t slot, const uint8_t *msg, size_t len, uint8_t sig[64]);
bool signer_slot_present(uint8_t slot);

/* ---- secp256k1 (EVM + Bitcoin), one key per slot per chain, same PIN gating as the ed25519 key ----
 * Imports a 32-byte BIP32 private key as a non-exportable secp256k1 key pair; returns the 64-byte X||Y
 * public key. */
int signer_import_secp256k1(uint8_t slot, uint8_t chain, const uint8_t priv[32], uint8_t pub[64]);
int signer_pubkey_secp256k1(uint8_t slot, uint8_t chain, uint8_t pub[64]);
bool signer_secp256k1_present(uint8_t slot, uint8_t chain);
/* ECDSA over a 32-byte digest. sig65 = r||s||recid with low-s; recid in 0..3 (bit 0 = R.y parity). The
 * back-end verifies the signature against its own copy of the public key before returning it. */
int signer_sign_secp256k1(uint8_t slot, uint8_t chain, const uint8_t digest[32], uint8_t sig65[65]);

/* ---- PIN ---- */
int signer_pin_set(const uint8_t code[SIGNER_PIN_LEN]);    /* creates the PIN object; 0 ok */
/* 0 = verified (an authenticated session is now open), 1 = wrong code, -1 = link/chip error. */
int signer_pin_verify(const uint8_t code[SIGNER_PIN_LEN]);
/* ---- second (duress) rhythm: a UserID object for the decoy slot ----
 * Creates or replaces it (the caller already checked that it differs from the real code). `salt` (16 bytes,
 * from the RNG) is used by the software signer's hash; the SE050 ignores it. 0 ok. */
int signer_duress_pin_set(const uint8_t code[SIGNER_PIN_LEN], const uint8_t salt[16]);
bool signer_duress_pin_present(void);
#define SIGNER_VERIFY_WRONG 1
#define SIGNER_VERIFY_REAL 0
#define SIGNER_VERIFY_DURESS 2
/* Checks the code against BOTH objects, always in the same order and with the same number of rounds (the
 * duress object first, then a second round against the real object, or against the duress object again when
 * it already matched), so neither the bus traffic nor the timing says which one matched. SIGNER_VERIFY_REAL
 * leaves a real session open (as signer_pin_verify), SIGNER_VERIFY_DURESS leaves none and lets the decoy slot
 * sign (decoy keys are gated by the duress object), -1 = link/chip error. */
int signer_pin_verify2(const uint8_t code[SIGNER_PIN_LEN]);
void signer_pin_close(void);                               /* ends the authenticated session (lock) */
bool signer_pin_present(void);                             /* the back-end holds a PIN object */
/* Destroys both key objects and the PIN object. Irreversible; the 24 words are the only recovery. */
int signer_wipe(void);

/* Software signer only: persistence hooks (32 bytes per slot + PIN hash). No-ops with the SE050. */
void signer_soft_export(uint8_t blob[SIGNER_SLOTS][32], uint8_t pin_hash[32]);
void signer_soft_load(const uint8_t blob[SIGNER_SLOTS][32], const uint8_t pin_hash[32]);
void signer_soft_export_secp(uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32]);
void signer_soft_load_secp(const uint8_t blob[SIGNER_SLOTS][SIGNER_SECP_CHAINS][32]);
/* Software signer only: the duress rhythm's salted hash (zero = none). No-ops with the SE050. */
void signer_soft_export_duress(uint8_t hash[32], uint8_t salt[16]);
void signer_soft_load_duress(const uint8_t hash[32], const uint8_t salt[16]);
#endif
