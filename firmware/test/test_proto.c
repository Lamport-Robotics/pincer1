#include "proto.h"

#include "chainaddr.h"
#include "harness.h"
#include "util.h"

static void test_framer(void)
{
    proto_framer_t f;
    const char *line = NULL;
    const char *in = "{\"t\":\"hello\"}\r\n\n{\"t\":\"lock\"}\n";
    int got = 0;
    size_t i;
    proto_framer_init(&f);
    for (i = 0; i < strlen(in); i++) {
        if (proto_framer_push(&f, in[i], &line)) {
            got++;
            if (got == 1) CHECK_STR(line, "{\"t\":\"hello\"}");
            if (got == 2) CHECK_STR(line, "{\"t\":\"lock\"}");
        }
    }
    CHECK_EQ(got, 2); /* empty line produced nothing */
    /* over-long line is dropped, following line still parses */
    for (i = 0; i < PROTO_LINE_MAX + 10; i++) CHECK(!proto_framer_push(&f, 'x', &line));
    CHECK(!proto_framer_push(&f, '\n', &line));
    CHECK(!proto_framer_push(&f, 'o', &line));
    CHECK(!proto_framer_push(&f, 'k', &line));
    CHECK(proto_framer_push(&f, '\n', &line));
    CHECK_STR(line, "ok");
}

static void test_parse_requests(void)
{
    proto_req_t r;
    CHECK_EQ(proto_parse_request("{\"t\":\"hello\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_HELLO);
    CHECK_EQ(proto_parse_request("{\"t\":\"time\",\"unix\":1791600000}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_TIME);
    CHECK_U64(r.unix_time, 1791600000ull);
    CHECK_EQ(proto_parse_request("{\"t\":\"init\",\"id\":\"i1\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_INIT); CHECK_STR(r.id, "i1");
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e1\",\"limit_lamports\":\"20000000000\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_ELEVATE); CHECK_U64(r.limit_lamports, 20000000000ull);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p1\",\"daily_limit_lamports\":\"5000000000\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_POLICY); CHECK_U64(r.limit_lamports, 5000000000ull);
    CHECK_EQ(proto_parse_request("{\"t\":\"lock\"}", &r), PROTO_OK); CHECK_EQ(r.type, PROTO_REQ_LOCK);
    CHECK_EQ(proto_parse_request("{\"t\":\"unlock\",\"id\":\"u1\"}", &r), PROTO_OK); CHECK_EQ(r.type, PROTO_REQ_UNLOCK);
    CHECK_EQ(proto_parse_request("{\"t\":\"duress\",\"id\":\"d1\"}", &r), PROTO_OK); CHECK_EQ(r.type, PROTO_REQ_DURESS);
    CHECK_EQ(proto_parse_request("{\"t\":\"set_pin\",\"id\":\"n1\"}", &r), PROTO_OK); CHECK_EQ(r.type, PROTO_REQ_SET_PIN); CHECK_STR(r.id, "n1");
    /* errors */
    CHECK_EQ(proto_parse_request("not json", &r), PROTO_E_JSON);
    CHECK_EQ(proto_parse_request("{\"t\":\"nope\"}", &r), PROTO_E_TYPE);
    CHECK_EQ(proto_parse_request("{\"t\":\"time\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"limit_lamports\":\"abc\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"limit_lamports\":-5}", &r), PROTO_E_FIELD);
}

static void test_parse_sign(void)
{
    proto_req_t r;
    const char *line =
        "{\"t\":\"sign\",\"id\":\"s7\",\"kind\":\"tx\",\"bytes\":\"AQIDBA==\","
        "\"claims\":{\"lamports_out\":\"500000000\",\"token_out\":[{\"mint\":\"TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA\",\"amount\":\"1000000\",\"dest\":\"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4\"}],"
        "\"dest\":\"11111111111111111111111111111111\",\"programs\":[\"11111111111111111111111111111111\"],\"dangerous\":[\"lookup_table\",\"SetAuthority\"]},"
        "\"ttl_ms\":60000}";
    CHECK_EQ(proto_parse_request(line, &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_SIGN);
    CHECK_STR(r.id, "s7");
    CHECK(!r.sign.is_msg);
    CHECK_EQ(r.sign.len, 4);
    CHECK_EQ(r.sign.bytes[0], 1); CHECK_EQ(r.sign.bytes[3], 4);
    CHECK_U64(r.sign.claims.lamports_out, 500000000ull);
    CHECK_EQ(r.sign.claims.n_token_out, 1);
    CHECK_U64(r.sign.claims.token_out[0].amount, 1000000ull);
    CHECK_EQ(r.sign.claims.token_out[0].mint[0], 0x06);
    CHECK(r.sign.claims.token_out[0].has_dest); CHECK_EQ(r.sign.claims.token_out[0].dest[0], 0x04);
    CHECK(r.sign.claims.has_dest);
    CHECK_EQ(r.sign.claims.n_programs, 1);
    CHECK_EQ(r.sign.claims.dangerous, SOLMSG_DANGER_LOOKUP_TABLE | SOLMSG_DANGER_SET_AUTHORITY);
    CHECK_EQ(r.sign.ttl_ms, 60000);
    /* msg kind: claims optional */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"m1\",\"kind\":\"msg\",\"bytes\":\"aGVsbG8=\"}", &r), PROTO_OK);
    CHECK(r.sign.is_msg); CHECK_EQ(r.sign.len, 5); CHECK_MEM(r.sign.bytes, "hello", 5);
    CHECK_EQ(r.sign.ttl_ms, 0);
    /* tx without claims -> field error; bad base64 -> encoding; bad kind -> field */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"@@@@\",\"claims\":{}}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"blob\",\"bytes\":\"AQID\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\",\"claims\":{\"dangerous\":[\"foo\"]}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\",\"claims\":{\"programs\":[\"bad\"]}}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\",\"claims\":{\"token_out\":[{\"mint\":\"11111111111111111111111111111111\",\"amount\":\"1\",\"dest\":\"bad\"}]}}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\",\"claims\":{\"dangerous\":[\"unknown_program\"]}}", &r), PROTO_OK);
    CHECK_EQ(r.sign.claims.dangerous, SOLMSG_DANGER_UNKNOWN_PROGRAM);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"tx\",\"bytes\":\"AQID\",\"claims\":{\"token_out\":[{\"mint\":\"11111111111111111111111111111111\",\"amount\":\"1\"}]}}", &r), PROTO_OK);
    CHECK(!r.sign.claims.token_out[0].has_dest); /* dest is optional at the wire level; policy demands it for transfers */
    /* ttl clamp */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"msg\",\"bytes\":\"AQID\",\"ttl_ms\":999999999}", &r), PROTO_OK);
    CHECK_EQ(r.sign.ttl_ms, CONSENT_MAX_TTL_MS);
    /* oversized payload (PROTO_MAX_BYTES is 4096 now; the Solana parser enforces its own 1232 bound) */
    {
        static char big[9000];
        static char b64[7000];
        static uint8_t raw[5000];
        memset(raw, 7, sizeof raw);
        base64_encode(raw, sizeof raw, b64, sizeof b64);
        snprintf(big, sizeof big, "{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"msg\",\"bytes\":\"%s\"}", b64);
        CHECK_EQ(proto_parse_request(big, &r), PROTO_E_TOOBIG);
        base64_encode(raw, 4096, b64, sizeof b64);
        snprintf(big, sizeof big, "{\"t\":\"sign\",\"id\":\"s1\",\"kind\":\"msg\",\"bytes\":\"%s\"}", b64);
        CHECK_EQ(proto_parse_request(big, &r), PROTO_OK);
        CHECK_EQ(r.sign.len, 4096);
    }
}

static void test_parse_allowance(void)
{
    proto_req_t r;
    CHECK_EQ(proto_parse_request("{\"t\":\"allowance\",\"id\":\"a1\",\"lamports\":\"1000000000\",\"programs\":[\"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4\"],\"until_unix\":1791650000,\"max_per_tx_lamports\":\"200000000\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_ALLOWANCE);
    CHECK(!r.allowance.revoke);
    CHECK_U64(r.allowance.a.lamports, 1000000000ull);
    CHECK_U64(r.allowance.a.max_per_tx, 200000000ull);
    CHECK_U64(r.allowance.a.until_unix, 1791650000ull);
    CHECK_EQ(r.allowance.a.n_programs, 1);
    CHECK_EQ(r.allowance.a.programs[0][0], 0x04);
    CHECK_EQ(proto_parse_request("{\"t\":\"allowance\",\"id\":\"a2\",\"revoke\":true}", &r), PROTO_OK);
    CHECK(r.allowance.revoke);
    CHECK_EQ(proto_parse_request("{\"t\":\"allowance\",\"id\":\"a3\",\"lamports\":\"1\",\"until_unix\":5,\"max_per_tx_lamports\":\"1\"}", &r), PROTO_E_FIELD); /* no programs */
}

static void test_format(void)
{
    char out[1024];
    uint8_t key[32] = {0};
    consent_allowance_t a;
    proto_hello_t h;
    int n;
    memset(&h, 0, sizeof h);
    h.fw = PROTO_FW_VERSION; h.se050 = true; h.initialized = true; h.pubkey = key; h.daily_limit = 2000000000ull; h.day = 20395; h.pin_set = true;
    n = proto_fmt_hello(out, sizeof out, &h);
    CHECK(n > 0 && out[n - 1] == '\n');
    CHECK_STR(out, "{\"t\":\"hello\",\"fw\":\"0.4.0\",\"se050\":true,\"initialized\":true,\"pubkey\":\"11111111111111111111111111111111\","
                   "\"addresses\":{\"solana\":\"11111111111111111111111111111111\",\"evm\":null,\"bitcoin\":null},\"locked\":false,"
                   "\"policy\":{\"daily_limit_lamports\":\"2000000000\",\"spent_today_lamports\":\"0\",\"day\":20395,"
                   "\"daily_limit_wei\":\"0\",\"spent_today_wei\":\"0\",\"daily_limit_sats\":\"0\",\"spent_today_sats\":\"0\"},"
                   "\"allowance\":null,\"pin_set\":true,\"duress_pin_set\":false,\"pin_failures\":0,\"inheritance\":{\"armed\":false,\"inactivity_days\":0,\"days_since_touch\":0.0},"
                   "\"display\":false,\"routers\":{\"builtin\":[{\"chain\":\"solana\",\"address\":\"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4\"},"
                   "{\"chain\":\"evm\",\"address\":\"0x111111125421cA6dc452d289314280a0f8842A65\"},{\"chain\":\"evm\",\"address\":\"0xDef1C0ded9bec7F1a1670819833240f027b25EfF\"}],\"owner\":[]}}\n");
    /* display present + an owner-added router */
    {
        router_list_t rl;
        uint8_t thor[20];
        memset(&rl, 0, sizeof rl); memset(thor, 0x7c, 20);
        routers_add(&rl, ROUTER_CHAIN_EVM, thor);
        h.display = true; h.routers = &rl;
        CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
        CHECK(strstr(out, "\"display\":true,\"routers\":{") != NULL);
        {
            char hex[EVM_ADDR_HEX_LEN], want[128];
            evm_address_to_hex(thor, hex);
            snprintf(want, sizeof want, "\"owner\":[{\"chain\":\"evm\",\"address\":\"%s\"}]}}\n", hex);
            CHECK(strstr(out, want) != NULL);
        }
        h.display = false; h.routers = NULL;
    }
    /* with the secp256k1 addresses and the per-chain limits */
    {
        uint8_t evm[20], pkh[20];
        u256_t lim, spent;
        hex2bin("9858effd232b4033e47d90003d41ec34ecaeda94", evm, 20);
        hex2bin("c0cebcd6c3d3ca8c75dc5ec62ebe55330ef910e2", pkh, 20); /* hash160 of the BIP-84 vector key */
        u256_from_dec("2000000000000000000", &lim); u256_from_dec("500000000000000000", &spent);
        h.evm_addr = evm; h.btc_pkh = pkh; h.daily_limit_wei = &lim; h.spent_today_wei = &spent; h.daily_limit_sats = 300000; h.spent_today_sats = 1500;
        CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
        CHECK(strstr(out, "\"addresses\":{\"solana\":\"11111111111111111111111111111111\",\"evm\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"bitcoin\":\"bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu\"}") != NULL);
        CHECK(strstr(out, "\"daily_limit_wei\":\"2000000000000000000\",\"spent_today_wei\":\"500000000000000000\",\"daily_limit_sats\":\"300000\",\"spent_today_sats\":\"1500\"}") != NULL);
        h.evm_addr = NULL; h.btc_pkh = NULL; h.daily_limit_wei = NULL; h.spent_today_wei = NULL; h.daily_limit_sats = 0; h.spent_today_sats = 0;
    }
    h.pin_set = false; h.pin_failures = 3; h.dev_keys = true;
    CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
    CHECK(strstr(out, "\"pin_set\":false,\"duress_pin_set\":false,\"pin_failures\":3,\"dev_keys\":true,\"inheritance\":{") != NULL);
    /* init: pubkey + addresses + decoy addresses (once, here), NEVER a mnemonic */
    {
        proto_addrs_t real = {key, NULL, NULL};
        CHECK(proto_fmt_init(out, sizeof out, "i1", &real, NULL) > 0);
        CHECK_STR(out, "{\"t\":\"init\",\"id\":\"i1\",\"pubkey\":\"11111111111111111111111111111111\",\"addresses\":{\"solana\":\"11111111111111111111111111111111\",\"evm\":null,\"bitcoin\":null},\"decoy_addresses\":null}\n");
        CHECK(strstr(out, "mnemonic") == NULL);
    }
    {
        uint8_t evm[20], pkh[20], dkey[32];
        proto_addrs_t real, decoy;
        hex2bin("9858effd232b4033e47d90003d41ec34ecaeda94", evm, 20);
        hex2bin("c0cebcd6c3d3ca8c75dc5ec62ebe55330ef910e2", pkh, 20);
        memset(dkey, 0x01, 32);
        real.pubkey = key; real.evm_addr = evm; real.btc_pkh = pkh;
        decoy.pubkey = dkey; decoy.evm_addr = evm; decoy.btc_pkh = NULL;
        CHECK(proto_fmt_init(out, sizeof out, "i1", &real, &decoy) > 0);
        CHECK(strstr(out, "\"addresses\":{\"solana\":\"11111111111111111111111111111111\",\"evm\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"bitcoin\":\"bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu\"},") != NULL);
        CHECK(strstr(out, "\"decoy_addresses\":{\"solana\":\"4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi\",\"evm\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"bitcoin\":null}}\n") != NULL);
        CHECK(proto_fmt_decoy_addresses(out, sizeof out, "x2", &decoy) > 0);
        CHECK_STR(out, "{\"t\":\"decoy_addresses\",\"id\":\"x2\",\"addresses\":{\"solana\":\"4vJ9JU1bJJE96FWSJKvHsmmFADCg4gpZQff4P3bkLKi\",\"evm\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"bitcoin\":null}}\n");
    }
    /* hello says whether a decoy rhythm EXISTS (same for every owner who set one) but never whether duress is
     * active: the decoy slot must be indistinguishable over the wire */
    h.duress_pin_set = true;
    CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
    CHECK(strstr(out, "\"duress_pin_set\":true") != NULL);
    CHECK(strstr(out, "\"duress\":") == NULL);
    CHECK(strstr(out, "decoy") == NULL);
    h.duress_pin_set = false;
    /* set_duress_pin / decoy_addresses requests */
    {
        proto_req_t r;
        CHECK_EQ(proto_parse_request("{\"t\":\"set_duress_pin\",\"id\":\"d1\"}", &r), PROTO_OK);
        CHECK_EQ(r.type, PROTO_REQ_SET_DURESS_PIN); CHECK_STR(r.id, "d1");
        CHECK_EQ(proto_parse_request("{\"t\":\"decoy_addresses\",\"id\":\"x1\"}", &r), PROTO_OK);
        CHECK_EQ(r.type, PROTO_REQ_DECOY_ADDRESSES); CHECK_STR(r.id, "x1");
    }
    CHECK(proto_fmt_ok(out, sizeof out, "set_duress_pin", "d1") > 0);
    CHECK_STR(out, "{\"t\":\"set_duress_pin\",\"id\":\"d1\",\"ok\":true}\n");
    /* reveal_seed parses (so the firmware can refuse it by name) */
    {
        proto_req_t r;
        CHECK_EQ(proto_parse_request("{\"t\":\"reveal_seed\",\"id\":\"x1\"}", &r), PROTO_OK);
        CHECK_EQ(r.type, PROTO_REQ_REVEAL_SEED); CHECK_STR(r.id, "x1");
    }
    CHECK(proto_fmt_denied(out, sizeof out, "x1", "not_supported") > 0);
    CHECK_STR(out, "{\"t\":\"denied\",\"id\":\"x1\",\"reason\":\"not_supported\"}\n");
    CHECK(proto_fmt_event_display(out, sizeof out, 2, 3) > 0);
    CHECK_STR(out, "{\"t\":\"event\",\"ev\":\"display\",\"page\":2,\"pages\":3}\n");
    h.pin_set = true; h.pin_failures = 0; h.dev_keys = false;
    memset(&a, 0, sizeof a);
    a.active = true; a.lamports = 10; a.spent = 3; a.max_per_tx = 5; a.until_unix = 1791650000ull; a.n_programs = 1;
    h.allowance = &a; h.pubkey = NULL; h.initialized = false; h.se050 = false;
    CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
    CHECK(strstr(out, "\"se050\":false") != NULL);
    CHECK(strstr(out, "\"pubkey\":null,\"addresses\":null") != NULL);
    CHECK(strstr(out, "\"allowance\":{\"lamports\":\"10\",\"spent_lamports\":\"3\",\"max_per_tx_lamports\":\"5\",\"until_unix\":1791650000,\"programs\":[\"11111111111111111111111111111111\"]}") != NULL);

    {
        solmsg_summary_t p;
        memset(&p, 0, sizeof p);
        p.lamports_out = 500000000ull; p.has_dest = true; p.dangerous = SOLMSG_DANGER_APPROVE;
        CHECK(proto_fmt_pending(out, sizeof out, "s7", "touch", &p) > 0);
        CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"s7\",\"need\":\"touch\",\"parsed\":{\"lamports_out\":\"500000000\",\"dest\":\"11111111111111111111111111111111\",\"dangerous\":[\"approve\"]}}\n");
        p.n_token_out = 1; p.token_out[0].has_mint = true; p.token_out[0].has_dest = true; p.token_out[0].amount = 7;
        CHECK(proto_fmt_pending_sol(out, sizeof out, "s8", "touch", &p, SOLMSG_DANGER_APPROVE | SOLMSG_DANGER_UNKNOWN_PROGRAM) > 0);
        CHECK(strstr(out, "\"token_out\":[{\"mint\":\"11111111111111111111111111111111\",\"dest\":\"11111111111111111111111111111111\",\"amount\":\"7\"}],\"dangerous\":[\"approve\",\"unknown_program\"]}}\n") != NULL);
        CHECK(proto_fmt_pending(out, sizeof out, "i1", "long_touch", NULL) > 0);
        CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"i1\",\"need\":\"long_touch\"}\n");
        CHECK(proto_fmt_pending(out, sizeof out, "u1", "pin", NULL) > 0);
        CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"u1\",\"need\":\"pin\"}\n");
        CHECK(proto_fmt_pending_confirm(out, sizeof out, "n1") > 0);
        CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"n1\",\"need\":\"pin\",\"confirm\":true}\n");
    }
    {
        uint8_t sig[64];
        memset(sig, 0xAB, 64);
        CHECK(proto_fmt_signed(out, sizeof out, "s7", sig) > 0);
        CHECK(strstr(out, "\"sig\":\"q6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urqw==\"") != NULL);
    }
    CHECK(proto_fmt_denied(out, sizeof out, "s7", "timeout") > 0);
    CHECK_STR(out, "{\"t\":\"denied\",\"id\":\"s7\",\"reason\":\"timeout\"}\n");
    CHECK(proto_fmt_elevated(out, sizeof out, "e1", 120000) > 0);
    CHECK_STR(out, "{\"t\":\"elevated\",\"id\":\"e1\",\"until_ms\":120000}\n");
    CHECK(proto_fmt_ok(out, sizeof out, "policy", "p1") > 0);
    CHECK_STR(out, "{\"t\":\"policy\",\"id\":\"p1\",\"ok\":true}\n");
    CHECK(proto_fmt_simple(out, sizeof out, "locked", NULL) > 0);
    CHECK_STR(out, "{\"t\":\"locked\"}\n");
    CHECK(proto_fmt_simple(out, sizeof out, "unlocked", "u1") > 0);
    CHECK_STR(out, "{\"t\":\"unlocked\",\"id\":\"u1\"}\n");
    CHECK(proto_fmt_event(out, sizeof out, "tamper", "usb_reset") > 0);
    CHECK_STR(out, "{\"t\":\"event\",\"ev\":\"tamper\",\"what\":\"usb_reset\"}\n");
    CHECK(proto_fmt_event(out, sizeof out, "touch", NULL) > 0);
    CHECK_STR(out, "{\"t\":\"event\",\"ev\":\"touch\"}\n");
    /* ids are escaped, so a hostile id cannot break framing */
    CHECK(proto_fmt_denied(out, sizeof out, "x\"\n", "policy") > 0);
    CHECK_STR(out, "{\"t\":\"denied\",\"id\":\"x\\\"\\n\",\"reason\":\"policy\"}\n");
    CHECK(proto_fmt_denied(out, 10, "s7", "timeout") < 0);
}

static void test_parse_multichain_sign(void)
{
    proto_req_t r;
    char dec[U256_DEC_MAX];
    /* evm_tx with the full claims shape */
    CHECK_EQ(proto_parse_request(
                 "{\"t\":\"sign\",\"id\":\"e1\",\"kind\":\"evm_tx\",\"bytes\":\"AQIDBA==\",\"claims\":{\"chain_id\":1,"
                 "\"to\":\"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\",\"value_wei\":\"500000000000000000\","
                 "\"token_out\":[{\"token\":\"0xa0b86991c6218b36c1d19d4a2e9eb0ce3606eb48\",\"amount\":\"1000000\",\"to\":\"0x2222222222222222222222222222222222222222\"}],"
                 "\"max_fee_wei\":\"0x1dcd6500\",\"routers\":[\"0x1111111254EEB25477B68fb85Ed929f73A960582\"],"
                 "\"dangerous\":[\"approve_unlimited\",\"unknown_contract\"]},\"ttl_ms\":30000}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_SIGN);
    CHECK_EQ(r.sign.kind, PROTO_KIND_EVM_TX);
    CHECK(!r.sign.is_msg);
    CHECK_EQ(r.sign.len, 4);
    CHECK_U64(r.sign.evm_claims.chain_id, 1);
    CHECK(r.sign.evm_claims.has_to);
    CHECK_EQ(r.sign.evm_claims.to[0], 0xa0); CHECK_EQ(r.sign.evm_claims.to[19], 0x48);
    u256_to_dec(&r.sign.evm_claims.value_wei, dec, sizeof dec); CHECK_STR(dec, "500000000000000000");
    CHECK_EQ(r.sign.evm_claims.n_token_out, 1);
    CHECK_EQ(r.sign.evm_claims.token_out[0].token[19], 0x48);
    CHECK_U64(u256_low64(&r.sign.evm_claims.token_out[0].amount), 1000000);
    CHECK_U64(u256_low64(&r.sign.evm_claims.max_fee_wei), 500000000ull);
    CHECK(r.sign.evm_claims.token_out[0].has_to); CHECK_EQ(r.sign.evm_claims.token_out[0].to[0], 0x22);
    /* `routers` was accepted above and ignored: the controller keeps its own list */
    CHECK_EQ(r.sign.evm_claims.dangerous, EVMTX_DANGER_APPROVE_UNLIMITED | EVMTX_DANGER_UNKNOWN_CONTRACT);
    CHECK_EQ(r.sign.ttl_ms, 30000);
    /* without `to` (contract creation), minimal claims */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"e2\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\",\"claims\":{\"chain_id\":\"8453\"}}", &r), PROTO_OK);
    CHECK(!r.sign.evm_claims.has_to);
    CHECK_U64(r.sign.evm_claims.chain_id, 8453);
    CHECK(u256_is_zero(&r.sign.evm_claims.value_wei));
    /* bad address checksum, bad danger name, missing chain id, chain id 0 */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\",\"claims\":{\"chain_id\":1,\"to\":\"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD\"}}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\",\"claims\":{\"chain_id\":1,\"dangerous\":[\"lookup_table\"]}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\",\"claims\":{}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\",\"claims\":{\"chain_id\":0}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_tx\",\"bytes\":\"AQID\"}", &r), PROTO_E_FIELD);
    /* btc_psbt */
    CHECK_EQ(proto_parse_request(
                 "{\"t\":\"sign\",\"id\":\"b1\",\"kind\":\"btc_psbt\",\"bytes\":\"cHNidP8=\",\"claims\":{\"network\":\"mainnet\","
                 "\"sats_out\":\"150000\",\"dest\":\"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4\",\"fee_sats\":\"2100\",\"inputs\":2,"
                 "\"dangerous\":[\"high_fee\",\"sighash_not_all\"]}}", &r), PROTO_OK);
    CHECK_EQ(r.sign.kind, PROTO_KIND_BTC_PSBT);
    CHECK(r.sign.btc_claims.mainnet);
    CHECK_U64(r.sign.btc_claims.sats_out, 150000);
    CHECK_U64(r.sign.btc_claims.fee_sats, 2100);
    CHECK(r.sign.btc_claims.has_inputs); CHECK_EQ(r.sign.btc_claims.inputs, 2);
    CHECK(r.sign.btc_claims.has_dest); CHECK_EQ(r.sign.btc_claims.dest_spk_len, 22); CHECK_EQ(r.sign.btc_claims.dest_spk[0], 0x00);
    CHECK_EQ(r.sign.btc_claims.dangerous, BTCPSBT_DANGER_HIGH_FEE | BTCPSBT_DANGER_SIGHASH_NOT_ALL);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"btc_psbt\",\"bytes\":\"cHNidP8=\",\"claims\":{\"network\":\"testnet\"}}", &r), PROTO_OK);
    CHECK(!r.sign.btc_claims.mainnet);
    CHECK(!r.sign.btc_claims.has_inputs);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"btc_psbt\",\"bytes\":\"cHNidP8=\",\"claims\":{\"network\":\"mainnet\",\"dest\":\"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t5\"}}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"btc_psbt\",\"bytes\":\"cHNidP8=\",\"claims\":{\"sats_out\":\"1\"}}", &r), PROTO_E_FIELD); /* no network */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"btc_psbt\",\"bytes\":\"cHNidP8=\",\"claims\":{\"network\":\"mainnet\",\"inputs\":9}}", &r), PROTO_E_FIELD);
    /* evm_msg: EIP-191 bytes, or typed data + digest */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"m1\",\"kind\":\"evm_msg\",\"bytes\":\"aGVsbG8=\"}", &r), PROTO_OK);
    CHECK_EQ(r.sign.kind, PROTO_KIND_EVM_MSG); CHECK(!r.sign.typed.present); CHECK_EQ(r.sign.len, 5);
    CHECK_EQ(proto_parse_request(
                 "{\"t\":\"sign\",\"id\":\"m2\",\"kind\":\"evm_msg\",\"digest\":\"0x1df394b5f178d0a1acd782b55774602224b42ba7a6f7a5e0936ee2e21bd46d7b\","
                 "\"typed_data\":{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"},{\"name\":\"version\",\"type\":\"string\"},{\"name\":\"chainId\",\"type\":\"uint256\"},{\"name\":\"verifyingContract\",\"type\":\"address\"}],"
                 "\"Permit\":[{\"name\":\"owner\",\"type\":\"address\"},{\"name\":\"spender\",\"type\":\"address\"},{\"name\":\"value\",\"type\":\"uint256\"},{\"name\":\"nonce\",\"type\":\"uint256\"},{\"name\":\"deadline\",\"type\":\"uint256\"}]},"
                 "\"primaryType\":\"Permit\",\"domain\":{\"name\":\"USD Coin\",\"version\":\"2\",\"chainId\":1,\"verifyingContract\":\"0xA0b86991c6218b36c1d19D4a2e9Eb0cE3606eB48\"},"
                 "\"message\":{\"owner\":\"0x9858EfFD232B4033E47d90003D41EC34EcaEda94\",\"spender\":\"0x1111111254EEB25477B68fb85Ed929f73A960582\",\"value\":\"1000000\",\"nonce\":0,\"deadline\":1800000000}}}", &r), PROTO_OK);
    CHECK(r.sign.typed.present);
    CHECK_EQ(r.sign.typed.rc, 0);
    CHECK_EQ(r.sign.len, 0);
    CHECK_MEM(r.sign.typed.digest, r.sign.typed.claimed, 32);
    CHECK_EQ(r.sign.typed.digest[0], 0x1d);
    /* unsupported typed data is reported, not rejected at parse time (the handler answers unsupported_typed_data) */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_msg\",\"digest\":\"0x0000000000000000000000000000000000000000000000000000000000000000\","
                                 "\"typed_data\":{\"types\":{\"EIP712Domain\":[{\"name\":\"name\",\"type\":\"string\"}],\"T\":[{\"name\":\"a\",\"type\":\"uint256[]\"}]},"
                                 "\"primaryType\":\"T\",\"domain\":{\"name\":\"x\"},\"message\":{\"a\":[]}}}", &r), PROTO_OK);
    CHECK(r.sign.typed.present);
    CHECK(r.sign.typed.rc < 0);
    /* typed data needs a digest; bytes and typed data together are refused; a digest must be 32 bytes */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_msg\",\"typed_data\":{}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_msg\",\"bytes\":\"AQID\",\"digest\":\"0x0000000000000000000000000000000000000000000000000000000000000000\",\"typed_data\":{}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"kind\":\"evm_msg\",\"digest\":\"0x00\",\"typed_data\":{}}", &r), PROTO_E_FIELD);
    /* btc_msg */
    CHECK_EQ(proto_parse_request("{\"t\":\"sign\",\"id\":\"m3\",\"kind\":\"btc_msg\",\"bytes\":\"SGVsbG8gV29ybGQ=\"}", &r), PROTO_OK);
    CHECK_EQ(r.sign.kind, PROTO_KIND_BTC_MSG); CHECK_EQ(r.sign.len, 11);
    /* elevate per chain */
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e1\",\"limit_wei\":\"20000000000000000000\"}", &r), PROTO_OK);
    CHECK_EQ(r.elevate_chain, CONSENT_CHAIN_EVM);
    u256_to_dec(&r.limit_wei, dec, sizeof dec); CHECK_STR(dec, "20000000000000000000");
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e2\",\"limit_sats\":\"5000000\"}", &r), PROTO_OK);
    CHECK_EQ(r.elevate_chain, CONSENT_CHAIN_BTC); CHECK_U64(r.limit_lamports, 5000000);
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e3\",\"limit_lamports\":\"1\"}", &r), PROTO_OK);
    CHECK_EQ(r.elevate_chain, CONSENT_CHAIN_SOL);
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e4\",\"limit_lamports\":\"1\",\"limit_sats\":\"1\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"elevate\",\"id\":\"e5\"}", &r), PROTO_E_FIELD);
    /* policy per chain, any subset */
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p1\",\"daily_limit_wei\":\"1000000000000000000\",\"daily_limit_sats\":\"200000\"}", &r), PROTO_OK);
    CHECK(!r.policy.has_sol && r.policy.has_wei && r.policy.has_sats);
    CHECK_U64(r.policy.sats, 200000);
    CHECK_U64(u256_low64(&r.policy.wei), 1000000000000000000ull);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p2\",\"daily_limit_lamports\":\"5\"}", &r), PROTO_OK);
    CHECK(r.policy.has_sol && !r.policy.has_wei); CHECK_U64(r.policy.sol, 5); CHECK_U64(r.limit_lamports, 5);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p3\"}", &r), PROTO_E_FIELD);
    /* router allowlist changes (PIN gated, applied by consent.c) */
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p5\",\"add_router\":{\"chain\":\"evm\",\"address\":\"0xDef1C0ded9bec7F1a1670819833240f027b25EfF\"}}", &r), PROTO_OK);
    CHECK(r.policy.has_router_add && !r.policy.has_router_remove && !r.policy.has_sol);
    CHECK_EQ(r.policy.router.chain, ROUTER_CHAIN_EVM); CHECK_EQ(r.policy.router.addr[0], 0xde); CHECK_EQ(r.policy.router.addr[20], 0);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p6\",\"remove_router\":{\"chain\":\"solana\",\"address\":\"JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4\"},\"daily_limit_sats\":\"1\"}", &r), PROTO_OK);
    CHECK(r.policy.has_router_remove && r.policy.has_sats);
    CHECK_EQ(r.policy.router.chain, ROUTER_CHAIN_SOL); CHECK_EQ(r.policy.router.addr[0], 0x04);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p7\",\"add_router\":{\"chain\":\"evm\",\"address\":\"0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAeD\"}}", &r), PROTO_E_ENCODING); /* bad checksum */
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p8\",\"add_router\":{\"chain\":\"btc\",\"address\":\"x\"}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p9\",\"add_router\":{\"chain\":\"evm\",\"address\":\"0xDef1C0ded9bec7F1a1670819833240f027b25EfF\"},\"remove_router\":{\"chain\":\"evm\",\"address\":\"0xDef1C0ded9bec7F1a1670819833240f027b25EfF\"}}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"policy\",\"id\":\"p4\",\"daily_limit_wei\":\"-1\"}", &r), PROTO_E_FIELD);
}

static void test_format_multichain(void)
{
    static char out[PROTO_LINE_MAX];
    evmtx_t tx;
    btcpsbt_t p;
    uint8_t sig[65], psbt[4900]; /* <= BTCPSBT_MAX_OUT_BYTES */
    memset(&tx, 0, sizeof tx);
    tx.type = 2; tx.chain_id = 8453; tx.nonce = 3; tx.has_to = true; memset(tx.to, 0x22, 20);
    u256_from_dec("500000000000000000", &tx.value); u256_from_u64(&tx.max_fee_total, 42000000000000ull);
    tx.call = EVMTX_CALL_NONE;
    CHECK(proto_fmt_pending_evm(out, sizeof out, "e1", "touch", &tx, 0) > 0);
    CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"e1\",\"need\":\"touch\",\"parsed\":{\"chain_id\":8453,\"nonce\":3,\"to\":\"0x2222222222222222222222222222222222222222\","
                   "\"value_wei\":\"500000000000000000\",\"max_fee_wei\":\"42000000000000\",\"call\":\"transfer_native\"}}\n");
    tx.call = EVMTX_CALL_TRANSFER; memset(tx.call_to, 0x11, 20); u256_from_u64(&tx.call_amount, 1000000);
    CHECK(proto_fmt_pending_evm(out, sizeof out, "e2", "touch", &tx, 0) > 0);
    CHECK(strstr(out, "\"call\":\"erc20_transfer\",\"token_out\":[{\"token\":\"0x2222222222222222222222222222222222222222\",\"to\":\"0x1111111111111111111111111111111111111111\",\"amount\":\"1000000\"}]}}") != NULL);
    tx.call = EVMTX_CALL_APPROVE; u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &tx.call_amount);
    CHECK(proto_fmt_pending_evm(out, sizeof out, "e3", "touch", &tx, EVMTX_DANGER_APPROVE_UNLIMITED | EVMTX_DANGER_UNKNOWN_CONTRACT) > 0);
    CHECK(strstr(out, "\"call\":\"approve\",\"spender\":\"0x1111111111111111111111111111111111111111\",\"allowance\":\"115792089237316195423570985008687907853269984665640564039457584007913129639935\",\"dangerous\":[\"approve_unlimited\",\"unknown_contract\"]}}") != NULL);
    tx.has_to = false; tx.call = EVMTX_CALL_CREATE;
    CHECK(proto_fmt_pending_evm(out, sizeof out, "e4", "touch", &tx, EVMTX_DANGER_UNKNOWN_CONTRACT) > 0);
    CHECK(strstr(out, "\"to\":null") != NULL && strstr(out, "\"call\":\"contract_create\"") != NULL);
    memset(&p, 0, sizeof p);
    p.sats_out = 50000; p.fee = 2000; p.sats_change = 48000; p.n_owner_inputs = 2; p.n_out = 2; p.has_dest = true;
    hex2bin("0014751e76e8199196d454941c45d1b3a323f1433bd6", p.dest_spk, 22); p.dest_spk_len = 22;
    CHECK(proto_fmt_pending_btc(out, sizeof out, "b1", "touch", &p, BTCPSBT_DANGER_HIGH_FEE) > 0);
    CHECK_STR(out, "{\"t\":\"pending\",\"id\":\"b1\",\"need\":\"touch\",\"parsed\":{\"sats_out\":\"50000\",\"fee_sats\":\"2000\",\"change_sats\":\"48000\","
                   "\"dest\":\"bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4\",\"inputs\":2,\"outputs\":2,\"dangerous\":[\"high_fee\"]}}\n");
    p.has_dest = false;
    CHECK(proto_fmt_pending_btc(out, sizeof out, "b2", "touch", &p, 0) > 0);
    CHECK(strstr(out, "\"dest\":null,\"inputs\":2,\"outputs\":2}}") != NULL);
    memset(sig, 0xAB, 65); sig[64] = 28;
    CHECK(proto_fmt_signed_bytes(out, sizeof out, "e1", sig, 65) > 0);
    CHECK_STR(out, "{\"t\":\"signed\",\"id\":\"e1\",\"sig\":\"q6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urqxw=\"}\n");
    memset(psbt, 0x55, sizeof psbt);
    CHECK(proto_fmt_signed_psbt(out, sizeof out, "b1", psbt, sizeof psbt) > 0);
    CHECK(strncmp(out, "{\"t\":\"signed\",\"id\":\"b1\",\"psbt\":\"VVVV", 32) == 0);
    CHECK(strlen(out) > 6500);
    CHECK(proto_fmt_denied(out, sizeof out, "m1", "unsupported_typed_data") > 0);
    CHECK_STR(out, "{\"t\":\"denied\",\"id\":\"m1\",\"reason\":\"unsupported_typed_data\"}\n");
}

int main(void)
{
    RUN(test_framer);
    RUN(test_parse_requests);
    RUN(test_parse_sign);
    RUN(test_parse_allowance);
    RUN(test_format);
    RUN(test_parse_multichain_sign);
    RUN(test_format_multichain);
    TEST_END();
}
