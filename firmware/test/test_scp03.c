/* SCP03 + AES-128 + CMAC against vectors produced independently with Python `cryptography`
 * (AES/CMAC primitives validated there against FIPS-197 and RFC 4493 published values). The SCP03
 * trace was computed from the GlobalPlatform Amd D rules as implemented by NXP nxScp03_Com.c. */
#include "scp03.h"

#include "aes128.h"
#include "harness.h"
#include "util.h"

static const char *ENC_HEX = "0f0e0d0c0b0a09080706050403020100";
static const char *MAC_HEX = "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
static const char *HOST_HEX = "0102030405060708";
static const char *CARD_HEX = "f1f2f3f4f5f6f7f8";
static const char *CARD_CG_HEX = "8adcf126cd267e77";
static const char *EXT_APDU_HEX = "84823300102b14de67bf266f338a7b025bdc0c7ecb";
static const char *MCV0_HEX = "8a7b025bdc0c7ecbb584ad898cc62fd9";
static const char *C1_HEX = "84020000081dc7ae66e9d8f618";
static const char *R1_HEX = "da658e2a6e84bf5ff90c2358d159c220842a7278606895799000";
static const char *C2_HEX = "84010203000028038ad3fc15f6dce5e89c68582a01659d4e1bdb9e0438616a972072758402977a4dc58eb1c76816650000";
static const char *R2_HEX = "33a70747a48b81d770f61a443b076bb18778ebeb36be09af794b2ca44ababd0b501a1475b66f44719000";
static const char *MCV2_HEX = "4dc58eb1c768166560307a839dafe10b";

static void test_aes_fips197(void)
{
    aes128_t c;
    uint8_t k[16], p[16], out[16], exp[16], back[16];
    hex2bin("000102030405060708090a0b0c0d0e0f", k, 16);
    hex2bin("00112233445566778899aabbccddeeff", p, 16);
    hex2bin("69c4e0d86a7b0430d8cdb78070b4c55a", exp, 16);
    aes128_init(&c, k);
    aes128_encrypt_block(&c, p, out);
    CHECK_MEM(out, exp, 16);
    aes128_decrypt_block(&c, out, back);
    CHECK_MEM(back, p, 16);
    /* CBC, two blocks, IV 00..0f */
    {
        uint8_t iv[16], in[32], ct[32], ctexp[32], pt[32];
        int i;
        for (i = 0; i < 16; i++) { iv[i] = (uint8_t)i; in[i] = p[i]; in[16 + i] = p[15 - i]; }
        hex2bin("76d0627da1d290436e21a4af7fca94b730cdf5479769414250df6cf5d3fcae8e", ctexp, 32);
        aes128_cbc_encrypt(&c, iv, in, ct, 32);
        CHECK_MEM(ct, ctexp, 32);
        aes128_cbc_decrypt(&c, iv, ct, pt, 32);
        CHECK_MEM(pt, in, 32);
        /* in-place */
        memcpy(ct, in, 32);
        aes128_cbc_encrypt(&c, iv, ct, ct, 32);
        CHECK_MEM(ct, ctexp, 32);
        aes128_cbc_decrypt(&c, iv, ct, ct, 32);
        CHECK_MEM(ct, in, 32);
    }
}

static void test_cmac_rfc4493(void)
{
    aes128_t c;
    uint8_t k[16], m[40], tag[16], exp[16];
    hex2bin("2b7e151628aed2a6abf7158809cf4f3c", k, 16);
    hex2bin("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411", m, 40);
    aes128_init(&c, k);
    aes128_cmac(&c, m, 0, tag);
    hex2bin("bb1d6929e95937287fa37d129b756746", exp, 16);
    CHECK_MEM(tag, exp, 16);
    aes128_cmac(&c, m, 16, tag);
    hex2bin("070a16b46b4d4144f79bdd9dd04a287c", exp, 16);
    CHECK_MEM(tag, exp, 16);
    aes128_cmac(&c, m, 40, tag);
    hex2bin("dfa66747de9ae63030ca32611497c827", exp, 16);
    CHECK_MEM(tag, exp, 16);
    /* two-piece form equals the one-piece form at every split */
    {
        size_t s;
        for (s = 0; s <= 40; s++) {
            aes128_cmac2(&c, m, s, m + s, 40 - s, tag);
            CHECK_MEM(tag, exp, 16);
        }
    }
}

/* Builds the INITIALIZE UPDATE response the card would send for our fixed challenges. */
static size_t make_iu_rsp(uint8_t *out, const char *cg_hex)
{
    size_t i;
    for (i = 0; i < 13; i++) out[i] = (uint8_t)(0xD0 + i); /* key div data + key info: opaque */
    hex2bin(CARD_HEX, out + 13, 8);
    hex2bin(cg_hex, out + 21, 8);
    out[29] = 0x90;
    out[30] = 0x00;
    return 31;
}

static int open_channel(scp03_t *s)
{
    uint8_t enc[16], mac[16], host[8], iu[64], ext[SCP03_EXT_AUTH_APDU_LEN], exp[SCP03_EXT_AUTH_APDU_LEN];
    size_t n = make_iu_rsp(iu, CARD_CG_HEX);
    hex2bin(ENC_HEX, enc, 16);
    hex2bin(MAC_HEX, mac, 16);
    hex2bin(HOST_HEX, host, 8);
    scp03_init(s);
    if (scp03_handshake(s, enc, mac, host, iu, n, ext) != 0) return -1;
    hex2bin(EXT_APDU_HEX, exp, sizeof exp);
    CHECK_MEM(ext, exp, sizeof exp);
    hex2bin(MCV0_HEX, exp, 16);
    CHECK_MEM(s->mcv, exp, 16);
    CHECK(!s->open);
    return scp03_finish(s, 0x9000);
}

static void test_handshake(void)
{
    scp03_t s;
    uint8_t apdu[SCP03_INIT_UPDATE_APDU_LEN], host[8];
    hex2bin(HOST_HEX, host, 8);
    CHECK_EQ(scp03_build_initialize_update(host, SCP03_KEY_VERSION_SE050, apdu), 13);
    CHECK(apdu[0] == 0x80 && apdu[1] == 0x50 && apdu[2] == 0x0B && apdu[3] == 0x00 && apdu[4] == 0x08);
    CHECK_MEM(apdu + 5, host, 8);
    CHECK_EQ(open_channel(&s), 0);
    CHECK(s.open);
    CHECK_EQ(s.counter[15], 1);
    /* wrong card cryptogram -> refused, nothing kept */
    {
        uint8_t enc[16], mac[16], iu[64], ext[SCP03_EXT_AUTH_APDU_LEN];
        size_t n = make_iu_rsp(iu, "8adcf126cd267e78");
        hex2bin(ENC_HEX, enc, 16);
        hex2bin(MAC_HEX, mac, 16);
        scp03_init(&s);
        CHECK_EQ(scp03_handshake(&s, enc, mac, host, iu, n, ext), -1);
        CHECK(!s.open);
        /* bad SW / bad length */
        n = make_iu_rsp(iu, CARD_CG_HEX);
        iu[29] = 0x6A;
        CHECK_EQ(scp03_handshake(&s, enc, mac, host, iu, n, ext), -1);
        iu[29] = 0x90;
        CHECK_EQ(scp03_handshake(&s, enc, mac, host, iu, n - 1, ext), -1);
        /* EXTERNAL AUTHENTICATE rejected by the card */
        CHECK_EQ(scp03_handshake(&s, enc, mac, host, iu, n, ext), 0);
        CHECK_EQ(scp03_finish(&s, 0x6982), -1);
        CHECK(!s.open);
    }
    /* nothing works on a closed channel */
    {
        uint8_t hdr[4] = {0x80, 0x02, 0, 0}, out[64];
        size_t n;
        CHECK_EQ(scp03_wrap(&s, hdr, NULL, 0, false, out, sizeof out, &n), -1);
    }
}

static void test_wrap_unwrap_trace(void)
{
    scp03_t s;
    uint8_t out[128], exp[128], rsp[128];
    size_t n, rn;
    CHECK_EQ(open_channel(&s), 0);
    /* C1: data-less command, Le absent */
    {
        uint8_t hdr[4] = {0x80, 0x02, 0x00, 0x00};
        CHECK_EQ(scp03_wrap(&s, hdr, NULL, 0, false, out, sizeof out, &n), 0);
        CHECK_EQ(n, 13);
        hex2bin(C1_HEX, exp, 13);
        CHECK_MEM(out, exp, 13);
    }
    /* R1: 5 plain bytes, decrypted with counter-1 (data-less command), counter does not move */
    hex2bin(R1_HEX, rsp, 26);
    rn = 26;
    CHECK_EQ(scp03_unwrap(&s, 0, rsp, &rn), 0);
    CHECK_EQ(rn, 7);
    CHECK_MEM(rsp, "ABCDE\x90\x00", 7);
    CHECK_EQ(s.counter[15], 1);
    /* C2: 20 data bytes, Le present -> 3-byte Lc */
    {
        uint8_t hdr[4] = {0x80, 0x01, 0x02, 0x03}, data[20];
        int i;
        for (i = 0; i < 20; i++) data[i] = (uint8_t)i;
        CHECK_EQ(scp03_wrap(&s, hdr, data, 20, true, out, sizeof out, &n), 0);
        CHECK_EQ(n, 4 + 3 + 32 + 8 + 2);
        hex2bin(C2_HEX, exp, n);
        CHECK_MEM(out, exp, n);
    }
    /* R2: 17 plain bytes -> 32 encrypted; counter advances to 2; MCV as predicted */
    hex2bin(R2_HEX, rsp, 42);
    rn = 42;
    CHECK_EQ(scp03_unwrap(&s, 20, rsp, &rn), 0);
    CHECK_EQ(rn, 19);
    hex2bin("0102030405060708090a0b0c0d0e0f10119000", exp, 19);
    CHECK_MEM(rsp, exp, 19);
    CHECK_EQ(s.counter[15], 2);
    hex2bin(MCV2_HEX, exp, 16);
    CHECK_MEM(s.mcv, exp, 16);
    scp03_close(&s);
    CHECK(!s.open);
}

static void test_unwrap_rejects(void)
{
    scp03_t s;
    uint8_t rsp[64];
    size_t rn;
    CHECK_EQ(open_channel(&s), 0);
    {
        uint8_t hdr[4] = {0x80, 0x02, 0x00, 0x00}, out[64];
        size_t n;
        CHECK_EQ(scp03_wrap(&s, hdr, NULL, 0, false, out, sizeof out, &n), 0);
    }
    /* flipped MAC bit */
    hex2bin(R1_HEX, rsp, 26);
    rsp[20] ^= 0x01;
    rn = 26;
    CHECK_EQ(scp03_unwrap(&s, 0, rsp, &rn), -1);
    /* flipped ciphertext bit (MAC covers it) */
    hex2bin(R1_HEX, rsp, 26);
    rsp[3] ^= 0x01;
    rn = 26;
    CHECK_EQ(scp03_unwrap(&s, 0, rsp, &rn), -1);
    /* 9000 without a MAC is not accepted (fail closed) */
    rsp[0] = 0x90; rsp[1] = 0x00;
    rn = 2;
    CHECK_EQ(scp03_unwrap(&s, 0, rsp, &rn), -1);
    /* error SW: passed through, counter advanced because the plain command length was not 8 */
    rsp[0] = 0x69; rsp[1] = 0x85;
    rn = 2;
    CHECK_EQ(scp03_unwrap(&s, 0, rsp, &rn), 0);
    CHECK_EQ(rn, 2);
    CHECK_EQ(s.counter[15], 2);
    rsp[0] = 0x69; rsp[1] = 0x85;
    rn = 2;
    CHECK_EQ(scp03_unwrap(&s, 8, rsp, &rn), 0); /* ...but not when it was 8 */
    CHECK_EQ(s.counter[15], 2);
}

static void test_se051_counting(void)
{
    scp03_t s;
    uint8_t rsp[4];
    size_t rn;
    CHECK_EQ(open_channel(&s), 0);
    scp03_set_applet_version(&s, 0x04030000);
    CHECK(s.count_every_cmd);
    rsp[0] = 0x69; rsp[1] = 0x85;
    rn = 2;
    CHECK_EQ(scp03_unwrap(&s, 8, rsp, &rn), 0);
    CHECK_EQ(s.counter[15], 2);
    scp03_set_applet_version(&s, 0x03010000);
    CHECK(!s.count_every_cmd);
}

static void test_counter_carry(void)
{
    scp03_t s;
    uint8_t rsp[4];
    size_t rn;
    CHECK_EQ(open_channel(&s), 0);
    s.counter[15] = 0xFF;
    rsp[0] = 0x6A; rsp[1] = 0x80;
    rn = 2;
    CHECK_EQ(scp03_unwrap(&s, 1, rsp, &rn), 0);
    CHECK_EQ(s.counter[15], 0x00);
    CHECK_EQ(s.counter[14], 0x01);
}

int main(void)
{
    RUN(test_aes_fips197);
    RUN(test_cmac_rfc4493);
    RUN(test_handshake);
    RUN(test_wrap_unwrap_trace);
    RUN(test_unwrap_rejects);
    RUN(test_se051_counting);
    RUN(test_counter_carry);
    TEST_END();
}
