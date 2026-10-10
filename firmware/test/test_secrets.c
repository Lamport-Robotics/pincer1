/* secrets.c on the host: there is no OTP, so the dev fallback (-DPINCER_DEV_SCP03_KEYS) is the only
 * source. The CMake target compiles this test with the key string below. */
#include "secrets.h"

#include "harness.h"

#define DEV_HEX "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f"

static void test_parse(void)
{
    scp03_keys_t k;
    int i;
    CHECK(secrets_parse_hex_keys(DEV_HEX, &k));
    for (i = 0; i < 16; i++) {
        CHECK_EQ(k.enc[i], i);
        CHECK_EQ(k.mac[i], 0x10 + i);
        CHECK_EQ(k.dek[i], 0x20 + i);
    }
    CHECK(secrets_parse_hex_keys("ABCDEFabcdef0123456789ABCDEFabcdef0123456789ABCDEFabcdef0123456789ABCDEFabcdef0123456789ABCDEFab", &k));
    CHECK_EQ(k.enc[0], 0xAB);
    CHECK(!secrets_parse_hex_keys(NULL, &k));
    CHECK(!secrets_parse_hex_keys("", &k));
    CHECK(!secrets_parse_hex_keys(DEV_HEX "0", &k));     /* 97 chars */
    CHECK(!secrets_parse_hex_keys(&DEV_HEX[2], &k));     /* 94 chars */
    {
        char bad[97];
        memcpy(bad, DEV_HEX, 97);
        bad[10] = 'g';
        CHECK(!secrets_parse_hex_keys(bad, &k));
        bad[10] = ' ';
        CHECK(!secrets_parse_hex_keys(bad, &k));
    }
}

static void test_dev_fallback(void)
{
    scp03_keys_t k, exp;
    memset(&k, 0xAA, sizeof k);
#ifdef PINCER_DEV_SCP03_KEYS
    CHECK_EQ(secrets_get_scp03_keys(&k), SECRETS_DEV);
    CHECK(secrets_parse_hex_keys(PINCER_DEV_SCP03_KEYS, &exp));
    CHECK_MEM(&k, &exp, sizeof k);
    CHECK_STR(PINCER_DEV_SCP03_KEYS, DEV_HEX);
#else
    (void)exp;
    CHECK_EQ(secrets_get_scp03_keys(&k), SECRETS_NONE);
    {
        uint8_t zero[sizeof k];
        memset(zero, 0, sizeof zero);
        CHECK_MEM(&k, zero, sizeof k); /* no keys -> zeroed, never stale */
    }
#endif
}

int main(void)
{
    RUN(test_parse);
    RUN(test_dev_fallback);
    TEST_END();
}
