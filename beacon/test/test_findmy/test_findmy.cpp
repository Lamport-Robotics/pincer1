// Find My tests: X9.63 KDF, 224-bit modular helpers, key schedule determinism against the vectors
// produced by tools/findmy_keys.py (which was itself cross-checked against the cryptography package),
// advertisement layout, and, only with -DFINDMY_HOST_MBEDTLS, the P-224 public key.
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "findmy.h"
#include "sha256.h"

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}

// seed 000102…1f, vectors from: python3 tools/findmy_keys.py --seed <seed> --count 3 --format jsonl --check
static const char *SEED_HEX = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
static const char *D0_HEX   = "ad9cdbe239d0b5a19d2061a6710c3027bb7d409952b71df7027f9448";
static const char *SK0_HEX  = "40c75f875e2feb0dd7c59c509aa6d39784201c009f645e70810cd551fd0f4244";
static const char *SK1_HEX  = "25559e98111d5900064ca1d925c05ff24610e974025c2590888444dcb3a53e3e";
static const char *KDF72_HEX = "46387fc98044749b6af547f5b047ebe2959d9e74f6b7e1f3aa496e0853eaa4de62d7a294c3972adc7fee8a1a2f0146ad937cf53141118dc18e27dea27f3650afe315214ceff994f3";
static const char *D_HEX[3] = {
    "9e4eb4982289d6fc2bb317b25b021815e9f09d48659a9eaca2ee8447",
    "3b6505b755e0e4d59bc49e02d8488d16b19dd99789287a671a569573",
    "6957e310545826fc58a1202bf86b48b0022130f343cf8a53901cfcad"};
static const char *X_HEX[3] = {
    "c80c1e298ec21830f3475831a995aaaf11f3004d218fa1bd2425fd96",
    "7ee0dfdac6b4842273dce51d7996ab2e3412cdf710d9c868bda408b4",
    "106f2fa98ccb20cf954c8c4d41c795a73e29bd05e2adaed986a4291c"};
static const char *H0_HEX = "4758c7b26da739cb2d9451f6472c1b764de7a94e032b1d0ef2e44da3818ec41d";

void test_sha256_known(void) {
    uint8_t h[32], want[32];
    sha256((const uint8_t *)"abc", 3, h);
    unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, h, 32);
    sha256((const uint8_t *)"", 0, h);
    unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, h, 32);
    // 56-byte message exercises the padding boundary
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256((const uint8_t *)m, 56, h);
    unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, h, 32);
}

void test_x963_kdf(void) {
    uint8_t seed[32], sk0[32], want[72], out[72];
    unhex(SEED_HEX, seed, 32);
    unhex(SK0_HEX, sk0, 32);
    findmy_x963_kdf(seed, 32, "pincer1-findmy-sk0", out, 32);
    TEST_ASSERT_EQUAL_MEMORY(sk0, out, 32);
    unhex(KDF72_HEX, want, 72);
    findmy_x963_kdf(sk0, 32, "diversify", out, 72);        // 3 SHA-256 blocks, truncated
    TEST_ASSERT_EQUAL_MEMORY(want, out, 72);
    unhex(SK1_HEX, want, 32);
    findmy_x963_kdf(sk0, 32, "update", out, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 32);
}

void test_mod_helpers(void) {
    uint8_t n[28], a[28], b[28], c[28], out[28], want[28];
    memcpy(n, FINDMY_P224_N, 28);
    // n mod n = 0 ; (n+1) mod n = 1 ; big 36-byte value
    findmy_mod_be(n, 28, n, out);
    memset(want, 0, 28);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
    uint8_t n1[28]; memcpy(n1, n, 28); n1[27] += 1;
    findmy_mod_be(n1, 28, n, out);
    want[27] = 1;
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
    uint8_t big[36]; memset(big, 0xff, 36);
    findmy_mod_be(big, 36, n, out);
    // python: (2**288-1) % n
    unhex("000000000000e95d1f470fc1ec22d6baa3a3d5c2ffffffffffffffff", want, 28);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
    // (a*b + c) mod n with small values: (3*5 + 7) = 22
    memset(a, 0, 28); memset(b, 0, 28); memset(c, 0, 28);
    a[27] = 3; b[27] = 5; c[27] = 7;
    findmy_muladd_mod(a, b, c, n, out);
    memset(want, 0, 28); want[27] = 22;
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
    // (n-1)*(n-1) + (n-1) mod n = 1 + (n-1) = 0 … python: ((n-1)*(n-1) + (n-1)) % n == 0
    memcpy(a, n, 28); a[27] -= 1; memcpy(b, a, 28); memcpy(c, a, 28);
    findmy_muladd_mod(a, b, c, n, out);
    memset(want, 0, 28);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
    // (n-1)*2 + 0 mod n = n-2
    memset(b, 0, 28); b[27] = 2; memset(c, 0, 28);
    findmy_muladd_mod(a, b, c, n, out);
    memcpy(want, n, 28); want[27] -= 2;
    TEST_ASSERT_EQUAL_MEMORY(want, out, 28);
}

void test_key_schedule_matches_python(void) {
    uint8_t seed[32], want[32], d[28];
    unhex(SEED_HEX, seed, 32);
    findmy_state_t s;
    findmy_init(&s, seed);
    unhex(D0_HEX, want, 28);
    TEST_ASSERT_EQUAL_MEMORY(want, s.d0, 28);
    unhex(SK0_HEX, want, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, s.sk, 32);
    TEST_ASSERT_EQUAL(0, s.index);
    for (int i = 0; i < 3; i++) {
        findmy_private_key(&s, d);
        unhex(D_HEX[i], want, 28);
        TEST_ASSERT_EQUAL_MEMORY(want, d, 28);
        findmy_advance(&s);
        TEST_ASSERT_EQUAL(i + 1, s.index);
    }
    unhex(SK1_HEX, want, 32);
    findmy_state_t t;
    findmy_init_at(&t, seed, 1);
    TEST_ASSERT_EQUAL_MEMORY(want, t.sk, 32);
    TEST_ASSERT_EQUAL(1, t.index);
    findmy_init_at(&t, seed, 2);
    findmy_private_key(&t, d);
    unhex(D_HEX[2], want, 28);
    TEST_ASSERT_EQUAL_MEMORY(want, d, 28);
    // determinism: same seed twice, same keys; a different seed, different keys
    findmy_state_t u;
    findmy_init(&u, seed);
    TEST_ASSERT_EQUAL_MEMORY(s.d0, u.d0, 28);
    seed[31] ^= 1;
    findmy_init(&u, seed);
    TEST_ASSERT_TRUE(memcmp(s.d0, u.d0, 28) != 0);
    // every private key is in [1, n-1]
    for (int i = 0; i < 50; i++) {
        findmy_private_key(&u, d);
        TEST_ASSERT_TRUE(memcmp(d, FINDMY_P224_N, 28) < 0);
        bool zero = true;
        for (int k = 0; k < 28; k++) if (d[k]) zero = false;
        TEST_ASSERT_FALSE(zero);
        findmy_advance(&u);
    }
}

void test_adv_payload_layout(void) {
    uint8_t pub[28], adv[31], addr[6], h[32], want[32];
    unhex(X_HEX[0], pub, 28);
    TEST_ASSERT_EQUAL(31, findmy_adv_payload(pub, 0x00, adv));
    TEST_ASSERT_EQUAL_HEX8(0x1e, adv[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, adv[1]);
    TEST_ASSERT_EQUAL_HEX8(0x4c, adv[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, adv[3]);
    TEST_ASSERT_EQUAL_HEX8(0x12, adv[4]);
    TEST_ASSERT_EQUAL_HEX8(0x19, adv[5]);
    TEST_ASSERT_EQUAL_HEX8(0x00, adv[6]);
    TEST_ASSERT_EQUAL_MEMORY(pub + 6, adv + 7, 22);
    TEST_ASSERT_EQUAL_HEX8(pub[0] >> 6, adv[29]);
    TEST_ASSERT_EQUAL_HEX8(0x00, adv[30]);
    findmy_adv_payload(pub, FINDMY_STATUS_BATT_LOW, adv);
    TEST_ASSERT_EQUAL_HEX8(0x80, adv[6]);
    findmy_adv_address(pub, addr);
    TEST_ASSERT_EQUAL_HEX8(pub[0] | 0xc0, addr[0]);
    TEST_ASSERT_EQUAL_MEMORY(pub + 1, addr + 1, 5);
    // the full key is recoverable from address + payload (what the iPhone and the fetcher do)
    uint8_t rebuilt[28];
    rebuilt[0] = (uint8_t)((addr[0] & 0x3f) | (adv[29] << 6));
    memcpy(rebuilt + 1, addr + 1, 5);
    memcpy(rebuilt + 6, adv + 7, 22);
    TEST_ASSERT_EQUAL_MEMORY(pub, rebuilt, 28);
    findmy_hashed_adv_key(pub, h);
    unhex(H0_HEX, want, 32);
    TEST_ASSERT_EQUAL_MEMORY(want, h, 32);
    TEST_ASSERT_EQUAL_HEX8(FINDMY_STATUS_BATT_FULL, findmy_status_for_mv(4100));
    TEST_ASSERT_EQUAL_HEX8(FINDMY_STATUS_BATT_MEDIUM, findmy_status_for_mv(3700));
    TEST_ASSERT_EQUAL_HEX8(FINDMY_STATUS_BATT_LOW, findmy_status_for_mv(3500));
    TEST_ASSERT_EQUAL_HEX8(FINDMY_STATUS_BATT_CRITICAL, findmy_status_for_mv(3200));
}

#ifdef FINDMY_HOST_MBEDTLS
void test_p224_public_key_host_mbedtls(void) {
    uint8_t d[28], x[28], want[28];
    for (int i = 0; i < 3; i++) {
        unhex(D_HEX[i], d, 28);
        unhex(X_HEX[i], want, 28);
        TEST_ASSERT_EQUAL(1, findmy_public_key(d, x));
        TEST_ASSERT_EQUAL_MEMORY(want, x, 28);
    }
    memset(d, 0, 28);
    TEST_ASSERT_EQUAL(0, findmy_public_key(d, x));   // scalar 0 is refused
}
#endif

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_sha256_known);
    RUN_TEST(test_x963_kdf);
    RUN_TEST(test_mod_helpers);
    RUN_TEST(test_key_schedule_matches_python);
    RUN_TEST(test_adv_payload_layout);
#ifdef FINDMY_HOST_MBEDTLS
    RUN_TEST(test_p224_public_key_host_mbedtls);
#endif
    return UNITY_END();
}
