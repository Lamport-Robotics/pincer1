#include "util.h"

#include <stdint.h>

#include "harness.h"

static void test_base58(void)
{
    uint8_t key[32];
    char s[48];
    uint8_t back[32];
    /* System program id = 32 zero bytes */
    memset(key, 0, 32);
    CHECK_EQ(pubkey_to_b58(key, s, sizeof s), 32);
    CHECK_STR(s, "11111111111111111111111111111111");
    CHECK(pubkey_from_b58("11111111111111111111111111111111", back));
    CHECK_MEM(back, key, 32);
    /* Token program */
    CHECK(pubkey_from_b58("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA", back));
    CHECK_EQ(back[0], 0x06); CHECK_EQ(back[1], 0xdd); CHECK_EQ(back[31], 0xa9);
    CHECK_EQ(pubkey_to_b58(back, s, sizeof s), 43);
    CHECK_STR(s, "TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA");
    /* generic round trip incl. leading zeros */
    {
        uint8_t in[5] = {0, 0, 1, 2, 3};
        uint8_t out[5];
        int n = base58_encode(in, 5, s, sizeof s);
        CHECK(n > 2 && s[0] == '1' && s[1] == '1');
        CHECK_EQ(base58_decode(s, (size_t)n, out, sizeof out), 5);
        CHECK_MEM(in, out, 5);
    }
    CHECK(!pubkey_from_b58("0OIl", back));          /* invalid alphabet chars */
    CHECK(!pubkey_from_b58("TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5D", back)); /* wrong length */
    CHECK(base58_encode(key, 32, s, 10) < 0);
}

static void test_base64(void)
{
    const uint8_t in[] = "Man";
    char s[16];
    uint8_t out[16];
    CHECK_EQ(base64_encode(in, 3, s, sizeof s), 4); CHECK_STR(s, "TWFu");
    CHECK_EQ(base64_encode(in, 2, s, sizeof s), 4); CHECK_STR(s, "TWE=");
    CHECK_EQ(base64_encode(in, 1, s, sizeof s), 4); CHECK_STR(s, "TQ==");
    CHECK_EQ(base64_decode("TWFu", 4, out, sizeof out), 3); CHECK_MEM(out, "Man", 3);
    CHECK_EQ(base64_decode("TWE=", 4, out, sizeof out), 2);
    CHECK_EQ(base64_decode("TQ==", 4, out, sizeof out), 1);
    CHECK_EQ(base64_decode("TQ", 2, out, sizeof out), 1);
    CHECK(base64_decode("T*==", 4, out, sizeof out) < 0);
    CHECK(base64_decode("TWFuTWFu", 8, out, 3) < 0);
    {
        uint8_t big[64], back[64];
        char b64[92];
        int i;
        for (i = 0; i < 64; i++) big[i] = (uint8_t)(i * 7);
        CHECK_EQ(base64_encode(big, 64, b64, sizeof b64), 88);
        CHECK_EQ(base64_decode(b64, 88, back, sizeof back), 64);
        CHECK_MEM(big, back, 64);
    }
}

static void test_u64(void)
{
    uint64_t v;
    char s[24];
    CHECK(u64_from_dec("0", &v) && v == 0);
    CHECK(u64_from_dec("500000000", &v) && v == 500000000ull);
    CHECK(u64_from_dec("18446744073709551615", &v) && v == UINT64_MAX);
    CHECK(!u64_from_dec("18446744073709551616", &v));
    CHECK(!u64_from_dec("", &v));
    CHECK(!u64_from_dec("12a", &v));
    CHECK(!u64_from_dec("-1", &v));
    CHECK_EQ(u64_to_dec(0, s, sizeof s), 1); CHECK_STR(s, "0");
    CHECK_EQ(u64_to_dec(UINT64_MAX, s, sizeof s), 20); CHECK_STR(s, "18446744073709551615");
    CHECK(u64_to_dec(12345, s, 3) < 0);
    CHECK_U64(u64_sat_add(UINT64_MAX - 1, 5), UINT64_MAX);
}

static void test_crc_and_le(void)
{
    uint8_t b[8];
    CHECK_EQ(crc32_buf((const uint8_t *)"123456789", 9), 0xCBF43926);
    CHECK_EQ(crc32_buf((const uint8_t *)"", 0), 0);
    wr_le64(b, 0x1122334455667788ull);
    CHECK_EQ(b[0], 0x88); CHECK_EQ(b[7], 0x11);
    CHECK_U64(rd_le64(b), 0x1122334455667788ull);
    CHECK_EQ(rd_le16(b), 0x7788); CHECK_EQ(rd_le32(b), 0x55667788);
    CHECK_EQ(ct_memcmp("abc", "abc", 3), 0);
    CHECK(ct_memcmp("abc", "abd", 3) != 0);
}

int main(void)
{
    RUN(test_base58);
    RUN(test_base64);
    RUN(test_u64);
    RUN(test_crc_and_le);
    TEST_END();
}
