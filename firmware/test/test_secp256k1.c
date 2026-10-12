#include "secp256k1.h"

#include "bip32.h"
#include "chainaddr.h"
#include "harness.h"
#include "keccak.h"
#include "keygen.h"
#include "ripemd160.h"
#include "sha256.h"
#include "u256.h"
#include "util.h"

/* Vector sources: Keccak/RIPEMD-160 reference digests, RFC 4231 (HMAC-SHA256 TC2), BIP-32 test vector 1
 * (xprv/xpub decoded to key + chain code), BIP-84 ("abandon ... about" first address), BIP-143 "Native
 * P2WPKH" example (key, pubkey, DER signature), EIP-155 example (signing hash, v/r/s under RFC 6979),
 * EIP-55 example addresses, BIP-173 v0 and BIP-350 v1 address vectors. */

static void test_hashes(void)
{
    uint8_t out[32], exp[32];
    keccak256((const uint8_t *)"", 0, out);
    hex2bin("c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470", exp, 32);
    CHECK_MEM(out, exp, 32);
    keccak256((const uint8_t *)"abc", 3, out);
    hex2bin("4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45", exp, 32);
    CHECK_MEM(out, exp, 32);
    {
        /* streaming across the 136-byte rate boundary equals one-shot */
        uint8_t big[300], a[32], b[32];
        keccak_ctx c;
        int i;
        for (i = 0; i < 300; i++) big[i] = (uint8_t)(i * 7);
        keccak256(big, 300, a);
        keccak256_init(&c);
        keccak256_update(&c, big, 100);
        keccak256_update(&c, big + 100, 200);
        keccak256_final(&c, b);
        CHECK_MEM(a, b, 32);
    }
    ripemd160((const uint8_t *)"", 0, out);
    hex2bin("9c1185a5c5e9fc54612808977ee8f548b2258d31", exp, 20);
    CHECK_MEM(out, exp, 20);
    ripemd160((const uint8_t *)"abc", 3, out);
    hex2bin("8eb208f7e05d987a9b044a8e98c6b087f15a0bfc", exp, 20);
    CHECK_MEM(out, exp, 20);
    {
        uint8_t m[56];
        memcpy(m, "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56);
        ripemd160(m, 56, out); /* two-block padding path */
        hex2bin("12a053384a9c0c88e405a06c27dcf49ada62eb2b", exp, 20);
        CHECK_MEM(out, exp, 20);
    }
    hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out);
    hex2bin("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", exp, 32);
    CHECK_MEM(out, exp, 32);
    sha256d((const uint8_t *)"hello", 5, out);
    hex2bin("9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50", exp, 32);
    CHECK_MEM(out, exp, 32);
}

static void test_u256(void)
{
    u256_t a, b, c;
    char s[U256_DEC_MAX];
    uint8_t be[32], exp[32];
    CHECK(u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &a));
    CHECK(u256_is_max(&a));
    CHECK(!u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639936", &a));
    CHECK(!u256_from_dec("", &a));
    CHECK(!u256_from_dec("12a", &a));
    CHECK(u256_from_dec("500000000000000000", &a));
    CHECK(u256_fits_u64(&a));
    CHECK_U64(u256_low64(&a), 500000000000000000ull);
    CHECK(u256_to_dec(&a, s, sizeof s) > 0);
    CHECK_STR(s, "500000000000000000");
    u256_zero(&a);
    CHECK(u256_to_dec(&a, s, sizeof s) == 1);
    CHECK_STR(s, "0");
    CHECK(u256_from_hex("0xde0b6b3a7640000", &a));
    CHECK_U64(u256_low64(&a), 1000000000000000000ull);
    CHECK(!u256_from_hex("de0b", &a));
    CHECK(!u256_from_hex("0x", &a));
    CHECK(!u256_from_hex("0xg", &a));
    /* 2^64 * 2^64 = 2^128 ; 2^128 * 2^128 overflows */
    u256_zero(&a); a.w[1] = 1;
    CHECK(u256_mul(&c, &a, &a));
    CHECK(c.w[2] == 1 && c.w[0] == 0 && c.w[1] == 0 && c.w[3] == 0);
    CHECK(!u256_mul(&b, &c, &c));
    CHECK(u256_is_max(&b));
    CHECK(u256_to_dec(&c, s, sizeof s) > 0);
    CHECK_STR(s, "340282366920938463463374607431768211456");
    /* 21000 * 20 gwei */
    u256_from_u64(&a, 21000); u256_from_u64(&b, 20000000000ull);
    CHECK(u256_mul(&c, &a, &b));
    CHECK_U64(u256_low64(&c), 420000000000000ull);
    /* add saturates */
    u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &a);
    u256_from_u64(&b, 1);
    CHECK(u256_add_sat(&c, &a, &b));
    CHECK(u256_is_max(&c));
    u256_from_u64(&a, UINT64_MAX); u256_from_u64(&b, 1);
    CHECK(!u256_add_sat(&c, &a, &b));
    CHECK(c.w[1] == 1 && c.w[0] == 0);
    CHECK_EQ(u256_cmp(&c, &a), 1);
    CHECK_EQ(u256_cmp(&a, &c), -1);
    CHECK_EQ(u256_cmp(&a, &a), 0);
    /* big-endian round trip and short inputs */
    hex2bin("000000000000000000000000000000000000000000000000000000000000ffee", exp, 32);
    CHECK(u256_from_be(&a, exp + 30, 2));
    CHECK_U64(u256_low64(&a), 0xffee);
    u256_to_be32(&a, be);
    CHECK_MEM(be, exp, 32);
    CHECK(!u256_from_be(&a, exp, 33));
    /* two's complement negation: -1 = all ones */
    u256_from_u64(&a, 1); u256_neg(&a);
    CHECK(u256_is_max(&a));
}

static void test_bip32_vector1(void)
{
    uint8_t seed[64] = {0}, exp[32], pub[64];
    bip32_node_t m, c1, c2, c3, c4;
    bip32_xpub_t xp, xc;
    int i;
    for (i = 0; i < 16; i++) seed[i] = (uint8_t)i;
    /* BIP32 TV1 uses a 16-byte seed: HMAC it directly (bip32_master takes the 64-byte BIP39 seed) */
    {
        uint8_t I[64];
        extern void hmac_sha512(const uint8_t *, size_t, const uint8_t *, size_t, uint8_t[64]);
        hmac_sha512((const uint8_t *)"Bitcoin seed", 12, seed, 16, I);
        memcpy(m.key, I, 32); memcpy(m.chain, I + 32, 32);
    }
    hex2bin("e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35", exp, 32); CHECK_MEM(m.key, exp, 32);
    hex2bin("873dff81c02f525623fd1fe5167eac3a55a049de3d314bb42ee227ffed37d508", exp, 32); CHECK_MEM(m.chain, exp, 32);
    CHECK(secp256k1_pubkey(m.key, pub));
    {
        uint8_t cpub[33], ecp[33];
        secp256k1_compress(pub, cpub);
        hex2bin("0339a36013301597daef41fbe593a02cc513d0b55527ec2df1050e2e8ff49c85c2", ecp, 33);
        CHECK_MEM(cpub, ecp, 33);
    }
    CHECK(bip32_ckd_priv(&m, 0 | BIP32_HARDENED, &c1));
    hex2bin("edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea", exp, 32); CHECK_MEM(c1.key, exp, 32);
    hex2bin("47fdacbd0f1097043b78c63c20c34ef4ed9a111d980047ad16282c7ae6236141", exp, 32); CHECK_MEM(c1.chain, exp, 32);
    CHECK(bip32_ckd_priv(&c1, 1, &c2));
    hex2bin("3c6cb8d0f6a264c91ea8b5030fadaa8e538b020f0a387421a12de9319dc93368", exp, 32); CHECK_MEM(c2.key, exp, 32);
    hex2bin("2a7857631386ba23dacac34180dd1983734e444fdbf774041578e9b6adb37c19", exp, 32); CHECK_MEM(c2.chain, exp, 32);
    CHECK(bip32_ckd_priv(&c2, 2 | BIP32_HARDENED, &c3));
    hex2bin("cbce0d719ecf7431d88e6a89fa1483e02e35092af60c042b1df2ff59fa424dca", exp, 32); CHECK_MEM(c3.key, exp, 32);
    hex2bin("04466b9cc8e161e966409ca52986c584f07e9dc81f735db683c3ff6ec7b1503f", exp, 32); CHECK_MEM(c3.chain, exp, 32);
    /* public derivation m/0H -> m/0H/1 matches the private one (xpub6ASuArn...) */
    CHECK(bip32_xpub(&c1, &xp));
    CHECK(bip32_ckd_pub(&xp, 1, &xc));
    {
        uint8_t ecp[33];
        hex2bin("03501e454bf00751f24b1b489aa925215d66af2234e3891c3b21a52bedb3cd711c", ecp, 33);
        CHECK_MEM(xc.pub, ecp, 33);
        CHECK_MEM(xc.chain, c2.chain, 32);
        CHECK(bip32_xpub(&c2, &xp));
        CHECK_MEM(xp.pub, ecp, 33);
    }
    CHECK(!bip32_ckd_pub(&xp, 2 | BIP32_HARDENED, &xc)); /* hardened public derivation is impossible */
    (void)c4;
}

static void test_standard_mnemonic_addresses(void)
{
    /* "abandon x11 about": EVM m/44'/60'/0'/0/0 = 0x9858EfFD232B4033E47d90003D41EC34EcaEda94,
     * BTC m/84'/0'/0'/0/0 = bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu (BIP-84 test vector). */
    static const char *M = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
    uint8_t seed[64], exp[64], pub[64], addr[20], pkh[20];
    bip32_node_t n;
    char text[BTC_ADDR_MAX];
    keygen_seed_from_mnemonic(M, "", seed);
    hex2bin("5eb00bbddcf069084889a8ab9155568165f5c453ccb85e70811aaed6f6da5fc19a5ac40b389cd370d086206dec8aa6c43daea6690f20ad3d8d48b2d2ce9e38e4", exp, 64);
    CHECK_MEM(seed, exp, 64);
    CHECK(bip32_derive(seed, BIP32_PATH_EVM, 5, &n));
    hex2bin("1ab42cc412b618bdea3a599e3c9bae199ebf030895b039e9db1e30dafb12b727", exp, 32);
    CHECK_MEM(n.key, exp, 32);
    CHECK(secp256k1_pubkey(n.key, pub));
    evm_address_from_pubkey(pub, addr);
    evm_address_to_hex(addr, text);
    CHECK_STR(text, "0x9858EfFD232B4033E47d90003D41EC34EcaEda94");
    CHECK(bip32_derive(seed, BIP32_PATH_BTC, 5, &n));
    hex2bin("4604b4b710fe91f584fff084e1a9159fe4f8408fff380596a604948474ce4fa3", exp, 32);
    CHECK_MEM(n.key, exp, 32);
    CHECK(secp256k1_pubkey(n.key, pub));
    {
        uint8_t cp[33];
        secp256k1_compress(pub, cp);
        hex2bin("0330d54fd0dd420a6e5f8d3624f5f3482cae350f79d5f0753bf5beef9c2d91af3c", exp, 33);
        CHECK_MEM(cp, exp, 33);
    }
    btc_pkh_from_pubkey(pub, pkh);
    CHECK(btc_p2wpkh_address(pkh, text));
    CHECK_STR(text, "bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu");
    /* the account xpub derives the same leaf: m/84'/0'/0' then /0/0 publicly */
    {
        bip32_node_t acct;
        bip32_xpub_t xa, x0, x00;
        CHECK(bip32_derive(seed, BIP32_PATH_BTC_ACCT, 3, &acct));
        CHECK(bip32_xpub(&acct, &xa));
        CHECK(bip32_ckd_pub(&xa, 0, &x0));
        CHECK(bip32_ckd_pub(&x0, 0, &x00));
        CHECK_MEM(x00.pub, exp, 33);
    }
}

static void test_eip155_signature(void)
{
    /* EIP-155: key 0x46..46, signing hash daf5a7..., deterministic (RFC 6979) r/s published in the EIP */
    uint8_t priv[32], digest[32], sig[64], exp[64], pub[64], rec[64];
    uint8_t recid = 9;
    memset(priv, 0x46, 32);
    hex2bin("daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53", digest, 32);
    CHECK(secp256k1_sign(priv, digest, sig, &recid));
    hex2bin("28ef61340bd939bc2195fe537567866003e1a15d3c71ff63e1590620aa636276"
            "67cbe9d8997f761aecb703304b3800ccf555c9f3dc64214b297fb1966a3b6d83", exp, 64);
    CHECK_MEM(sig, exp, 64);
    CHECK_EQ(recid, 0); /* v = 37 = 1*2 + 35 + 0 */
    CHECK(secp256k1_is_low_s(sig));
    CHECK(secp256k1_pubkey(priv, pub));
    CHECK(secp256k1_verify(pub, digest, sig));
    CHECK(secp256k1_recover(digest, sig, 0, rec));
    CHECK_MEM(rec, pub, 64);
    CHECK(secp256k1_recover(digest, sig, 1, rec));
    CHECK(memcmp(rec, pub, 64) != 0);
    CHECK_EQ(secp256k1_find_recid(pub, digest, sig), 0);
    /* a tampered signature verifies against nothing we know */
    sig[5] ^= 1;
    CHECK(!secp256k1_verify(pub, digest, sig));
    CHECK_EQ(secp256k1_find_recid(pub, digest, sig), -1);
    sig[5] ^= 1;
    /* high-s form of the same signature: normalising flips recid bit 0 and restores the vector */
    {
        uint8_t hi[64], n_minus_s[32];
        uint8_t n_be[32];
        hex2bin("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", n_be, 32);
        memcpy(hi, sig, 32);
        {   /* n - s by hand, big-endian */
            int i, borrow = 0;
            for (i = 31; i >= 0; i--) {
                int d = (int)n_be[i] - (int)sig[32 + i] - borrow;
                borrow = d < 0;
                if (borrow) d += 256;
                n_minus_s[i] = (uint8_t)d;
            }
        }
        memcpy(hi + 32, n_minus_s, 32);
        CHECK(!secp256k1_is_low_s(hi));
        CHECK(secp256k1_verify(pub, digest, hi)); /* ECDSA accepts both */
        CHECK_EQ(secp256k1_find_recid(pub, digest, hi), 1);
        CHECK(secp256k1_normalize_s(hi));
        CHECK_MEM(hi, sig, 64);
        CHECK(!secp256k1_normalize_s(hi));
    }
    /* zero and n are not scalars */
    memset(priv, 0, 32);
    CHECK(!secp256k1_scalar_valid(priv));
    CHECK(!secp256k1_sign(priv, digest, sig, &recid));
    hex2bin("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", priv, 32);
    CHECK(!secp256k1_scalar_valid(priv));
    priv[31] = 0x40;
    CHECK(secp256k1_scalar_valid(priv));
}

static void test_bip143_key_and_der(void)
{
    uint8_t priv[32], pub[64], cp[33], exp[72], sig[64], der[SECP256K1_DER_MAX], sighash[32];
    size_t n;
    hex2bin("619c335025c7f4012e556c2a58b2506e30b8511b53ade95ea316fd8c3286feb9", priv, 32);
    CHECK(secp256k1_pubkey(priv, pub));
    secp256k1_compress(pub, cp);
    hex2bin("025476c2e83188368da1ff3e292e7acafcdb3566bb0ad253f62fc70f07aeee6357", exp, 33);
    CHECK_MEM(cp, exp, 33);
    {
        uint8_t back[64];
        CHECK(secp256k1_decompress(cp, back));
        CHECK_MEM(back, pub, 64);
    }
    /* the published signature (random k, so not reproducible) decodes, verifies and re-encodes identically */
    hex2bin("304402203609e17b84f6a7d30c80bfa610b5b4542f32a8a0d5447a12fb1366d7f01cc44a0220573a954c4518331561406f90300e8f3358f51928d43c212a8caed02de67eebee", exp, 70);
    CHECK(secp256k1_der_decode(exp, 70, sig));
    hex2bin("c37af31116d1b27caf68aae9e3ac82f1477929014d5b917657d0eb49478cb670", sighash, 32);
    CHECK(secp256k1_verify(pub, sighash, sig));
    CHECK(secp256k1_is_low_s(sig));
    n = secp256k1_der_encode(sig, der);
    CHECK_EQ(n, 70);
    CHECK_MEM(der, exp, 70);
    CHECK(secp256k1_find_recid(pub, sighash, sig) >= 0);
    /* DER edge: a high bit forces a leading zero; a leading zero without need is rejected */
    memset(sig, 0, 64); sig[0] = 0x80; sig[63] = 0x01;
    n = secp256k1_der_encode(sig, der);
    CHECK_EQ(n, 2 + 35 + 3);
    CHECK_EQ(der[2], 0x02); CHECK_EQ(der[3], 33); CHECK_EQ(der[4], 0x00); CHECK_EQ(der[5], 0x80);
    {
        uint8_t back[64];
        CHECK(secp256k1_der_decode(der, n, back));
        CHECK_MEM(back, sig, 64);
        der[3] = 34; /* claims one more byte than present */
        CHECK(!secp256k1_der_decode(der, n, back));
        exp[0] = 0x30; exp[1] = 6; exp[2] = 0x02; exp[3] = 2; exp[4] = 0x00; exp[5] = 0x01; exp[6] = 0x02; exp[7] = 0x01; /* non-minimal r */
        CHECK(!secp256k1_der_decode(exp, 8, back));
    }
    /* our own deterministic signature over the same sighash verifies too */
    {
        uint8_t recid;
        CHECK(secp256k1_sign(priv, sighash, sig, &recid));
        CHECK(secp256k1_verify(pub, sighash, sig));
        CHECK(secp256k1_is_low_s(sig));
    }
}

static void test_addresses(void)
{
    static const char *eip55[] = {"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed", "0xfB6916095ca1df60bB79Ce92cE3Ea74c37c5d359",
                                  "0xdbF03B407c01E7cD3CBea99509d93f8DDDC8C6FB", "0xD1220A0cf47c7B9Be7A2E6BA89F429762e7b9aDb"};
    uint8_t addr[20], script[42], exp[42];
    char out[BTC_ADDR_MAX];
    size_t i;
    for (i = 0; i < 4; i++) {
        char lower[EVM_ADDR_HEX_LEN], back[EVM_ADDR_HEX_LEN];
        size_t k;
        CHECK(evm_address_from_hex(eip55[i], addr));
        evm_address_to_hex(addr, back);
        CHECK_STR(back, eip55[i]);
        for (k = 0; eip55[i][k]; k++) lower[k] = (char)((eip55[i][k] >= 'A' && eip55[i][k] <= 'F') ? eip55[i][k] + 32 : eip55[i][k]);
        lower[k] = 0;
        CHECK(evm_address_from_hex(lower, addr)); /* all-lowercase is accepted */
    }
    CHECK(!evm_address_from_hex("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD", addr)); /* bad checksum case */
    CHECK(!evm_address_from_hex("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAe", addr));
    CHECK(!evm_address_from_hex("5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed00", addr));
    /* BIP-173 v0 vectors, BIP-350 v1 vector */
    hex2bin("0014751e76e8199196d454941c45d1b3a323f1433bd6", exp, 22);
    CHECK(btc_script_to_address(exp, 22, out));
    CHECK_STR(out, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    CHECK_EQ(btc_address_to_script("BC1QW508D6QEJXTDG4Y5R3ZARVARY0C5XW7KV8F3T4", script), 22);
    CHECK_MEM(script, exp, 22);
    hex2bin("512079be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798", exp, 34);
    CHECK(btc_script_to_address(exp, 34, out));
    CHECK_STR(out, "bc1p0xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqzk5jj0");
    CHECK_EQ(btc_address_to_script(out, script), 34);
    CHECK_MEM(script, exp, 34);
    CHECK_EQ(btc_address_to_script("bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5", script), 0); /* checksum */
    CHECK_EQ(btc_address_to_script("tb1qrp33g0q5c5txsp9arysrx4k6zdkfs4nce4xj0gdcccefvpysxf3q0sl5k7", script), 0); /* testnet hrp */
    CHECK_EQ(btc_address_to_script("", script), 0);
    /* legacy: P2PKH of the BIP-143 example key hash and a P2SH */
    hex2bin("76a9141d0f172a0ecb48aee1be1f2687d2963ae33f71a188ac", exp, 25);
    CHECK(btc_script_to_address(exp, 25, out));
    CHECK_EQ(btc_address_to_script(out, script), 25);
    CHECK_MEM(script, exp, 25);
    CHECK_EQ(out[0], '1');
    hex2bin("a9141d0f172a0ecb48aee1be1f2687d2963ae33f71a187", exp, 23);
    CHECK(btc_script_to_address(exp, 23, out));
    CHECK_EQ(out[0], '3');
    CHECK_EQ(btc_address_to_script(out, script), 23);
    CHECK_MEM(script, exp, 23);
    /* OP_RETURN has no address */
    exp[0] = 0x6a; exp[1] = 0x01; exp[2] = 0x00;
    CHECK(!btc_script_to_address(exp, 3, out));
}

int main(void)
{
    RUN(test_hashes);
    RUN(test_u256);
    RUN(test_bip32_vector1);
    RUN(test_standard_mnemonic_addresses);
    RUN(test_eip155_signature);
    RUN(test_bip143_key_and_der);
    RUN(test_addresses);
    TEST_END();
}
