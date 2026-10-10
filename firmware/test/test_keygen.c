#include "keygen.h"

#include "chainaddr.h"
#include "ed25519.h"
#include "harness.h"
#include "secp256k1.h"
#include "hmac_sha512.h"
#include "sha256.h"
#include "signer.h"
#include "util.h"

/* Expected values were produced independently with Python (hashlib/hmac + cryptography), after
 * confirming that implementation against the published SLIP-0010 and BIP-39 test vectors. */

static void test_primitives(void)
{
    uint8_t out[64], exp[64];
    sha256((const uint8_t *)"abc", 3, out);
    hex2bin("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", exp, 32);
    CHECK_MEM(out, exp, 32);
    hmac_sha512((const uint8_t *)"key", 3, (const uint8_t *)"The quick brown fox jumps over the lazy dog", 43, out);
    hex2bin("b42af09057bac1e2d41708e48a902e09b5ff7f12ab428a4fe86653c73dd248fb82f948a549f7b791a5b41915ee4d1ec3935357e4e2317250d0372afa2ebeeb3a", exp, 64);
    CHECK_MEM(out, exp, 64);
    pbkdf2_hmac_sha512((const uint8_t *)"password", 8, (const uint8_t *)"salt", 4, 1, out, 64);
    hex2bin("867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce", exp, 64);
    CHECK_MEM(out, exp, 64);
    /* long message through the streaming SHA-256 path */
    {
        sha256_ctx c;
        uint8_t big[1000];
        int i;
        memset(big, 'a', sizeof big);
        sha256_init(&c);
        for (i = 0; i < 1000; i++) sha256_update(&c, big, 1000); /* one million 'a' */
        sha256_final(&c, out);
        hex2bin("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", exp, 32);
        CHECK_MEM(out, exp, 32);
    }
}

static void test_slip10_published_vector(void)
{
    /* SLIP-0010 test vector 1 for ed25519: seed 000102..0f, chain m/0' */
    uint8_t seed[64] = {0}, priv[32], chain[32], exp[32], pub[32], sk[64];
    const uint32_t path[1] = {0};
    int i;
    for (i = 0; i < 16; i++) seed[i] = (uint8_t)i;
    /* The published vector uses a 16-byte seed; our API takes 64 bytes, so use the generic HMAC directly. */
    {
        uint8_t I[64], data[37];
        hmac_sha512((const uint8_t *)"ed25519 seed", 12, seed, 16, I);
        data[0] = 0; memcpy(data + 1, I, 32); data[33] = 0x80; data[34] = 0; data[35] = 0; data[36] = 0;
        hmac_sha512(I + 32, 32, data, 37, I);
        memcpy(priv, I, 32); memcpy(chain, I + 32, 32);
    }
    (void)path;
    hex2bin("68e0fe46dfb67e368c75379acec591dad19df3cde26e63b93a8e704f1dade7a3", exp, 32);
    CHECK_MEM(priv, exp, 32);
    hex2bin("8b59aa11380b624e81507a27fedda59fea6d0b779a778918a2fd3590e16e9c69", exp, 32);
    CHECK_MEM(chain, exp, 32);
    ed25519_create_keypair(pub, sk, priv);
    hex2bin("8c8a13df77a28f3445213a0f432fde644acaa215fc72dcdf300d5efaa85d350c", exp, 32);
    CHECK_MEM(pub, exp, 32);
}

static void test_mnemonic_vector(void)
{
    uint8_t ent[32] = {0}, seed[64], exp[64], priv[32], pub[32], sk[64];
    char words[KEYGEN_MNEMONIC_CAP], b58[48];
    CHECK_EQ(keygen_mnemonic_from_entropy(ent, words, sizeof words), 0);
    CHECK_STR(words, "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                     "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art");
    CHECK(keygen_mnemonic_valid(words));
    /* BIP-39 official vector (passphrase "TREZOR") */
    keygen_seed_from_mnemonic(words, "TREZOR", seed);
    hex2bin("bda85446c68413707090a52022edd26a1c9462295029f2e60cd7c4f2bbd3097170af7a4d73245cafa9c3cca8d561a7c3de6f5d4a10be8ed2a5e608d68f92fcc8", exp, 64);
    CHECK_MEM(seed, exp, 64);
    /* what the controller uses: empty passphrase */
    keygen_seed_from_mnemonic(words, "", seed);
    hex2bin("408b285c123836004f4b8842c89324c1f01382450c0d439af345ba7fc49acf705489c6fc77dbd4e3dc1dd8cc6bc9f043db8ada1e243c4a0eafb290d399480840", exp, 64);
    CHECK_MEM(seed, exp, 64);
    keygen_slip10_solana(seed, priv);
    hex2bin("7c139e1a603ca04f5f7cff194e1bb6f6d1b9098470ea90695ab628488a9f921b", exp, 32);
    CHECK_MEM(priv, exp, 32);
    ed25519_create_keypair(pub, sk, priv);
    pubkey_to_b58(pub, b58, sizeof b58);
    CHECK_STR(b58, "3Cy3YNTFywCmxoxt8n7UH6hg6dLo5uACowX3CFceaSnx");
    /* all-in-one */
    memset(priv, 0, 32);
    CHECK_EQ(keygen_derive(ent, words, sizeof words, priv), 0);
    CHECK_MEM(priv, exp, 32);
    /* second vector (random entropy) */
    hex2bin("f585c11aec520db57dd353c69554b21a89b20fb0650966fa0a9d6f74fd989d8f", ent, 32);
    CHECK_EQ(keygen_derive(ent, words, sizeof words, priv), 0);
    CHECK_STR(words, "void come effort suffer camp survey warrior heavy shoot primary clutch crush open amazing screen "
                     "patrol group space point ten exist slush involve unfold");
    CHECK(keygen_mnemonic_valid(words));
    ed25519_create_keypair(pub, sk, priv);
    pubkey_to_b58(pub, b58, sizeof b58);
    CHECK_STR(b58, "Fc2kvN7xbSyrhiGoXsuasXN3UjJLhrBnUg38f3F2raic");
    CHECK(keygen_mnemonic_from_entropy(ent, words, 50) < 0);
}

static void test_checksum_and_validation(void)
{
    char words[KEYGEN_MNEMONIC_CAP];
    uint8_t ent[32];
    int i;
    for (i = 0; i < 32; i++) ent[i] = (uint8_t)(i * 37 + 11);
    keygen_mnemonic_from_entropy(ent, words, sizeof words);
    CHECK(keygen_mnemonic_valid(words));
    /* swap the last word for another valid word -> checksum fails (statistically; "zoo" vs the real one) */
    {
        char *last = strrchr(words, ' ') + 1;
        char bad[KEYGEN_MNEMONIC_CAP];
        snprintf(bad, sizeof bad, "%.*s%s", (int)(last - words), words, strcmp(last, "zoo") ? "zoo" : "zone");
        CHECK(!keygen_mnemonic_valid(bad));
    }
    CHECK(!keygen_mnemonic_valid("abandon abandon notaword"));
    CHECK(!keygen_mnemonic_valid("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")); /* 12 words */
    CHECK(!keygen_mnemonic_valid(""));
    CHECK(!keygen_mnemonic_valid(NULL));
}

static void test_determinism_and_signing(void)
{
    uint8_t ent[32], priv1[32], priv2[32], pub[32], pub2[32], sig[64];
    char w1[KEYGEN_MNEMONIC_CAP], w2[KEYGEN_MNEMONIC_CAP];
    const uint8_t msg[] = "pincer1 consent";
    int i;
    for (i = 0; i < 32; i++) ent[i] = (uint8_t)(255 - i);
    CHECK_EQ(keygen_derive(ent, w1, sizeof w1, priv1), 0);
    CHECK_EQ(keygen_derive(ent, w2, sizeof w2, priv2), 0);
    CHECK_STR(w1, w2);
    CHECK_MEM(priv1, priv2, 32);
    ent[0] ^= 1;
    CHECK_EQ(keygen_derive(ent, w2, sizeof w2, priv2), 0);
    CHECK(strcmp(w1, w2) != 0);
    CHECK(memcmp(priv1, priv2, 32) != 0);
    /* derived key signs and verifies through the software signer (slot 0 real, slot 1 decoy) */
    CHECK_EQ(signer_init(), 0);
    CHECK(!signer_is_hardware());
    CHECK(!signer_slot_present(0));
    CHECK_EQ(signer_import(0, priv1, pub), 0);
    CHECK_EQ(signer_import(1, priv2, pub2), 0);
    CHECK(memcmp(pub, pub2, 32) != 0);
    CHECK_EQ(signer_sign(0, msg, sizeof msg - 1, sig), 0);
    CHECK(ed25519_verify(sig, msg, sizeof msg - 1, pub));
    CHECK(!ed25519_verify(sig, msg, sizeof msg - 1, pub2));
    CHECK_EQ(signer_sign(1, msg, sizeof msg - 1, sig), 0);
    CHECK(ed25519_verify(sig, msg, sizeof msg - 1, pub2));
    CHECK(signer_sign(2, msg, 1, sig) != 0);
    {
        uint8_t blob[2][32], p[32], ph[32], zero[32] = {0};
        signer_soft_export(blob, ph);
        CHECK_MEM(blob[0], priv1, 32);
        CHECK_MEM(ph, zero, 32); /* no PIN yet */
        /* reload into a fresh process-state would need a restart; at least verify load is idempotent */
        signer_soft_load((const uint8_t (*)[32])blob, ph);
        signer_pubkey(0, p);
        CHECK_MEM(p, pub, 32);
    }
    /* software PIN: once set, signing needs a verified session; wipe removes everything */
    {
        const uint8_t code[8] = {'P', 'R', 5, 4, (uint8_t)~5, (uint8_t)~4, 0x5A, 0xA5};
        const uint8_t other[8] = {'P', 'R', 5, 5, (uint8_t)~5, (uint8_t)~5, 0x5A, 0xA5};
        uint8_t blob[2][32], ph[32], zero[32] = {0};
        CHECK(!signer_pin_present());
        CHECK_EQ(signer_pin_verify(code), -1); /* nothing to verify against */
        CHECK_EQ(signer_pin_set(code), 0);
        CHECK(signer_pin_present());
        CHECK(signer_sign(0, msg, 1, sig) != 0); /* no session */
        CHECK_EQ(signer_pin_verify(other), 1);
        CHECK(signer_sign(0, msg, 1, sig) != 0);
        CHECK_EQ(signer_pin_verify(code), 0);
        CHECK_EQ(signer_sign(0, msg, sizeof msg - 1, sig), 0);
        CHECK(ed25519_verify(sig, msg, sizeof msg - 1, pub));
        signer_pin_close();
        CHECK(signer_sign(0, msg, 1, sig) != 0);
        signer_soft_export(blob, ph);
        CHECK(memcmp(ph, zero, 32) != 0);
        CHECK_EQ(signer_wipe(), 0);
        CHECK(!signer_pin_present());
        CHECK(!signer_slot_present(0) && !signer_slot_present(1));
        CHECK(signer_sign(0, msg, 1, sig) != 0);
        signer_soft_export(blob, ph);
        CHECK_MEM(blob[0], zero, 32);
        CHECK_MEM(ph, zero, 32);
        /* a persisted hash comes back as a set PIN */
        signer_pin_set(code);
        signer_soft_export(blob, ph);
        signer_wipe();
        signer_soft_load((const uint8_t (*)[32])blob, ph);
        CHECK(signer_pin_present());
        CHECK_EQ(signer_pin_verify(code), 0);
        /* second rhythm: verify2 tells real / duress / wrong; a duress session signs for the decoy slot only */
        {
            const uint8_t dur[8] = {'P', 'R', 6, 0x12, (uint8_t)~6, (uint8_t)~0x12, 0x5A, 0xA5};
            uint8_t salt[16], dh[32], ds[16], zero16[16] = {0};
            uint8_t seed1[32], pub1[32];
            memset(salt, 0x5a, 16);
            memset(seed1, 0x33, 32);
            signer_import(0, priv1, pub1); /* the wipe above removed the keys; the PIN hash came back from the blob */
            signer_import(1, seed1, pub1);
            CHECK(!signer_duress_pin_present());
            CHECK_EQ(signer_pin_verify2(code), SIGNER_VERIFY_REAL);  /* without a second object: the real one */
            CHECK_EQ(signer_pin_verify2(other), SIGNER_VERIFY_WRONG);
            CHECK_EQ(signer_duress_pin_set(dur, salt), 0);
            CHECK(signer_duress_pin_present());
            CHECK_EQ(signer_pin_verify2(code), SIGNER_VERIFY_REAL);
            CHECK_EQ(signer_sign(0, msg, 1, sig), 0);
            CHECK_EQ(signer_pin_verify2(dur), SIGNER_VERIFY_DURESS);
            CHECK(signer_sign(0, msg, 1, sig) != 0);       /* the real key refuses a duress session */
            CHECK_EQ(signer_sign(1, msg, 1, sig), 0);      /* the decoy key signs */
            CHECK_EQ(signer_pin_verify2(other), SIGNER_VERIFY_WRONG);
            CHECK(signer_sign(1, msg, 1, sig) != 0);
            CHECK_EQ(signer_pin_verify(dur), 1);           /* the plain real-only check does not accept it */
            /* persisted, replaceable, wiped */
            signer_soft_export_duress(dh, ds);
            CHECK(memcmp(ds, zero16, 16) == 0 ? 0 : 1);
            CHECK_MEM(ds, salt, 16);
            signer_soft_load((const uint8_t (*)[32])blob, ph); /* reload keeps the duress hash: separate blob */
            CHECK_EQ(signer_duress_pin_set(other, salt), 0);   /* replaced */
            CHECK_EQ(signer_pin_verify2(dur), SIGNER_VERIFY_WRONG);
            CHECK_EQ(signer_pin_verify2(other), SIGNER_VERIFY_DURESS);
            signer_wipe();
            CHECK(!signer_duress_pin_present());
            signer_soft_load((const uint8_t (*)[32])blob, ph);
            signer_soft_load_duress(dh, ds);
            CHECK(signer_duress_pin_present());
            CHECK_EQ(signer_pin_verify2(dur), SIGNER_VERIFY_DURESS);
            signer_wipe();
            signer_soft_export_duress(dh, ds);
            CHECK_MEM(ds, zero16, 16);
        }
    }
}

/* One seed, three chains: the standard "abandon x11 about" mnemonic must give the published EVM and BTC addresses. */
static void test_multichain_keys(void)
{
    uint8_t seed[64], pub[64], addr[20], exp[32], ent[32];
    keygen_keys_t k;
    char text[BTC_ADDR_MAX], words[KEYGEN_MNEMONIC_CAP];
    keygen_seed_from_mnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "", seed);
    CHECK(keygen_keys_from_seed(seed, &k));
    CHECK(secp256k1_pubkey(k.evm_priv, pub));
    evm_address_from_pubkey(pub, addr);
    evm_address_to_hex(addr, text);
    CHECK_STR(text, "0x9858EfFD232B4033E47d90003D41EC34EcaEda94");
    CHECK(secp256k1_pubkey(k.btc_priv, pub));
    btc_pkh_from_pubkey(pub, addr);
    CHECK(btc_p2wpkh_address(addr, text));
    CHECK_STR(text, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    /* the account xpub derives the same receive key: m/84'/0'/0' -> /0 -> /0 */
    {
        bip32_xpub_t b, l;
        uint8_t cp[33];
        CHECK(bip32_ckd_pub(&k.btc_account, 0, &b));
        CHECK(bip32_ckd_pub(&b, 0, &l));
        secp256k1_compress(pub, cp);
        CHECK_MEM(l.pub, cp, 33);
    }
    /* the Solana key is the same one keygen_slip10_solana gives (24-word vector from above) */
    keygen_seed_from_mnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                              "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art", "", seed);
    CHECK(keygen_keys_from_seed(seed, &k));
    hex2bin("7c139e1a603ca04f5f7cff194e1bb6f6d1b9098470ea90695ab628488a9f921b", exp, 32);
    CHECK_MEM(k.sol_priv, exp, 32);
    /* all-in-one from entropy: zero entropy is the 24 x abandon + art mnemonic */
    memset(ent, 0, 32);
    CHECK_EQ(keygen_derive_all(ent, words, sizeof words, &k), 0);
    CHECK_MEM(k.sol_priv, exp, 32);
    CHECK(secp256k1_scalar_valid(k.evm_priv) && secp256k1_scalar_valid(k.btc_priv));
    CHECK(memcmp(k.evm_priv, k.btc_priv, 32) != 0);
    CHECK(keygen_derive_all(ent, words, 50, &k) < 0);
}

/* The software signer holds the secp256k1 keys next to the ed25519 one, under the same PIN gate. */
static void test_soft_signer_secp256k1(void)
{
    uint8_t ent[32], pub_evm[64], pub_btc[64], pub2[64], digest[32], sig65[65], rec[64];
    keygen_keys_t k;
    char words[KEYGEN_MNEMONIC_CAP];
    const uint8_t code[8] = {'P', 'R', 5, 4, (uint8_t)~5, (uint8_t)~4, 0x5A, 0xA5};
    int i;
    for (i = 0; i < 32; i++) ent[i] = (uint8_t)(i * 3 + 1);
    signer_wipe();
    CHECK_EQ(keygen_derive_all(ent, words, sizeof words, &k), 0);
    CHECK(!signer_secp256k1_present(0, SIGNER_CHAIN_EVM));
    CHECK_EQ(signer_import_secp256k1(0, SIGNER_CHAIN_EVM, k.evm_priv, pub_evm), 0);
    CHECK_EQ(signer_import_secp256k1(0, SIGNER_CHAIN_BTC, k.btc_priv, pub_btc), 0);
    CHECK(signer_secp256k1_present(0, SIGNER_CHAIN_EVM) && signer_secp256k1_present(0, SIGNER_CHAIN_BTC));
    CHECK(!signer_secp256k1_present(1, SIGNER_CHAIN_EVM));
    CHECK(memcmp(pub_evm, pub_btc, 64) != 0);
    CHECK_EQ(signer_pubkey_secp256k1(0, SIGNER_CHAIN_BTC, pub2), 0);
    CHECK_MEM(pub2, pub_btc, 64);
    CHECK(signer_pubkey_secp256k1(0, 2, pub2) != 0);
    CHECK(signer_import_secp256k1(2, SIGNER_CHAIN_EVM, k.evm_priv, pub2) != 0);
    memset(digest, 0x5a, 32);
    /* no PIN set yet: signing allowed (init flow) */
    CHECK_EQ(signer_sign_secp256k1(0, SIGNER_CHAIN_EVM, digest, sig65), 0);
    CHECK(secp256k1_verify(pub_evm, digest, sig65));
    CHECK(secp256k1_is_low_s(sig65));
    CHECK(sig65[64] <= 3);
    CHECK(secp256k1_recover(digest, sig65, sig65[64], rec));
    CHECK_MEM(rec, pub_evm, 64);
    /* PIN gate */
    CHECK_EQ(signer_pin_set(code), 0);
    CHECK(signer_sign_secp256k1(0, SIGNER_CHAIN_BTC, digest, sig65) != 0);
    CHECK_EQ(signer_pin_verify(code), 0);
    CHECK_EQ(signer_sign_secp256k1(0, SIGNER_CHAIN_BTC, digest, sig65), 0);
    CHECK(secp256k1_verify(pub_btc, digest, sig65));
    CHECK(!secp256k1_verify(pub_evm, digest, sig65));
    signer_pin_close();
    CHECK(signer_sign_secp256k1(0, SIGNER_CHAIN_BTC, digest, sig65) != 0);
    /* export / wipe / load round trip */
    {
        uint8_t blob[2][2][32], zero[32] = {0};
        signer_soft_export_secp(blob);
        CHECK_MEM(blob[0][0], k.evm_priv, 32);
        CHECK_MEM(blob[0][1], k.btc_priv, 32);
        CHECK_MEM(blob[1][0], zero, 32);
        CHECK_EQ(signer_wipe(), 0);
        CHECK(!signer_secp256k1_present(0, SIGNER_CHAIN_EVM));
        signer_soft_export_secp(blob);
        CHECK_MEM(blob[0][0], zero, 32);
        memcpy(blob[0][0], k.evm_priv, 32); memcpy(blob[0][1], k.btc_priv, 32);
        signer_soft_load_secp((const uint8_t (*)[2][32])blob);
        CHECK(signer_secp256k1_present(0, SIGNER_CHAIN_EVM) && signer_secp256k1_present(0, SIGNER_CHAIN_BTC));
        CHECK(!signer_secp256k1_present(1, SIGNER_CHAIN_BTC));
        CHECK_EQ(signer_pubkey_secp256k1(0, SIGNER_CHAIN_EVM, pub2), 0);
        CHECK_MEM(pub2, pub_evm, 64);
    }
    signer_wipe();
}

int main(void)
{
    RUN(test_primitives);
    RUN(test_slip10_published_vector);
    RUN(test_mnemonic_vector);
    RUN(test_checksum_and_validation);
    RUN(test_determinism_and_signing);
    RUN(test_multichain_keys);
    RUN(test_soft_signer_secp256k1);
    TEST_END();
}
