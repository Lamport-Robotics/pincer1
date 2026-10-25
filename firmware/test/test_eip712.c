#include "eip712.h"

#include "harness.h"

static const char PERMIT[] =
    "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"version\",\"type\":\"string\"},{\"name\":\"chainId\",\"type\":\"uint256\"},{\"name\":\"verifyingContract\",\"type\":\"address\"}],"
    "\"Permit\":[{\"name\":\"owner\",\"type\":\"address\"},{\"name\":\"spender\",\"type\":\"address\"},{\"name\":\"value\",\"type\":\"uint256\"},{\"name\":\"nonce\",\"type\":\"uint256\"},{\"name\":\"deadline\",\"type\":\"uint256\"}]},"
    "\"primaryType\":\"Permit\",\"domain\":{\"name\":\"USD Coin\",\"version\":\"2\",\"chainId\":1,\"verifyingContract\":\"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\"},"
    "\"message\":{\"owner\":\"0x1111111111111111111111111111111111111111\",\"spender\":\"0x2222222222222222222222222222222222222222\","
    "\"value\":\"115792089237316195423570985008687907853269984665640564039457584007913129639935\",\"nonce\":7,\"deadline\":\"1800000000\"}}";

static const char MAIL[] =
    "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"chainId\",\"type\":\"uint256\"}],"
    "\"Mail\":[{\"name\":\"to\",\"type\":\"address\"},{\"name\":\"contents\",\"type\":\"string\"},{\"name\":\"urgent\",\"type\":\"bool\"}]},"
    "\"primaryType\":\"Mail\",\"domain\":{\"name\":\"Ether Mail\",\"chainId\":8453},"
    "\"message\":{\"to\":\"0x3333333333333333333333333333333333333333\",\"contents\":\"Hello, Bob!\",\"urgent\":true}}";

static void test_describe_permit(void)
{
    cJSON *td = cJSON_Parse(PERMIT);
    eip712_desc_t d;
    uint8_t digest[32];
    CHECK(td != NULL);
    CHECK_EQ(eip712_digest(td, digest), EIP712_OK);
    CHECK_EQ(eip712_describe(td, &d), EIP712_OK);
    CHECK_STR(d.primary, "Permit");
    CHECK_STR(d.domain_name, "USD Coin");
    CHECK_STR(d.chain_id, "1");
    CHECK(d.has_contract);
    CHECK_EQ(d.contract[0], 0xa0); CHECK_EQ(d.contract[19], 0x48);
    CHECK(d.permit_like);
    CHECK_EQ(d.n_fields, 5);
    CHECK(!d.fields_truncated);
    CHECK_STR(d.f[1].name, "spender");
    CHECK_STR(d.f[1].value, "0x2222222222222222222222222222222222222222");
    CHECK_STR(d.f[2].name, "value");
    CHECK_STR(d.f[2].value, "UNLIMITED"); /* 2^256-1: the word every permit means by it */
    CHECK_STR(d.f[4].value, "1800000000");
    cJSON_Delete(td);
}

static void test_describe_mail(void)
{
    cJSON *td = cJSON_Parse(MAIL);
    eip712_desc_t d;
    CHECK(td != NULL);
    CHECK_EQ(eip712_describe(td, &d), EIP712_OK);
    CHECK_STR(d.primary, "Mail");
    CHECK_STR(d.chain_id, "8453");
    CHECK(!d.has_contract);
    CHECK(!d.permit_like);
    CHECK_EQ(d.n_fields, 3);
    CHECK_STR(d.f[1].value, "Hello, Bob!");
    CHECK_STR(d.f[2].value, "true");
    cJSON_Delete(td);
}

static void test_describe_big_number(void)
{
    cJSON *td = cJSON_Parse(
        "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"}],\"Big\":[{\"name\":\"amount\",\"type\":\"uint256\"},{\"name\":\"debt\",\"type\":\"int256\"}]},"
        "\"primaryType\":\"Big\",\"domain\":{\"name\":\"X\"},\"message\":{\"amount\":\"12345678901234567890123456789012345678901234567890\",\"debt\":\"-5\"}}");
    eip712_desc_t d;
    CHECK(td != NULL);
    CHECK_EQ(eip712_describe(td, &d), EIP712_OK);
    CHECK(!d.fields_truncated);
    CHECK_STR(d.f[0].value, "1.2345e49"); /* 50 digits: magnitude, not digits */
    CHECK_STR(d.f[1].value, "-5");
    cJSON_Delete(td);
}

static void test_describe_operator_is_permit_like(void)
{
    cJSON *td = cJSON_Parse(
        "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"}],\"Grant\":[{\"name\":\"operator\",\"type\":\"address\"},{\"name\":\"ok\",\"type\":\"bool\"}]},"
        "\"primaryType\":\"Grant\",\"domain\":{\"name\":\"X\"},\"message\":{\"operator\":\"0x4444444444444444444444444444444444444444\",\"ok\":true}}");
    eip712_desc_t d;
    CHECK(td != NULL);
    CHECK_EQ(eip712_describe(td, &d), EIP712_OK);
    CHECK(d.permit_like);
    cJSON_Delete(td);
}

int main(void)
{
    RUN(test_describe_permit);
    RUN(test_describe_mail);
    RUN(test_describe_big_number);
    RUN(test_describe_operator_is_permit_like);
    TEST_END();
}
