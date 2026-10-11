// Find My offline-finding keys and advertisement, OpenHaystack scheme.
//
// Everything in this header is pure (no BLE, no mbedTLS) and host-tested in test/test_findmy.
// The one piece that needs elliptic-curve arithmetic, scalar → public key on P-224, lives in
// findmy_p224.cpp and uses mbedTLS (the ESP32 Arduino core ships it; on the host it is compiled
// only when -DFINDMY_HOST_MBEDTLS is given and mbedTLS headers are on the include path).
//
// Key derivation (documented here, mirrored byte for byte by tools/findmy_keys.py):
//
//   seed            32 random bytes from tools/provision.py (the Find My master seed)
//   d0              = X963KDF-SHA256(seed, "pincer1-findmy-d0", 32)  → int mod (n−1), + 1   (28-byte scalar)
//   SK_0            = X963KDF-SHA256(seed, "pincer1-findmy-sk0", 32)
//   SK_i            = X963KDF-SHA256(SK_{i−1}, "update", 32)
//   (u_i, v_i)      = X963KDF-SHA256(SK_i, "diversify", 72); u_i = int(bytes 0..35) mod (n−1) + 1,
//                                                            v_i = int(bytes 36..71) mod (n−1) + 1
//   d_i             = (d0 · u_i + v_i) mod n                          (the rotating private key)
//   P_i             = d_i · G on secp224r1; the advertised key is x(P_i), 28 bytes big-endian
//   hashed adv key  = SHA-256(x(P_i)); base64 of it is what the fetcher asks Apple for
//
// where n is the order of secp224r1 and X963KDF is ANSI X9.63 with SHA-256
// (out = SHA256(Z ‖ counter_be32 ‖ info) for counter = 1, 2, …, truncated to the requested length).
//
// The SK_i / (u,v) / d_i part is the rotation scheme of Apple's Find My accessory protocol as
// published by Heinrich, Stute, Kornhuber and Hollick ("Who Can Find My Devices?", PETS 2021) and
// implemented by the OpenHaystack and FindMy.py projects. The first two lines (how d0 and SK_0 come
// from one seed) are ours: OpenHaystack generates them randomly, we derive them so one 32-byte
// secret is enough to recreate every key. Nothing here has been checked against an Apple device;
// it was checked for determinism between this C code and the Python helper only, which is what the
// fetcher needs (it just takes the list of private keys). See README "Find My".
//
// Advertisement (31 bytes, non-connectable undirected, random static address):
//
//   1e ff 4c 00 12 19 <status> <pub[6..27]> <pub[0] >> 6> 00
//   address = (pub[0] | 0xC0) ‖ pub[1..5]          (big-endian / printed order)
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FINDMY_SEED_LEN   32
#define FINDMY_KEY_LEN    28    // P-224 scalar and x-coordinate
#define FINDMY_SK_LEN     32
#define FINDMY_ADV_LEN    31
#define FINDMY_ROTATE_SEC 900   // 15 min, like Apple's primary key

// status byte, best effort from the reverse-engineered AirTag format: bits 7..6 battery
#define FINDMY_STATUS_BATT_FULL     0x00
#define FINDMY_STATUS_BATT_MEDIUM   0x40
#define FINDMY_STATUS_BATT_LOW      0x80
#define FINDMY_STATUS_BATT_CRITICAL 0xC0

typedef struct {
    uint8_t  d0[FINDMY_KEY_LEN];   // master private scalar, big-endian
    uint8_t  sk[FINDMY_SK_LEN];    // SK_index
    uint32_t index;
} findmy_state_t;

// ANSI X9.63 KDF with SHA-256.
void findmy_x963_kdf(const uint8_t *z, size_t zlen, const char *info, uint8_t *out, size_t outlen);

// State at index 0 from the seed.
void findmy_init(findmy_state_t *s, const uint8_t seed[FINDMY_SEED_LEN]);
// State at an arbitrary index from the seed (index KDF steps; ~index × 1 SHA-256).
void findmy_init_at(findmy_state_t *s, const uint8_t seed[FINDMY_SEED_LEN], uint32_t index);
// SK_{i+1}, index + 1.
void findmy_advance(findmy_state_t *s);
// d_index, 28 bytes big-endian.
void findmy_private_key(const findmy_state_t *s, uint8_t d[FINDMY_KEY_LEN]);

// Advertisement payload for a 28-byte public key x-coordinate. Returns FINDMY_ADV_LEN.
size_t findmy_adv_payload(const uint8_t pub[FINDMY_KEY_LEN], uint8_t status, uint8_t out[FINDMY_ADV_LEN]);
// The random static address that goes with it, printed order (addr[0] is the most significant byte).
void findmy_adv_address(const uint8_t pub[FINDMY_KEY_LEN], uint8_t addr[6]);
// SHA-256 of the public key: the identifier the fetcher queries.
void findmy_hashed_adv_key(const uint8_t pub[FINDMY_KEY_LEN], uint8_t out[32]);
// Battery status byte from a cell voltage.
uint8_t findmy_status_for_mv(uint16_t mv);

// Public key x-coordinate for a scalar. Implemented in findmy_p224.cpp with mbedTLS.
// Returns 1 on success, 0 on failure (scalar out of range or mbedTLS error).
int findmy_public_key(const uint8_t d[FINDMY_KEY_LEN], uint8_t pub_x[FINDMY_KEY_LEN]);

// Exposed for tests: 224-bit helpers on big-endian byte strings.
// out = in (len bytes, big-endian) mod m.
void findmy_mod_be(const uint8_t *in, size_t len, const uint8_t m[28], uint8_t out[28]);
// out = (a·b + c) mod m, all 28-byte big-endian, a,b,c < m.
void findmy_muladd_mod(const uint8_t a[28], const uint8_t b[28], const uint8_t c[28], const uint8_t m[28], uint8_t out[28]);
// secp224r1 group order n.
extern const uint8_t FINDMY_P224_N[28];

#ifdef __cplusplus
}
#endif
