#include "policy.h"

#include "harness.h"
#include "msgbuild.h"
#include "u256.h"
#include "chainaddr.h"

#define SOL 1000000000ull

static uint8_t OWNER[32], DEST[32], MINT[32], JUP[32];

static void base_parsed(solmsg_summary_t *s)
{
    memset(s, 0, sizeof *s);
    s->fee_payer_is_owner = true;
    s->lamports_out = SOL / 2;
    memcpy(s->programs[0], SOLMSG_SYSTEM_PROGRAM, 32);
    s->n_programs = 1;
    s->has_dest = true;
    memcpy(s->dest, DEST, 32);
}

static void base_claims(policy_claims_t *c)
{
    memset(c, 0, sizeof *c);
    c->lamports_out = SOL / 2;
    memcpy(c->programs[0], SOLMSG_SYSTEM_PROGRAM, 32);
    c->n_programs = 1;
    c->has_dest = true;
    memcpy(c->dest, DEST, 32);
}

#define CHK(s, c) policy_check((s), (c), NULL, NULL)

static void test_check1_fee_payer(void)
{
    solmsg_summary_t s; policy_claims_t c;
    base_parsed(&s); base_claims(&c);
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    s.fee_payer_is_owner = false;
    CHECK_EQ(CHK(&s, &c), POLICY_FEE_PAYER);
    CHECK_STR(policy_verdict_reason(POLICY_FEE_PAYER), "policy");
}

static void test_check2_lamports(void)
{
    solmsg_summary_t s; policy_claims_t c;
    base_parsed(&s); base_claims(&c);
    c.lamports_out = SOL; /* claiming more than the bytes move is fine */
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    c.lamports_out = SOL / 2 - 1; /* daemon said 0.4999, bytes say 0.5 */
    CHECK_EQ(CHK(&s, &c), POLICY_LAMPORTS);
    CHECK_STR(policy_verdict_reason(POLICY_LAMPORTS), "claims_mismatch");
    c.lamports_out = 0; s.lamports_out = 0;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
}

static void test_check2_tokens(void)
{
    solmsg_summary_t s; policy_claims_t c;
    base_parsed(&s); base_claims(&c);
    s.lamports_out = 0; c.lamports_out = 0;
    memcpy(s.programs[0], SOLMSG_TOKEN_PROGRAM, 32); memcpy(c.programs[0], SOLMSG_TOKEN_PROGRAM, 32);
    s.n_token_out = 1; s.token_out[0].has_mint = true; memcpy(s.token_out[0].mint, MINT, 32); s.token_out[0].amount = 1000000;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN); /* nothing claimed */
    c.n_token_out = 1; memcpy(c.token_out[0].mint, MINT, 32); c.token_out[0].amount = 1000000;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    c.token_out[0].amount = 999999;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    /* wrong mint claimed */
    c.token_out[0].amount = 1000000; memcpy(c.token_out[0].mint, JUP, 32);
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    memcpy(c.token_out[0].mint, MINT, 32);
    /* two checked transfers of the same mint are summed */
    s.n_token_out = 2; s.token_out[1] = s.token_out[0]; s.token_out[1].amount = 1;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    c.token_out[0].amount = 1000001;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    /* plain transfer without mint: bounded by the claimed total */
    s.n_token_out = 1; s.token_out[0].has_mint = false; s.token_out[0].amount = 1000001;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    s.token_out[0].amount = 1000002;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    /* mint-less + minted may not double-use the same claim */
    s.n_token_out = 2; s.token_out[0].amount = 1000001;
    s.token_out[1].has_mint = true; memcpy(s.token_out[1].mint, MINT, 32); s.token_out[1].amount = 1;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    s.n_token_out = 0; s.token_out_overflow = true;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
}

static void test_check3_programs(void)
{
    solmsg_summary_t s; policy_claims_t c;
    base_parsed(&s); base_claims(&c);
    memcpy(s.programs[1], JUP, 32); s.n_programs = 2;
    CHECK_EQ(CHK(&s, &c), POLICY_PROGRAM); /* JUP not claimed */
    memcpy(c.programs[1], JUP, 32); c.n_programs = 2;
    CHECK_EQ(CHK(&s, &c), POLICY_DANGEROUS); /* claimed, but neither core nor a router: unknown_program */
    c.dangerous = SOLMSG_DANGER_UNKNOWN_PROGRAM;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    memcpy(c.programs[2], SOLMSG_TOKEN_PROGRAM, 32); c.n_programs = 3; /* extra claims are harmless */
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    s.programs_overflow = true;
    CHECK_EQ(CHK(&s, &c), POLICY_PROGRAM);
    CHECK_STR(policy_verdict_reason(POLICY_PROGRAM), "claims_mismatch");
}

static void test_check4_dangerous(void)
{
    solmsg_summary_t s; policy_claims_t c;
    base_parsed(&s); base_claims(&c);
    s.dangerous = SOLMSG_DANGER_SET_AUTHORITY;
    CHECK_EQ(CHK(&s, &c), POLICY_DANGEROUS);
    CHECK_STR(policy_verdict_reason(POLICY_DANGEROUS), "policy");
    c.dangerous = SOLMSG_DANGER_APPROVE; /* declared the wrong one */
    CHECK_EQ(CHK(&s, &c), POLICY_DANGEROUS);
    c.dangerous = SOLMSG_DANGER_APPROVE | SOLMSG_DANGER_SET_AUTHORITY;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    s.dangerous = SOLMSG_DANGER_LOOKUP_TABLE; c.dangerous = 0;
    CHECK_EQ(CHK(&s, &c), POLICY_DANGEROUS);
    c.dangerous = SOLMSG_DANGER_LOOKUP_TABLE;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    s.dangerous = 0; c.dangerous = SOLMSG_DANGER_ALL; /* over-declaring is fine */
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
}

/* destination is compared, not only the amount */
static void test_check2_dest(void)
{
    solmsg_summary_t s; policy_claims_t c; uint32_t eff = 9;
    base_parsed(&s); base_claims(&c);
    CHECK_EQ(policy_check(&s, &c, NULL, &eff), POLICY_OK);
    CHECK_EQ(eff, 0);
    /* daemon names another recipient */
    c.dest[0] ^= 1;
    CHECK_EQ(CHK(&s, &c), POLICY_DEST);
    CHECK_STR(policy_verdict_reason(POLICY_DEST), "claims_mismatch");
    /* daemon names no recipient at all while lamports leave */
    c.dest[0] ^= 1; c.has_dest = false;
    CHECK_EQ(CHK(&s, &c), POLICY_DEST);
    /* nothing leaves: no destination needed */
    s.lamports_out = 0; c.lamports_out = 0; s.has_dest = false;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    /* the parse has no destination (should not happen for a transfer) but lamports leave: refuse */
    s.lamports_out = 5; c.lamports_out = 5; c.has_dest = true;
    CHECK_EQ(CHK(&s, &c), POLICY_DEST);
}

static void test_check2_token_dest(void)
{
    solmsg_summary_t s; policy_claims_t c;
    uint8_t ATA1[32], ATA2[32];
    fill_key(ATA1, 0x41); fill_key(ATA2, 0x42);
    base_parsed(&s); base_claims(&c);
    s.lamports_out = 0; c.lamports_out = 0; s.has_dest = true; memcpy(s.dest, ATA1, 32);
    memcpy(s.programs[0], SOLMSG_TOKEN_PROGRAM, 32); memcpy(c.programs[0], SOLMSG_TOKEN_PROGRAM, 32);
    s.n_token_out = 1; s.token_out[0].has_mint = true; memcpy(s.token_out[0].mint, MINT, 32); s.token_out[0].amount = 1000;
    s.token_out[0].has_dest = true; memcpy(s.token_out[0].dest, ATA1, 32);
    c.n_token_out = 1; memcpy(c.token_out[0].mint, MINT, 32); c.token_out[0].amount = 1000;
    /* claimed entry without a recipient does not cover a transfer */
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    c.token_out[0].has_dest = true; memcpy(c.token_out[0].dest, ATA1, 32);
    CHECK_EQ(CHK(&s, &c), POLICY_OK); /* matching */
    /* wrong recipient */
    memcpy(c.token_out[0].dest, ATA2, 32);
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    memcpy(c.token_out[0].dest, ATA1, 32);
    /* an extra transfer of the same mint to a second recipient the daemon did not name */
    s.n_token_out = 2; s.token_out[1] = s.token_out[0]; memcpy(s.token_out[1].dest, ATA2, 32); s.token_out[1].amount = 1;
    c.token_out[0].amount = 1001;
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    c.n_token_out = 2; c.token_out[1] = c.token_out[0]; memcpy(c.token_out[1].dest, ATA2, 32); c.token_out[1].amount = 1; c.token_out[0].amount = 1000;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    /* a burn has no recipient and needs none */
    s.n_token_out = 1; s.token_out[0].has_dest = false;
    c.n_token_out = 1; c.token_out[0].has_dest = false;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
    /* a plain transfer (mint unknown) still needs its recipient named by some entry */
    s.token_out[0].has_mint = false; s.token_out[0].has_dest = true; memcpy(s.token_out[0].dest, ATA2, 32);
    CHECK_EQ(CHK(&s, &c), POLICY_TOKEN);
    c.token_out[0].has_dest = true; memcpy(c.token_out[0].dest, ATA2, 32);
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
}

/* F5 (Solana side): a program outside the core set and the controller's router list is unknown_program. */
static void test_unknown_program_and_routers(void)
{
    solmsg_summary_t s; policy_claims_t c; uint32_t eff;
    router_list_t owner;
    uint8_t jup6[32], ata[32];
    char b58[48];
    size_t nb;
    const router_entry_t *b = routers_builtin(&nb);
    CHECK_EQ(nb, 3);
    /* the built-in Jupiter v6 program id re-encodes to the published string */
    CHECK(pubkey_from_b58("JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4", jup6));
    CHECK_MEM(b[0].addr, jup6, 32);
    pubkey_to_b58(b[0].addr, b58, sizeof b58);
    CHECK_STR(b58, "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4");
    CHECK(pubkey_from_b58("ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL", ata));
    CHECK(solmsg_is_core_program(ata));
    CHECK(solmsg_is_core_program(SOLMSG_SYSTEM_PROGRAM));
    CHECK(!solmsg_is_core_program(JUP));
    base_parsed(&s); base_claims(&c);
    memcpy(s.programs[1], jup6, 32); s.n_programs = 2;
    memcpy(c.programs[1], jup6, 32); c.n_programs = 2;
    CHECK_EQ(policy_check(&s, &c, NULL, &eff), POLICY_OK); /* Jupiter is built in */
    CHECK_EQ(eff, 0);
    memcpy(s.programs[1], JUP, 32); memcpy(c.programs[1], JUP, 32);
    CHECK_EQ(policy_check(&s, &c, NULL, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, SOLMSG_DANGER_UNKNOWN_PROGRAM);
    /* the owner adds it: allowed */
    memset(&owner, 0, sizeof owner);
    CHECK_EQ(routers_add(&owner, ROUTER_CHAIN_SOL, JUP), 0);
    CHECK_EQ(routers_add(&owner, ROUTER_CHAIN_SOL, JUP), 1); /* duplicate */
    CHECK_EQ(policy_check(&s, &c, &owner, &eff), POLICY_OK);
    CHECK_EQ(eff, 0);
    /* declared by the daemon: allowed but flagged (the display shows DANGER) */
    c.dangerous = SOLMSG_DANGER_UNKNOWN_PROGRAM;
    CHECK_EQ(policy_check(&s, &c, NULL, &eff), POLICY_OK);
    CHECK_EQ(eff, SOLMSG_DANGER_UNKNOWN_PROGRAM);
    CHECK_STR(solmsg_danger_name(SOLMSG_DANGER_UNKNOWN_PROGRAM), "unknown_program");
    CHECK_EQ(solmsg_danger_from_name("unknown_program"), SOLMSG_DANGER_UNKNOWN_PROGRAM);
}

/* End to end on real bytes: the daemon says 0.5 SOL, the bytes say 50 SOL. */
static void test_end_to_end_lie(void)
{
    mb_t m;
    uint8_t keys[3][32], d[64], accts[2] = {0, 1};
    solmsg_t p; solmsg_summary_t s; policy_claims_t c;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_sys_transfer(d, 50 * SOL));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    base_claims(&c); /* claims 0.5 SOL */
    CHECK_EQ(CHK(&s, &c), POLICY_LAMPORTS);
    c.lamports_out = 50 * SOL;
    CHECK_EQ(CHK(&s, &c), POLICY_OK);
}

/* ---- EVM ---- */
static uint8_t EVM_OWNER[20], TOKEN[20], ROUTER[20], EVM_DEST[20];

/* a parsed+analysed legacy transfer of 0.5 ETH, gas 21000 x 20 gwei, chain 1 */
static void base_evm(evmtx_t *tx)
{
    memset(tx, 0, sizeof *tx);
    tx->type = 0; tx->chain_id = 1; tx->nonce = 9; tx->gas_limit = 21000;
    u256_from_u64(&tx->max_fee_per_gas, 20000000000ull);
    tx->has_to = true; memcpy(tx->to, EVM_DEST, 20);
    u256_from_dec("500000000000000000", &tx->value);
    evmtx_analyze(tx, EVM_OWNER);
}

static void base_evm_claims(policy_evm_claims_t *c)
{
    memset(c, 0, sizeof *c);
    c->chain_id = 1;
    c->has_to = true; memcpy(c->to, EVM_DEST, 20);
    u256_from_dec("500000000000000000", &c->value_wei);
    u256_from_u64(&c->max_fee_wei, 420000000000000ull);
}

static void test_evm_checks(void)
{
    evmtx_t tx; policy_evm_claims_t c; uint32_t eff = 99;
    base_evm(&tx); base_evm_claims(&c);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    CHECK_EQ(eff, 0);
    /* (1) chain id and destination */
    c.chain_id = 10;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_CHAIN);
    CHECK_STR(policy_verdict_reason(POLICY_CHAIN), "claims_mismatch");
    c.chain_id = 1; c.to[0] ^= 1;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DEST);
    c.to[0] ^= 1; c.has_to = false;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DEST);
    c.has_to = true;
    /* (2) value: daemon says 0.4999 ETH, bytes say 0.5 */
    u256_from_dec("499999999999999999", &c.value_wei);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_VALUE);
    CHECK_STR(policy_verdict_reason(POLICY_VALUE), "claims_mismatch");
    u256_from_dec("600000000000000000", &c.value_wei); /* over-claiming is fine */
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    /* (3) fee: gas limit x max fee per gas must be covered */
    u256_from_u64(&c.max_fee_wei, 420000000000000ull - 1);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_FEE);
    CHECK_STR(policy_verdict_reason(POLICY_FEE), "claims_mismatch");
    base_evm_claims(&c);
    tx.gas_limit = UINT64_MAX; u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &tx.max_fee_per_gas);
    evmtx_analyze(&tx, EVM_OWNER); /* fee overflows -> saturates above any claim */
    u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639934", &c.max_fee_wei);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_FEE);
}

static void set_calldata(evmtx_t *tx, uint8_t *buf, uint32_t sel, const uint8_t a[20], const uint8_t b[20], const u256_t *amt)
{
    size_t n = 4;
    buf[0] = (uint8_t)(sel >> 24); buf[1] = (uint8_t)(sel >> 16); buf[2] = (uint8_t)(sel >> 8); buf[3] = (uint8_t)sel;
    memset(buf + n, 0, 12); memcpy(buf + n + 12, a, 20); n += 32;
    if (b) { memset(buf + n, 0, 12); memcpy(buf + n + 12, b, 20); n += 32; }
    u256_to_be32(amt, buf + n); n += 32;
    tx->data = buf; tx->data_len = n;
    memcpy(tx->to, TOKEN, 20);
    u256_zero(&tx->value);
    evmtx_analyze(tx, EVM_OWNER);
}

static void test_evm_tokens_and_dangerous(void)
{
    evmtx_t tx; policy_evm_claims_t c; uint32_t eff;
    uint8_t data[4 + 96];
    u256_t amt;
    base_evm(&tx); base_evm_claims(&c);
    memcpy(c.to, TOKEN, 20);
    u256_zero(&c.value_wei);
    /* (4) ERC-20 transfer of 1e6 units: must be claimed for that token */
    u256_from_u64(&amt, 1000000);
    set_calldata(&tx, data, EVMTX_SELECTOR_TRANSFER, EVM_DEST, NULL, &amt);
    CHECK_EQ(tx.call, EVMTX_CALL_TRANSFER);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN); /* nothing claimed */
    c.n_token_out = 1; memcpy(c.token_out[0].token, TOKEN, 20); u256_from_u64(&c.token_out[0].amount, 1000000);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN); /* recipient not named */
    c.token_out[0].has_to = true; memcpy(c.token_out[0].to, ROUTER, 20);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN); /* wrong recipient */
    memcpy(c.token_out[0].to, EVM_DEST, 20);
    u256_from_u64(&c.token_out[0].amount, 999999);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN);
    u256_from_u64(&c.token_out[0].amount, 1000000);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    memcpy(c.token_out[0].token, ROUTER, 20); /* wrong token claimed */
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN);
    memcpy(c.token_out[0].token, TOKEN, 20);
    /* transferFrom with the owner as source counts; with another source it does not */
    set_calldata(&tx, data, EVMTX_SELECTOR_TRANSFER_FROM, EVM_OWNER, EVM_DEST, &amt);
    CHECK_EQ(tx.call, EVMTX_CALL_TRANSFER_FROM);
    u256_from_u64(&c.token_out[0].amount, 1);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_TOKEN);
    set_calldata(&tx, data, EVMTX_SELECTOR_TRANSFER_FROM, ROUTER, EVM_DEST, &amt);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    /* (5) approve must be declared; unlimited needs its own name */
    set_calldata(&tx, data, EVMTX_SELECTOR_APPROVE, ROUTER, NULL, &amt);
    c.dangerous = 0;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, EVMTX_DANGER_APPROVE);
    CHECK_STR(policy_verdict_reason(POLICY_DANGEROUS), "policy");
    c.dangerous = EVMTX_DANGER_APPROVE;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &amt);
    set_calldata(&tx, data, EVMTX_SELECTOR_APPROVE, ROUTER, NULL, &amt);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DANGEROUS); /* "approve" declared, but it is unlimited */
    CHECK_EQ(eff, EVMTX_DANGER_APPROVE_UNLIMITED);
    c.dangerous = EVMTX_DANGER_APPROVE_UNLIMITED;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    /* setApprovalForAll(true) */
    u256_from_u64(&amt, 1);
    set_calldata(&tx, data, EVMTX_SELECTOR_SET_APPROVAL_FOR_ALL, ROUTER, NULL, &amt);
    c.dangerous = 0;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DANGEROUS);
    c.dangerous = EVMTX_DANGER_SET_APPROVAL_FOR_ALL;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    /* unknown calldata: router or declared unknown_contract */
    data[0] = 0xde; data[1] = 0xad; data[2] = 0xbe; data[3] = 0xef;
    tx.data = data; tx.data_len = 36; memcpy(tx.to, ROUTER, 20); evmtx_analyze(&tx, EVM_OWNER);
    CHECK_EQ(tx.call, EVMTX_CALL_UNKNOWN);
    memcpy(c.to, ROUTER, 20); c.dangerous = 0;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, EVMTX_DANGER_UNKNOWN_CONTRACT);
    {
        /* host cannot whitelist; our own list can */
        router_list_t owner;
        uint8_t oneinch[20];
        char hex[EVM_ADDR_HEX_LEN];
        size_t nb;
        const router_entry_t *b = routers_builtin(&nb);
        memset(&owner, 0, sizeof owner);
        CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, &owner, &eff), POLICY_DANGEROUS); /* empty owner list: still unknown */
        CHECK_EQ(routers_add(&owner, ROUTER_CHAIN_EVM, ROUTER), 0);
        CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, &owner, &eff), POLICY_OK);
        CHECK_EQ(eff, 0);
        CHECK(routers_remove(&owner, ROUTER_CHAIN_EVM, ROUTER));
        CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, &owner, &eff), POLICY_DANGEROUS);
        /* built-ins: 1inch v6 and 0x, checksummed exactly as published */
        evm_address_to_hex(b[1].addr, hex); CHECK_STR(hex, "0x111111125421cA6dc452d289314280a0f8842A65");
        evm_address_to_hex(b[2].addr, hex); CHECK_STR(hex, "0xDef1C0ded9bec7F1a1670819833240f027b25EfF");
        CHECK(evm_address_from_hex("0x111111125421cA6dc452d289314280a0f8842A65", oneinch));
        memcpy(tx.to, oneinch, 20); memcpy(c.to, oneinch, 20); evmtx_analyze(&tx, EVM_OWNER);
        CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
        CHECK_EQ(eff, 0);
        CHECK(routers_is_allowed(NULL, ROUTER_CHAIN_EVM, b[2].addr));
        CHECK(!routers_is_allowed(NULL, ROUTER_CHAIN_SOL, b[2].addr)); /* chain matters */
        CHECK(!routers_remove(&owner, ROUTER_CHAIN_EVM, oneinch));   /* built-ins cannot be removed */
        memcpy(tx.to, ROUTER, 20); memcpy(c.to, ROUTER, 20); evmtx_analyze(&tx, EVM_OWNER);
    }
    c.dangerous = EVMTX_DANGER_UNKNOWN_CONTRACT; /* declared: allowed, shown as DANGER */
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_OK);
    CHECK_EQ(eff, EVMTX_DANGER_UNKNOWN_CONTRACT);
    /* a router swap that also sends value is bounded by value_wei */
    u256_from_u64(&tx.value, 5);
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_VALUE);
    /* contract creation is always unknown_contract */
    tx.has_to = false; evmtx_analyze(&tx, EVM_OWNER); u256_zero(&tx.value);
    c.has_to = false; c.dangerous = 0;
    CHECK_EQ(policy_check_evm(&tx, EVM_OWNER, &c, NULL, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, EVMTX_DANGER_UNKNOWN_CONTRACT);
}

/* ---- Bitcoin ---- */
static void base_btc(btcpsbt_t *p)
{
    memset(p, 0, sizeof *p);
    p->n_in = 2; p->n_owner_inputs = 2; p->n_out = 2;
    p->sats_in = 100000; p->sats_out = 50000; p->sats_change = 48000; p->fee = 2000;
    p->out[0].amount = 50000; p->out[0].spk_len = 22; p->out[0].spk[1] = 0x14; memset(p->out[0].spk + 2, 0xd1, 20);
    p->out[1].amount = 48000; p->out[1].spk_len = 22; p->out[1].spk[1] = 0x14; memset(p->out[1].spk + 2, 0xc1, 20); p->out[1].is_change = true;
    p->has_dest = true; memcpy(p->dest_spk, p->out[0].spk, 22); p->dest_spk_len = 22;
    p->n_non_change = 1;
}

static void base_btc_claims(policy_btc_claims_t *c, const btcpsbt_t *p)
{
    memset(c, 0, sizeof *c);
    c->mainnet = true;
    c->sats_out = 50000;
    c->fee_sats = 2000;
    c->has_dest = true; memcpy(c->dest_spk, p->out[0].spk, 22); c->dest_spk_len = 22;
    c->has_inputs = true; c->inputs = 2;
}

static void test_btc_checks(void)
{
    btcpsbt_t p; policy_btc_claims_t c; uint32_t eff = 99;
    base_btc(&p); base_btc_claims(&c, &p);
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_OK);
    CHECK_EQ(eff, 0);
    /* (1) network */
    c.mainnet = false;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_CHAIN);
    c.mainnet = true;
    /* input count */
    c.inputs = 1;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_INPUTS);
    CHECK_STR(policy_verdict_reason(POLICY_INPUTS), "claims_mismatch");
    c.has_inputs = false;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_OK);
    c.has_inputs = true; c.inputs = 2;
    /* (2) sats_out */
    c.sats_out = 49999;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_VALUE);
    c.sats_out = 50000;
    /* (3) fee */
    c.fee_sats = 1999;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_FEE);
    c.fee_sats = 2000;
    /* (4) the destination must match; another non-change output must be declared */
    c.dest_spk[5] ^= 1;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DEST);
    CHECK_EQ(eff, BTCPSBT_DANGER_UNKNOWN_CHANGE); /* the real dest is not the claimed one */
    c.dest_spk[5] ^= 1;
    p.out[1].is_change = false; p.sats_out = 98000; p.sats_change = 0; p.n_non_change = 2; c.sats_out = 98000;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, BTCPSBT_DANGER_UNKNOWN_CHANGE);
    c.dangerous = BTCPSBT_DANGER_UNKNOWN_CHANGE;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_OK);
    /* no claimed dest at all while sats leave: claims_mismatch, declaring unknown_change does not help */
    base_btc(&p); base_btc_claims(&c, &p);
    c.has_dest = false; c.dangerous = 0;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DEST);
    CHECK_EQ(eff, BTCPSBT_DANGER_UNKNOWN_CHANGE);
    c.dangerous = BTCPSBT_DANGER_UNKNOWN_CHANGE;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DEST);
    /* (5) sighash and high fee come from the analysis and must be declared */
    base_btc(&p); base_btc_claims(&c, &p);
    p.dangerous = BTCPSBT_DANGER_SIGHASH_NOT_ALL;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DANGEROUS);
    CHECK_EQ(eff, BTCPSBT_DANGER_SIGHASH_NOT_ALL);
    c.dangerous = BTCPSBT_DANGER_SIGHASH_NOT_ALL | BTCPSBT_DANGER_HIGH_FEE; /* over-declaring is fine */
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_OK);
    p.dangerous = BTCPSBT_DANGER_HIGH_FEE; c.dangerous = BTCPSBT_DANGER_SIGHASH_NOT_ALL;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_DANGEROUS);
    /* consolidation: no destination on either side is fine */
    base_btc(&p); base_btc_claims(&c, &p);
    p.out[0].is_change = true; p.has_dest = false; p.sats_out = 0; p.sats_change = 98000; p.n_non_change = 0;
    c.has_dest = false; c.sats_out = 0;
    CHECK_EQ(policy_check_btc(&p, &c, &eff), POLICY_OK);
    CHECK_EQ(eff, 0);
}

int main(void)
{
    fill_key(OWNER, 0x11); fill_key(DEST, 0x22); fill_key(MINT, 0x66); fill_key(JUP, 0x99);
    memset(EVM_OWNER, 0x98, 20); memset(TOKEN, 0xa0, 20); memset(ROUTER, 0x1a, 20); memset(EVM_DEST, 0x35, 20);
    RUN(test_check1_fee_payer);
    RUN(test_check2_lamports);
    RUN(test_check2_tokens);
    RUN(test_check2_dest);
    RUN(test_check2_token_dest);
    RUN(test_unknown_program_and_routers);
    RUN(test_check3_programs);
    RUN(test_check4_dangerous);
    RUN(test_end_to_end_lie);
    RUN(test_evm_checks);
    RUN(test_evm_tokens_and_dangerous);
    RUN(test_btc_checks);
    TEST_END();
}
