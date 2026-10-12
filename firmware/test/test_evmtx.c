#include "evmtx.h"

#include "cJSON.h"
#include "chainaddr.h"
#include "eip712.h"
#include "harness.h"
#include "u256.h"

/* Vector sources: EIP-155 example (bytes + signing hash as published); the other transactions were built
 * with an independent Python RLP encoder and hashed with pycryptodome's keccak (scratch script, values
 * inlined here); EIP-712 domain separator for the "Ether Mail" example is the value published in the EIP. */

static uint8_t OWNER[20];

static int parse_hex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex) / 2;
    if (n > cap) return -1;
    hex2bin(hex, out, n);
    return (int)n;
}

static void test_eip155_example(void)
{
    uint8_t b[128], d[32], exp[32], sig[64] = {0}, out[65];
    evmtx_t tx;
    int n = parse_hex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080", b, sizeof b);
    char dec[U256_DEC_MAX];
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK_EQ(tx.type, 0);
    CHECK_U64(tx.nonce, 9);
    CHECK_U64(tx.gas_limit, 21000);
    CHECK_U64(tx.chain_id, 1);
    CHECK(u256_fits_u64(&tx.max_fee_per_gas));
    CHECK_U64(u256_low64(&tx.max_fee_per_gas), 20000000000ull);
    CHECK(tx.has_to);
    CHECK_EQ(tx.to[0], 0x35); CHECK_EQ(tx.to[19], 0x35);
    CHECK(u256_to_dec(&tx.value, dec, sizeof dec) > 0);
    CHECK_STR(dec, "1000000000000000000");
    CHECK_EQ(tx.data_len, 0);
    evmtx_signing_digest(b, (size_t)n, d);
    hex2bin("daf5a779ae972f972197303d7b574746c7ef83eadac0f2791ad23db92e4c8e53", exp, 32);
    CHECK_MEM(d, exp, 32);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_NONE);
    CHECK_EQ(tx.dangerous, 0);
    CHECK_U64(u256_low64(&tx.max_fee_total), 420000000000000ull);
    evmtx_encode_signature(&tx, sig, 1, out);
    CHECK_EQ(out[64], 28);
    evmtx_encode_signature(&tx, sig, 0, out);
    CHECK_EQ(out[64], 27);
    CHECK(evmtx_looks_like_tx(b, (size_t)n));
    CHECK(!evmtx_looks_like_tx((const uint8_t *)"hello", 5));
}

static void test_legacy_erc20_transfer(void)
{
    uint8_t b[160], d[32], exp[32];
    evmtx_t tx;
    int n = parse_hex("f869078506fc23ac0082fde894a0b86991c6218b36c1d19d4a2e9eb0ce3606eb4880b844a9059cbb0000000000000000000000001111111111111111111111111111111111111111"
                      "00000000000000000000000000000000000000000000000000000000000f4240018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK_U64(tx.nonce, 7);
    CHECK_U64(tx.gas_limit, 65000);
    CHECK(u256_is_zero(&tx.value));
    CHECK_EQ(tx.data_len, 68);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_TRANSFER);
    CHECK_EQ(tx.call_to[0], 0x11); CHECK_EQ(tx.call_to[19], 0x11);
    CHECK_U64(u256_low64(&tx.call_amount), 1000000);
    CHECK_EQ(tx.dangerous, 0);
    hex2bin("a0b86991c6218b36c1d19d4a2e9eb0ce3606eb48", exp, 20);
    CHECK_MEM(tx.to, exp, 20);
    evmtx_signing_digest(b, (size_t)n, d);
    hex2bin("d448461ec7637a263121cea034531ff4adb2b65380685c76a4901e5882bb5754", exp, 32);
    CHECK_MEM(d, exp, 32);
}

static void test_type2(void)
{
    uint8_t b[256], d[32], exp[32], sig[64] = {0}, out[65];
    evmtx_t tx;
    int n = parse_hex("02f182210503843b9aca0084773594008252089422222222222222222222222222222222222222228806f05b59d3b2000080c0", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK_EQ(tx.type, 2);
    CHECK_U64(tx.chain_id, 8453);
    CHECK_U64(tx.nonce, 3);
    CHECK_U64(u256_low64(&tx.max_priority_fee), 1000000000ull);
    CHECK_U64(u256_low64(&tx.max_fee_per_gas), 2000000000ull);
    CHECK_U64(tx.gas_limit, 21000);
    CHECK_U64(u256_low64(&tx.value), 500000000000000000ull);
    CHECK_EQ(tx.data_len, 0);
    CHECK_EQ(tx.access_list_len, 0);
    evmtx_signing_digest(b, (size_t)n, d);
    hex2bin("64334dd641d90c1a429e15e5f2820b4fbb5e9224eca5f2bc71e358e89b476e9a", exp, 32);
    CHECK_MEM(d, exp, 32);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_NONE);
    CHECK_U64(u256_low64(&tx.max_fee_total), 42000000000000ull);
    evmtx_encode_signature(&tx, sig, 1, out);
    CHECK_EQ(out[64], 1); /* typed: v = recid */
    /* unlimited approve with a non-empty access list */
    n = parse_hex("02f8a60180843b9aca00850ba43b740082ea6094444444444444444444444444444444444444444480b844095ea7b30000000000000000000000003333333333333333333333333333333333333333"
                  "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff838f7944444444444444444444444444444444444444444e1a00101010101010101010101010101010101010101010101010101010101010101", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK(tx.access_list_len > 0);
    CHECK_U64(tx.nonce, 0);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_APPROVE);
    CHECK_EQ(tx.dangerous, EVMTX_DANGER_APPROVE_UNLIMITED);
    CHECK(u256_is_max(&tx.call_amount));
    CHECK_EQ(tx.call_to[0], 0x33);
    evmtx_signing_digest(b, (size_t)n, d);
    hex2bin("d8da9c0fab63ee2e4dd18bcde1f26e5c72346faddb9cc17069a1744ac4daf4a6", exp, 32);
    CHECK_MEM(d, exp, 32);
}

static void test_calldata_kinds(void)
{
    uint8_t b[256];
    evmtx_t tx;
    int n;
    /* transferFrom(from = owner, to = 0x55.., 42): an outflow of the owner's tokens */
    n = parse_hex("f88901843b9aca00830186a094444444444444444444444444444444444444444480b86423b872dd0000000000000000000000009858effd232b4033e47d90003d41ec34ecaeda94"
                  "0000000000000000000000005555555555555555555555555555555555555555000000000000000000000000000000000000000000000000000000000000002a018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_TRANSFER_FROM);
    CHECK_MEM(tx.call_from, OWNER, 20);
    CHECK_EQ(tx.call_to[0], 0x55);
    CHECK_U64(u256_low64(&tx.call_amount), 42);
    CHECK_EQ(tx.dangerous, 0);
    /* setApprovalForAll(0x66.., true) */
    n = parse_hex("f86901843b9aca00830186a094444444444444444444444444444444444444444480b844a22cb4650000000000000000000000006666666666666666666666666666666666666666"
                  "0000000000000000000000000000000000000000000000000000000000000001018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_SET_APPROVAL);
    CHECK(tx.call_flag);
    CHECK_EQ(tx.dangerous, EVMTX_DANGER_SET_APPROVAL_FOR_ALL);
    b[n - 4] = 0; /* approved = false: harmless */
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_SET_APPROVAL);
    CHECK(!tx.call_flag);
    CHECK_EQ(tx.dangerous, 0);
    b[n - 4] = 2; /* not a bool: unrecognised call */
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_UNKNOWN);
    /* increaseAllowance(0x77.., 5) counts as approve */
    n = parse_hex("f86901843b9aca00830186a094444444444444444444444444444444444444444480b844395093510000000000000000000000007777777777777777777777777777777777777777"
                  "0000000000000000000000000000000000000000000000000000000000000005018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_APPROVE);
    CHECK_EQ(tx.dangerous, EVMTX_DANGER_APPROVE);
    CHECK_U64(u256_low64(&tx.call_amount), 5);
    /* a transfer with a truncated argument (67 data bytes) is NOT recognised as a transfer */
    n = parse_hex("f868078506fc23ac0082fde894a0b86991c6218b36c1d19d4a2e9eb0ce3606eb4880b843a9059cbb0000000000000000000000001111111111111111111111111111111111111111"
                  "00000000000000000000000000000000000000000000000000000000000f42018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_UNKNOWN);
    CHECK_EQ(tx.data_len, 67);
    /* unknown call with long data, large nonce, chain 137 */
    n = parse_hex("f89b86010000000000843b9aca00830493e0948888888888888888888888888888888888888888872386f26fc10000b86812"
                  "345678aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa81898080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK_U64(tx.nonce, 1ull << 40);
    CHECK_U64(tx.chain_id, 137);
    CHECK_EQ(tx.data_len, 104);
    CHECK_U64(u256_low64(&tx.value), 10000000000000000ull);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_UNKNOWN);
    CHECK_EQ(tx.dangerous, 0); /* unknown_contract is decided by policy (routers) */
    /* contract creation */
    n = parse_hex("d280843b9aca00830493e08080826080018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_OK);
    CHECK(!tx.has_to);
    evmtx_analyze(&tx, OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_CREATE);
}

static void test_rejections(void)
{
    uint8_t b[128];
    evmtx_t tx;
    int n;
    /* pre-EIP-155 (6 fields): no replay protection */
    n = parse_hex("e9098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_FIELDS);
    CHECK(!evmtx_looks_like_tx(b, (size_t)n));
    /* type 1 and unknown types */
    n = parse_hex("01c0", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_TYPE);
    n = parse_hex("03c0", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_TYPE);
    /* a string, not a list */
    n = parse_hex("83010203", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RLP);
    /* trailing byte */
    n = parse_hex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a76400008001808000", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RLP);
    /* truncated */
    n = parse_hex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a7640000800180", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RLP);
    /* non-canonical: nonce encoded as 0x8109 (single byte must be literal) */
    n = parse_hex("ed81098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RLP);
    /* leading zero in an integer */
    n = parse_hex("ee8200098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RANGE);
    /* chain id 0 */
    n = parse_hex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080808080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RANGE);
    /* r/s placeholders must be empty */
    n = parse_hex("ec098504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080010180", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_FIELDS);
    /* `to` of 19 bytes */
    n = parse_hex("eb0985" "04a817c800" "825208" "93" "35353535353535353535353535353535353535" "880de0b6b3a764000080018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_FIELDS);
    /* nonce wider than 64 bits */
    n = parse_hex("f5890100000000000000008504a817c800825208943535353535353535353535353535353535353535880de0b6b3a764000080018080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RANGE);
    /* type 2 with priority fee above max fee */
    n = parse_hex("02f182210503847735940084" "3b9aca00" "8252089422222222222222222222222222222222222222228806f05b59d3b2000080c0", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_RANGE);
    /* type 2 with 8 fields */
    n = parse_hex("02f082210503843b9aca008477359400825208942222222222222222222222222222222222222222" "8806f05b59d3b2000080", b, sizeof b);
    CHECK_EQ(evmtx_parse(b, (size_t)n, &tx), EVMTX_E_FIELDS);
    CHECK_EQ(evmtx_parse(b, 0, &tx), EVMTX_E_SIZE);
}

static void test_eip191(void)
{
    uint8_t d[32], exp[32], m[123];
    evmtx_personal_digest((const uint8_t *)"hello", 5, d);
    hex2bin("50b2c43fd39106bafbba0da34fc430e1f91e3c96ea2acee2bc34119f92b37750", exp, 32);
    CHECK_MEM(d, exp, 32);
    memset(m, 'x', sizeof m);
    evmtx_personal_digest(m, sizeof m, d);
    hex2bin("d328e808456f28fe16b137f9a182b9e4de2ea2b92f699f6b5bba57064c31fc9e", exp, 32);
    CHECK_MEM(d, exp, 32);
}

static int digest_of(const char *json, uint8_t out[32])
{
    cJSON *td = cJSON_Parse(json);
    int rc;
    if (!td) return -99;
    rc = eip712_digest(td, out);
    cJSON_Delete(td);
    return rc;
}

static void test_eip712(void)
{
    uint8_t d[32], exp[32];
    /* domain separator of the EIP-712 "Ether Mail" example (published value) */
    {
        cJSON *td = cJSON_Parse("{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"version\",\"type\":\"string\"},"
                                "{\"name\":\"chainId\",\"type\":\"uint256\"},{\"name\":\"verifyingContract\",\"type\":\"address\"}]},"
                                "\"domain\":{\"name\":\"Ether Mail\",\"version\":\"1\",\"chainId\":1,\"verifyingContract\":\"0xCcCCccccCCCCcCCCCCCcCcCccCcCCCcCcccccccC\"}}");
        CHECK(td != NULL);
        CHECK_EQ(eip712_hash_struct(cJSON_GetObjectItemCaseSensitive(td, "types"), "EIP712Domain",
                                    cJSON_GetObjectItemCaseSensitive(td, "domain"), d), EIP712_OK);
        hex2bin("f2cee375fa42b42143804025fc449deafd50cc031ca257e0b194a650a912090f", exp, 32);
        CHECK_MEM(d, exp, 32);
        cJSON_Delete(td);
    }
    /* ERC-2612 permit: all primitives */
    {
        const char *permit =
            "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"version\",\"type\":\"string\"},{\"name\":\"chainId\",\"type\":\"uint256\"},{\"name\":\"verifyingContract\",\"type\":\"address\"}],"
            "\"Permit\":[{\"name\":\"owner\",\"type\":\"address\"},{\"name\":\"spender\",\"type\":\"address\"},{\"name\":\"value\",\"type\":\"uint256\"},{\"name\":\"nonce\",\"type\":\"uint256\"},{\"name\":\"deadline\",\"type\":\"uint256\"}]},"
            "\"primaryType\":\"Permit\",\"domain\":{\"name\":\"USD Coin\",\"version\":\"2\",\"chainId\":1,\"verifyingContract\":\"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\"},"
            "\"message\":{\"owner\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"spender\":\"0x1111111254EEB25477B68fb85Ed929f73A960582\",\"value\":\"1000000\",\"nonce\":0,\"deadline\":\"0x6b49d200\"}}";
        CHECK_EQ(digest_of(permit, d), EIP712_OK);
        hex2bin("1df394b5f178d0a1acd782b55774602224b42ba7a6f7a5e0936ee2e21bd46d7b", exp, 32);
        CHECK_MEM(d, exp, 32);
    }
    /* every primitive kind, negative int, bytes, bytes32, string with UTF-8, uint8 */
    {
        const char *mixed =
            "{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"chainId\",\"type\":\"uint256\"}],"
            "\"Mixed\":[{\"name\":\"flag\",\"type\":\"bool\"},{\"name\":\"amount\",\"type\":\"int256\"},{\"name\":\"tag\",\"type\":\"bytes32\"},{\"name\":\"blob\",\"type\":\"bytes\"},"
            "{\"name\":\"who\",\"type\":\"address\"},{\"name\":\"note\",\"type\":\"string\"},{\"name\":\"small\",\"type\":\"uint8\"}]},"
            "\"primaryType\":\"Mixed\",\"domain\":{\"name\":\"Mixed\",\"chainId\":\"8453\"},"
            "\"message\":{\"flag\":true,\"amount\":\"-5\",\"tag\":\"0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20\",\"blob\":\"0xdeadbeef\","
            "\"who\":\"0x0000000000000000000000000000000000000001\",\"note\":\"h\\u00e9llo\",\"small\":255}}";
        CHECK_EQ(digest_of(mixed, d), EIP712_OK);
        hex2bin("b5b55cbc68cafe65271b2833a9650fa9f753bd55cf6589197077176f9e99987b", exp, 32);
        CHECK_MEM(d, exp, 32);
    }
    /* refusals */
    {
        const char *dom = "\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"}]";
        char buf[1024];
        /* nested struct */
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"Mail\":[{\"name\":\"from\",\"type\":\"Person\"}],\"Person\":[{\"name\":\"name\",\"type\":\"string\"}]},"
                 "\"primaryType\":\"Mail\",\"domain\":{\"name\":\"x\"},\"message\":{\"from\":{\"name\":\"Bob\"}}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_UNSUPPORTED);
        /* array */
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"uint256[]\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":[1]}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_UNSUPPORTED);
        /* unknown type */
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"uint257\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":1}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_UNSUPPORTED);
        /* value does not fit: uint8 = 256, negative uint, bytes32 with 31 bytes, bool as number, missing field */
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"uint8\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":256}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"uint8\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":255}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_OK);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"uint256\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":\"-1\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"int8\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":\"-128\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_OK);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"int8\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":\"128\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"bytes32\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":\"0x00\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"bool\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":1}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"bool\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        /* address must be valid (EIP-55 if mixed case) */
        snprintf(buf, sizeof buf, "{\"types\":{%s,\"T\":[{\"name\":\"a\",\"type\":\"address\"}]},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":\"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        /* primaryType may not be the domain; missing sections */
        snprintf(buf, sizeof buf, "{\"types\":{%s},\"primaryType\":\"EIP712Domain\",\"domain\":{\"name\":\"x\"},\"message\":{\"name\":\"x\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        snprintf(buf, sizeof buf, "{\"types\":{%s},\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"}}", dom);
        CHECK_EQ(digest_of(buf, d), EIP712_E_MALFORMED);
        CHECK_EQ(digest_of("[]", d), EIP712_E_MALFORMED);
    }
}

static void test_danger_names(void)
{
    uint32_t bit;
    for (bit = 1; bit <= EVMTX_DANGER_ALL; bit <<= 1) {
        const char *n = evmtx_danger_name(bit);
        CHECK(n != NULL);
        CHECK_EQ(evmtx_danger_from_name(n), bit);
    }
    CHECK_EQ(evmtx_danger_from_name("delegatecall"), 0);
    CHECK_EQ(evmtx_danger_from_name(NULL), 0);
    CHECK_STR(evmtx_danger_name(EVMTX_DANGER_APPROVE_UNLIMITED), "approve_unlimited");
}

int main(void)
{
    hex2bin("9858effd232b4033e47d90003d41ec34ecaeda94", OWNER, 20);
    RUN(test_eip155_example);
    RUN(test_legacy_erc20_transfer);
    RUN(test_type2);
    RUN(test_calldata_kinds);
    RUN(test_rejections);
    RUN(test_eip191);
    RUN(test_eip712);
    RUN(test_danger_names);
    TEST_END();
}
