/* Pincer1 - secp256k1 for EVM and Bitcoin on top of the vendored micro-ecc (BSD-2, Kenneth MacKay).
 * PURE LOGIC. micro-ecc gives us the field/point arithmetic (uECC_vli.h API); ECDSA signing, RFC 6979
 * nonces, low-s normalisation, the recovery id and public-key recovery are implemented here because
 * micro-ecc's own uECC_sign() exposes none of them.
 *
 * Byte conventions: private keys and scalars are 32-byte big-endian; public keys are 64 bytes X||Y
 * (uncompressed without the 0x04 prefix, micro-ecc's native form); signatures are 64 bytes r||s.
 *
 * Side channels: micro-ecc's modular inverse is not constant time and its ladder is only blinded when
 * an RNG is set. In production the private keys live in the SE050 and this code signs nothing; it
 * derives keys once at `init` (and the software signer, dev boards only, signs with it). */
#ifndef PINCER_SECP256K1_H
#define PINCER_SECP256K1_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SECP256K1_DER_MAX 72 /* 0x30 len 0x02 len r(<=33) 0x02 len s(<=33) */

/* 0 < k < n */
bool secp256k1_scalar_valid(const uint8_t k[32]);
/* out = (a + b) mod n; false when either input is >= n or the sum is 0 (BIP32: "proceed with the next index"). */
bool secp256k1_scalar_add(const uint8_t a[32], const uint8_t b[32], uint8_t out[32]);
/* Public key of a private key. False when priv is not a valid scalar. */
bool secp256k1_pubkey(const uint8_t priv[32], uint8_t pub[64]);
/* out = pub + k*G (BIP32 public child derivation). False on invalid inputs or the point at infinity. */
bool secp256k1_pubkey_add_scalar(const uint8_t pub[64], const uint8_t k[32], uint8_t out[64]);
bool secp256k1_pubkey_valid(const uint8_t pub[64]);
void secp256k1_compress(const uint8_t pub[64], uint8_t out[33]);
bool secp256k1_decompress(const uint8_t in[33], uint8_t pub[64]);

/* Deterministic ECDSA (RFC 6979, HMAC-SHA256) over a 32-byte digest. Output is low-s; `recid` is the
 * recovery id 0..3 (bit 0 = R.y parity after low-s normalisation, bit 1 = R.x overflowed n). */
bool secp256k1_sign(const uint8_t priv[32], const uint8_t digest[32], uint8_t sig[64], uint8_t *recid);
bool secp256k1_verify(const uint8_t pub[64], const uint8_t digest[32], const uint8_t sig[64]);
/* Recovers the public key of a signature. False when (digest, sig, recid) do not describe a point. */
bool secp256k1_recover(const uint8_t digest[32], const uint8_t sig[64], uint8_t recid, uint8_t pub[64]);
/* s > n/2 -> s = n - s. Returns true when it changed the signature (the recovery id's bit 0 flips). */
bool secp256k1_normalize_s(uint8_t sig[64]);
bool secp256k1_is_low_s(const uint8_t sig[64]);
/* Finds the recovery id of an externally produced signature (the SE050's) against a known public key:
 * tries 0..3 and returns the one whose recovered key matches; -1 when none does (the signature is bogus). */
int secp256k1_find_recid(const uint8_t pub[64], const uint8_t digest[32], const uint8_t sig[64]);

/* DER (strict, as Bitcoin requires): r||s <-> 0x30 ... Returns the length, or 0 on a malformed input. */
size_t secp256k1_der_encode(const uint8_t sig[64], uint8_t out[SECP256K1_DER_MAX]);
bool secp256k1_der_decode(const uint8_t *der, size_t len, uint8_t sig[64]);
#endif
