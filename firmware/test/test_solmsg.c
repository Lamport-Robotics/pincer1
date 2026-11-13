#include "solmsg.h"
#include "util.h"

#include "harness.h"
#include "msgbuild.h"

#define SOL 1000000000ull

static uint8_t OWNER[32], DEST[32], OTHER[32], SRC_ATA[32], DST_ATA[32], MINT[32], TOKACCT[32], TABLE[32];

static void init_keys(void)
{
    fill_key(OWNER, 0x11); fill_key(DEST, 0x22); fill_key(OTHER, 0x33); fill_key(SRC_ATA, 0x44);
    fill_key(DST_ATA, 0x55); fill_key(MINT, 0x66); fill_key(TOKACCT, 0x77); fill_key(TABLE, 0x88);
}

static void test_legacy_transfer_half_sol(void)
{
    mb_t m;
    uint8_t keys[3][32], data[64], accts[2] = {0, 1};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 3);
    mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, data, ix_sys_transfer(data, SOL / 2));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK_EQ(p.version, SOLMSG_VERSION_LEGACY);
    CHECK_EQ(p.num_required_signatures, 1);
    CHECK_EQ(p.num_readonly_unsigned, 1);
    CHECK_EQ(p.n_static_keys, 3);
    CHECK_MEM(p.static_keys, OWNER, 32);
    CHECK_EQ(p.n_ix, 1);
    CHECK_EQ(p.ix[0].program_index, 2);
    CHECK_EQ(p.ix[0].n_accounts, 2);
    CHECK_EQ(p.ix[0].data_len, 12);
    CHECK(!p.ix[0].touches_lookup);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.fee_payer_is_owner);
    CHECK_U64(s.lamports_out, SOL / 2);
    CHECK(s.has_dest); CHECK_MEM(s.dest, DEST, 32);
    CHECK_EQ(s.n_programs, 1); CHECK_MEM(s.programs[0], SOLMSG_SYSTEM_PROGRAM, 32);
    CHECK_EQ(s.dangerous, 0);
    CHECK_EQ(s.n_token_out, 0);
    CHECK(solmsg_looks_like_message(m.buf, m.len));
    /* same bytes seen from another owner: nothing leaves, fee payer mismatch */
    solmsg_analyze(&p, OTHER, &s);
    CHECK(!s.fee_payer_is_owner);
    CHECK_U64(s.lamports_out, 0);
    /* transfer FROM someone else while owner pays fees: owner loses nothing */
    accts[0] = 1; accts[1] = 0;
    mb_begin(&m, -1, 2, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, data, ix_sys_transfer(data, SOL));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, 0);
}

static void test_two_transfers(void)
{
    mb_t m;
    uint8_t keys[4][32], d1[64], d2[64], a1[2] = {0, 1}, a2[2] = {0, 2};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], OTHER, 32); memcpy(keys[3], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 4);
    mb_n_ix(&m, 2);
    mb_ix(&m, 3, a1, 2, d1, ix_sys_transfer(d1, SOL / 2));
    mb_ix(&m, 3, a2, 2, d2, ix_sys_transfer(d2, SOL / 4));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK_EQ(p.n_ix, 2);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, SOL / 2 + SOL / 4);
    CHECK_MEM(s.dest, DEST, 32); /* first destination */
    CHECK(s.multi_dest);         /* ...and a second one the display could not show: flagged */
    CHECK_EQ(s.n_programs, 1);   /* deduplicated */
    /* the same recipient twice is one recipient */
    mb_begin(&m, -1, 1, 0, 1, keys, 4);
    mb_n_ix(&m, 2);
    mb_ix(&m, 3, a1, 2, d1, ix_sys_transfer(d1, SOL / 2));
    mb_ix(&m, 3, a1, 2, d2, ix_sys_transfer(d2, SOL / 4));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(!s.multi_dest);
    CHECK_U64(s.lamports_out, SOL / 2 + SOL / 4);
}

/* nonce accounts: withdrawing is a spend, handing the authority over is dangerous, an unknown tag is dangerous */
static void test_nonce_and_unknown_system_ixs(void)
{
    mb_t m;
    uint8_t keys[4][32], d[64], a_auth[2] = {1, 0}, a_wd[5] = {1, 2, 1, 1, 0}, a_adv[3] = {1, 1, 0};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], OTHER, 32); memcpy(keys[3], SOLMSG_SYSTEM_PROGRAM, 32);
    /* AuthorizeNonceAccount (7): [nonce, authority=owner], new authority OTHER */
    wr_le32(d, 7); memcpy(d + 4, OTHER, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 4); mb_n_ix(&m, 1); mb_ix(&m, 3, a_auth, 2, d, 36);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_SET_AUTHORITY);
    CHECK_U64(s.lamports_out, 0);
    /* the same instruction with someone else as the authority is not ours to flag */
    a_auth[1] = 2;
    mb_begin(&m, -1, 1, 0, 1, keys, 4); mb_n_ix(&m, 1); mb_ix(&m, 3, a_auth, 2, d, 36);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
    /* WithdrawNonceAccount (5): [nonce, to=OTHER, blockhashes, rent, authority=owner], lamports */
    wr_le32(d, 5); wr_le64(d + 4, SOL / 3);
    mb_begin(&m, -1, 1, 0, 1, keys, 4); mb_n_ix(&m, 1); mb_ix(&m, 3, a_wd, 5, d, 12);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, SOL / 3);
    CHECK(s.has_dest); CHECK_MEM(s.dest, OTHER, 32);
    /* AdvanceNonceAccount (4) moves nothing */
    wr_le32(d, 4);
    mb_begin(&m, -1, 1, 0, 1, keys, 4); mb_n_ix(&m, 1); mb_ix(&m, 3, a_adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0); CHECK_U64(s.lamports_out, 0);
    /* a System tag this parser has never heard of */
    wr_le32(d, 99);
    mb_begin(&m, -1, 1, 0, 1, keys, 4); mb_n_ix(&m, 1); mb_ix(&m, 3, a_adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_UNKNOWN_IX);
    CHECK_STR(solmsg_danger_name(SOLMSG_DANGER_UNKNOWN_IX), "unknown_instruction");
    CHECK_EQ(solmsg_danger_from_name("unknown_instruction"), SOLMSG_DANGER_UNKNOWN_IX);
}

static void test_other_system_ixs(void)
{
    mb_t m;
    uint8_t keys[3][32], d[128], accts[3] = {0, 1, 2};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32);
    /* createAccount funded by owner */
    mb_begin(&m, -1, 2, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_sys_create_account(d, 2039280, 165, SOLMSG_TOKEN_PROGRAM));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, 2039280);
    /* createAccountWithSeed */
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_sys_create_with_seed(d, OWNER, 777));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, 777);
    /* transferWithSeed: [derived, base=owner, to] */
    {
        uint8_t a[3] = {1, 0, 1};
        mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
        mb_ix(&m, 2, a, 3, d, ix_sys_transfer_with_seed(d, 999));
        CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
        solmsg_analyze(&p, OWNER, &s);
        CHECK_U64(s.lamports_out, 999);
    }
    /* assign on the owner account -> dangerous */
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 1, d, ix_sys_assign(d, SOLMSG_TOKEN_PROGRAM));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_ASSIGN);
    CHECK_U64(s.lamports_out, 0);
    /* assign on some other account -> not our problem */
    accts[0] = 1;
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 1, d, ix_sys_assign(d, SOLMSG_TOKEN_PROGRAM));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
}

static void test_spl_transfer_checked(void)
{
    mb_t m;
    uint8_t keys[5][32], d[64], accts[4] = {1, 2, 3, 0};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], SRC_ATA, 32); memcpy(keys[2], MINT, 32);
    memcpy(keys[3], DST_ATA, 32); memcpy(keys[4], SOLMSG_TOKEN_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 2, keys, 5);
    mb_n_ix(&m, 1);
    mb_ix(&m, 4, accts, 4, d, ix_tok_transfer_checked(d, 1000000, 6));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, 0);
    CHECK_EQ(s.n_token_out, 1);
    CHECK(s.token_out[0].has_mint);
    CHECK_MEM(s.token_out[0].mint, MINT, 32);
    CHECK_MEM(s.token_out[0].source, SRC_ATA, 32);
    CHECK_U64(s.token_out[0].amount, 1000000);
    CHECK(s.has_dest); CHECK_MEM(s.dest, DST_ATA, 32);
    CHECK_EQ(s.n_programs, 1); CHECK_MEM(s.programs[0], SOLMSG_TOKEN_PROGRAM, 32);
    CHECK_EQ(s.dangerous, 0);
    /* Token-2022 plain transfer (no mint) + burn */
    {
        uint8_t a3[3] = {1, 3, 0};
        uint8_t ab[3] = {1, 2, 0};
        uint8_t d2[16];
        memcpy(keys[4], SOLMSG_TOKEN2022_PROGRAM, 32);
        mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 2);
        mb_ix(&m, 4, a3, 3, d, ix_tok_transfer(d, 42));
        mb_ix(&m, 4, ab, 3, d2, ix_tok_burn(d2, 7));
        CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
        solmsg_analyze(&p, OWNER, &s);
        CHECK_EQ(s.n_token_out, 2);
        CHECK(!s.token_out[0].has_mint); CHECK_U64(s.token_out[0].amount, 42);
        CHECK(s.token_out[1].has_mint); CHECK_U64(s.token_out[1].amount, 7);
        CHECK_MEM(s.programs[0], SOLMSG_TOKEN2022_PROGRAM, 32);
    }
    /* transfer authorised by someone else: not counted */
    {
        uint8_t a[4] = {1, 2, 3, 3};
        mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
        mb_ix(&m, 4, a, 4, d, ix_tok_transfer_checked(d, 5, 6));
        CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
        solmsg_analyze(&p, OWNER, &s);
        CHECK_EQ(s.n_token_out, 0);
    }
}

static void test_set_authority_and_friends(void)
{
    mb_t m;
    uint8_t keys[3][32], d[64], accts[3];
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], TOKACCT, 32); memcpy(keys[2], SOLMSG_TOKEN_PROGRAM, 32);
    /* setAuthority: [account, current authority=owner] */
    accts[0] = 1; accts[1] = 0;
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_tok_set_authority(d, OTHER));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_SET_AUTHORITY);
    CHECK_STR(solmsg_danger_name(s.dangerous), "set_authority");
    /* approve: [source, delegate, owner] */
    accts[0] = 1; accts[1] = 1; accts[2] = 0;
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 3, d, ix_tok_approve(d, 1));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_APPROVE);
    /* closeAccount: [account, dest, owner] */
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 3, d, ix_tok_close(d));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_CLOSE_ACCOUNT);
    /* setAuthority where the owner is merely the fee payer: not dangerous for us */
    accts[0] = 1; accts[1] = 1;
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_tok_set_authority(d, OTHER));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
    CHECK_EQ(solmsg_danger_from_name("close_account"), SOLMSG_DANGER_CLOSE_ACCOUNT);
    CHECK_EQ(solmsg_danger_from_name("CloseAccount"), SOLMSG_DANGER_CLOSE_ACCOUNT);
    CHECK_EQ(solmsg_danger_from_name("lookup_table"), SOLMSG_DANGER_LOOKUP_TABLE);
    CHECK_EQ(solmsg_danger_from_name("nope"), 0);
}

static void test_v0_lookup_table(void)
{
    mb_t m;
    uint8_t keys[2][32], d[64], accts[2] = {0, 2}, wr[1] = {5};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, 0, 1, 0, 1, keys, 2);
    mb_n_ix(&m, 1);
    mb_ix(&m, 1, accts, 2, d, ix_sys_transfer(d, 50 * SOL)); /* "to" comes from the table */
    mb_n_lookups(&m, 1);
    mb_lookup(&m, TABLE, wr, 1, NULL, 0);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK_EQ(p.version, 0);
    CHECK_EQ(p.n_lookup_tables, 1);
    CHECK_EQ(p.n_lookup_accounts, 1);
    CHECK(p.ix[0].touches_lookup);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.fee_payer_is_owner);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_LOOKUP_TABLE);
    CHECK_U64(s.lamports_out, 0); /* unknown, not trusted: policy requires the claim */
    CHECK_EQ(s.n_programs, 1);
    CHECK(solmsg_looks_like_message(m.buf, m.len));
    /* v0 with a table but a purely static instruction is fine */
    accts[1] = 0;
    mb_begin(&m, 0, 1, 0, 1, keys, 2); mb_n_ix(&m, 1);
    mb_ix(&m, 1, accts, 2, d, ix_sys_transfer(d, 5));
    mb_n_lookups(&m, 1); mb_lookup(&m, TABLE, wr, 1, NULL, 0);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
    CHECK_U64(s.lamports_out, 5);
    /* program id from the table */
    mb_begin(&m, 0, 1, 0, 1, keys, 2); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_sys_transfer(d, 5));
    mb_n_lookups(&m, 1); mb_lookup(&m, TABLE, wr, 1, NULL, 0);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, SOLMSG_DANGER_LOOKUP_TABLE);
    CHECK_EQ(s.n_programs, 0);
    /* index beyond static + loaded -> error */
    accts[1] = 3;
    mb_begin(&m, 0, 1, 0, 1, keys, 2); mb_n_ix(&m, 1);
    mb_ix(&m, 1, accts, 2, d, ix_sys_transfer(d, 5));
    mb_n_lookups(&m, 1); mb_lookup(&m, TABLE, wr, 1, NULL, 0);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_INDEX);
}

static uint8_t CB_PROGRAM[32];

/* ComputeBudget: the fee is parsed from unit limit x unit price; without a limit, 200k units per instruction. */
static void test_compute_budget_fee(void)
{
    mb_t m;
    uint8_t keys[4][32], d1[16], d2[16], d3[64], a0[1] = {0}, a3[2] = {0, 1};
    solmsg_t p;
    solmsg_summary_t s;
    CHECK(pubkey_from_b58("ComputeBudget111111111111111111111111111111", CB_PROGRAM));
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32); memcpy(keys[3], CB_PROGRAM, 32);
    /* limit 1.4M units at 10^12 micro-lamports per unit = 1.4e12 lamports of "fee" on a 0-lamport memo-like tx */
    d1[0] = 2; wr_le32(d1 + 1, 1400000u);
    d2[0] = 3; wr_le64(d2 + 1, 1000000000000ull);
    mb_begin(&m, -1, 1, 0, 2, keys, 4); mb_n_ix(&m, 3);
    mb_ix(&m, 3, a0, 0, d1, 5);
    mb_ix(&m, 3, a0, 0, d2, 9);
    mb_ix(&m, 2, a3, 2, d3, ix_sys_transfer(d3, 0));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.lamports_out, 0);
    CHECK_U64(s.priority_lamports, 1400000000000ull);
    CHECK_U64(s.fee_lamports, 1400000000000ull + 5000);
    CHECK_EQ(s.dangerous, 0); /* ComputeBudget stays a core program: the fee is the claim, not a danger flag */
    /* no limit set: 200k units per non-budget instruction (one transfer) at 1000 micro-lamports = 200 lamports */
    d2[0] = 3; wr_le64(d2 + 1, 1000);
    mb_begin(&m, -1, 1, 0, 2, keys, 4); mb_n_ix(&m, 2);
    mb_ix(&m, 3, a0, 0, d2, 9);
    mb_ix(&m, 2, a3, 2, d3, ix_sys_transfer(d3, SOL));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.priority_lamports, 200);
    CHECK_U64(s.fee_lamports, 5200);
    /* no budget at all, two signers: just the base fee */
    mb_begin(&m, -1, 2, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, a3, 2, d3, ix_sys_transfer(d3, SOL));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.priority_lamports, 0);
    CHECK_U64(s.fee_lamports, 10000);
    /* a price that overflows saturates rather than wrapping to something small */
    d1[0] = 2; wr_le32(d1 + 1, 1400000u);
    d2[0] = 3; wr_le64(d2 + 1, UINT64_MAX);
    mb_begin(&m, -1, 1, 0, 2, keys, 4); mb_n_ix(&m, 2);
    mb_ix(&m, 3, a0, 0, d1, 5);
    mb_ix(&m, 3, a0, 0, d2, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.priority_lamports, UINT64_MAX);
    CHECK_U64(s.fee_lamports, UINT64_MAX);
}

/* Token-2022: transfer-with-fee and checked burn are outflows; anything unclassified is flagged, benign setup is not. */
static void test_token_2022_outflows(void)
{
    mb_t m;
    uint8_t keys[5][32], d[32], a4[4] = {1, 2, 3, 0}, a3[3] = {1, 2, 0}, a1[1] = {1};
    solmsg_t p;
    solmsg_summary_t s;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], SRC_ATA, 32); memcpy(keys[2], MINT, 32); memcpy(keys[3], DST_ATA, 32);
    memcpy(keys[4], SOLMSG_TOKEN2022_PROGRAM, 32);
    /* TransferCheckedWithFee: 26, 1, amount, decimals, fee */
    d[0] = 26; d[1] = 1; wr_le64(d + 2, 777); d[10] = 6; wr_le64(d + 11, 7);
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a4, 4, d, 19);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.n_token_out, 1);
    CHECK_U64(s.token_out[0].amount, 777);
    CHECK(s.token_out[0].has_mint); CHECK_MEM(s.token_out[0].mint, MINT, 32);
    CHECK(s.token_out[0].has_dest); CHECK_MEM(s.token_out[0].dest, DST_ATA, 32);
    CHECK_EQ(s.dangerous, 0);
    /* BurnChecked: 15, amount, decimals */
    d[0] = 15; wr_le64(d + 1, 55); d[9] = 6;
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a3, 3, d, 10);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.n_token_out, 1);
    CHECK_U64(s.token_out[0].amount, 55);
    CHECK(!s.token_out[0].has_dest);
    /* a transfer-fee sub-instruction that is not a transfer (harvest) and a confidential transfer: unknown */
    d[0] = 26; d[1] = 4;
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a3, 3, d, 2);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_UNKNOWN_IX);
    d[0] = 27; d[1] = 3;
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a3, 3, d, 2);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_UNKNOWN_IX);
    d[0] = 38;
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a3, 3, d, 1);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_UNKNOWN_IX);
    /* syncNative (17) and initializeAccount3 (18) move nothing: no flag */
    d[0] = 17;
    mb_begin(&m, -1, 1, 0, 2, keys, 5); mb_n_ix(&m, 1);
    mb_ix(&m, 4, a1, 1, d, 1);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
    CHECK_EQ(s.n_token_out, 0);
}

/* Jupiter routes: read end to end, or flagged. Reference ATAs from @solana/spl-token for owner 0x11.. / mint 0x66.. */
static void test_jupiter_route(void)
{
    mb_t m;
    uint8_t jup[32], ata[32], ata2022[32], want[32], want2022[32], disc[8] = {0xe5, 0x17, 0xcb, 0x97, 0x7a, 0xe3, 0xad, 0x2a};
    uint8_t keys[8][32], d[16], accts[9];
    solmsg_t p;
    solmsg_summary_t s;
    CHECK(pubkey_from_b58("JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4", jup));
    CHECK(pubkey_from_b58("8i2kbQccvSxNPcbQTtoxEoJi77GKE6i2sj9mmm5HH82t", want));
    CHECK(pubkey_from_b58("DxmVKtdeojLVA46sinLeY9abTPC5SGkJWKEgBZV74cGG", want2022));
    CHECK(solmsg_ata_derive(OWNER, SOLMSG_TOKEN_PROGRAM, MINT, ata));
    CHECK_MEM(ata, want, 32);
    CHECK(solmsg_ata_derive(OWNER, SOLMSG_TOKEN2022_PROGRAM, MINT, ata2022));
    CHECK_MEM(ata2022, want2022, 32);
    /* keys: 0 owner, 1 token program, 2 source ata, 3 dest ata (owner's), 4 mint, 5 event auth, 6 jupiter, 7 stranger ata */
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], SOLMSG_TOKEN_PROGRAM, 32); memcpy(keys[2], SRC_ATA, 32); memcpy(keys[3], ata, 32);
    memcpy(keys[4], MINT, 32); memcpy(keys[5], OTHER, 32); memcpy(keys[6], jup, 32); memcpy(keys[7], DST_ATA, 32);
    memcpy(d, disc, 8); d[8] = 0; /* route plan etc. do not matter to the check */
    /* route: [token_program, authority, user_source, user_destination, destination(opt = program id), destination_mint, platform_fee(opt), event_authority, program] */
    accts[0] = 1; accts[1] = 0; accts[2] = 2; accts[3] = 3; accts[4] = 6; accts[5] = 4; accts[6] = 6; accts[7] = 5; accts[8] = 6;
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous, 0);
    CHECK_EQ(s.n_unverified_programs, 0);
    CHECK(s.has_swap); CHECK_MEM(s.swap_mint, MINT, 32);
    /* output to someone else's account: flagged, and the program counts as unread */
    accts[3] = 7;
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_ROUTE);
    CHECK_EQ(s.n_unverified_programs, 1);
    CHECK(!s.has_swap);
    /* a third-party destination that is not the owner's ATA */
    accts[3] = 3; accts[4] = 7;
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_ROUTE);
    /* authority is not the owner */
    accts[4] = 6; accts[1] = 5;
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_ROUTE);
    /* an instruction shape this parser does not know */
    accts[1] = 0; d[0] ^= 0xff;
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.dangerous & SOLMSG_DANGER_ROUTE);
    CHECK_STR(solmsg_danger_name(SOLMSG_DANGER_ROUTE), "route_dest");
    /* any other non-core program is simply unread (no route flag) */
    memcpy(keys[6], OTHER, 32);
    mb_begin(&m, -1, 1, 0, 5, keys, 8); mb_n_ix(&m, 1);
    mb_ix(&m, 6, accts, 9, d, 9);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_EQ(s.dangerous & SOLMSG_DANGER_ROUTE, 0);
    CHECK_EQ(s.n_unverified_programs, 1);
}

static void test_malformed(void)
{
    mb_t m;
    uint8_t keys[3][32], d[64], accts[2] = {0, 1};
    solmsg_t p;
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32); memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, d, ix_sys_transfer(d, 1));
    CHECK_EQ(solmsg_parse(m.buf, m.len - 1, &p), SOLMSG_E_SHORT);     /* truncated */
    m.buf[m.len] = 0; CHECK_EQ(solmsg_parse(m.buf, m.len + 1, &p), SOLMSG_E_TRAILING);
    m.buf[0] = 0x82; CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_VERSION); /* a version this parser does not know */
    m.buf[0] = 0x81; CHECK(solmsg_parse(m.buf, m.len, &p) != SOLMSG_OK);        /* version 1 has another layout: these bytes are not one */
    m.buf[0] = 0x00; CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_HEADER);  /* no signers */
    m.buf[0] = 0x04; CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_HEADER);  /* more signers than keys */
    m.buf[0] = 0x01;
    /* program index out of range */
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 1);
    mb_ix(&m, 7, accts, 2, d, ix_sys_transfer(d, 1));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_INDEX);
    CHECK(!solmsg_looks_like_message(m.buf, m.len));
    CHECK(!solmsg_looks_like_message((const uint8_t *)"hello pincer", 12));
    CHECK(!solmsg_looks_like_message((const uint8_t *)"\xffsolana offchain", 16));
    CHECK(!solmsg_looks_like_message(m.buf, 0));
    /* a message with zero instructions still parses (and is refused for signMessage) */
    mb_begin(&m, -1, 1, 0, 1, keys, 3); mb_n_ix(&m, 0);
    CHECK(solmsg_looks_like_message(m.buf, m.len));
}

/* signMessage must never sign the message the Privacy Rings keys are derived from, however it is wrapped */
static void test_ring_key_derivation(void)
{
#define RING(s) solmsg_is_ring_key_derivation((const uint8_t *)(s), sizeof(s) - 1)
    CHECK(RING("TSPP/derive/v1"));
    CHECK(RING("TSPP/derive/v2"));                                  /* a later version of the same derivation */
    CHECK(RING("TSPP/derive/"));
    CHECK(RING("Sign in to example.xyz\nnonce: 81\nTSPP/derive/v1")); /* buried in an innocent-looking text */
    CHECK(RING("\xffsolana offchain\x00\x00\x0e\x00TSPP/derive/v1"));  /* inside an off-chain envelope */
    CHECK(RING("\x00\x00TSPP/derive/v1\x00\x00"));                   /* binary on both sides */
    CHECK(RING("TSPP/deriveTSPP/derive/v1"));                       /* a false start right before it */
    /* the canonical envelope's application domain, SHA-256("TSPP/derive/v1"), with some other payload behind it */
    CHECK(RING("\xffsolana offchain\x00" "\x1d\x32\xa8\x85\x33\xaf\x12\xd3\x5e\x5a\xc6\xfc\xe8\x17\xa4\xcb\x81\x0b\xcc\x41\x15\x38\x6b\x14\xa7\x8e\x8b\x2e\xf0\x9d\x86\x4c" "\x00\x01" "pubkeypubkeypubkeypubkeypubkey32" "\x02\x00" "hi"));
    CHECK(!RING("Sign in to pump.fun\nnonce: 1234"));
    CHECK(!RING("TSPP/derive"));                                    /* one byte short of the tag */
    CHECK(!RING("tspp/derive/v1"));                                 /* other bytes give another signature: not the key message */
    CHECK(!RING("TSPP/deriv/e/v1"));
    CHECK(!RING(""));
    CHECK(!solmsg_is_ring_key_derivation(NULL, 0));
    CHECK(!solmsg_is_ring_key_derivation((const uint8_t *)"TSPP/derive/v1", 11)); /* the length is honoured */
#undef RING
}

/* signMessage signs text the display shows whole, or an off-chain message for this key, and nothing else */
static void test_signmsg_check(void)
{
    solmsg_signmsg_t sm;
    uint8_t buf[256], v1tx[64];
    size_t n;
#define OK(s) solmsg_signmsg_check((const uint8_t *)(s), sizeof(s) - 1, OWNER, OTHER, &sm)
    /* bare text */
    CHECK(OK("Sign in to pump.fun\nnonce: 1234"));
    CHECK_EQ(sm.shape, SOLMSG_SIGNMSG_TEXT); CHECK_EQ((int)sm.text_len, 31); CHECK(sm.app_domain == NULL);
    CHECK(OK("hi"));
    CHECK(OK("Sign in to example.org nonce 42"));    /* 31 bytes */
    CHECK(!OK("abcdefghijklmnop qrstuvwxyz01234"));  /* exactly 32 bytes: the length of a digest */
    CHECK(OK("Sign in to example.org nonce 4242"));  /* 33 bytes, with spaces */
    CHECK(!OK("0123456789abcdef0123456789abcdef0"));  /* 33 characters and no space: a hash in text clothing */
    CHECK(!OK("{\"type\":\"withdraw\",\"amount\":\"1000000\",\"to\":\"x\"}")); /* compact JSON */
    CHECK(!OK("tab\there")); CHECK(!OK("cr\r\nlf")); CHECK(!OK("del\x7f")); CHECK(!OK("caf\xc3\xa9 au lait"));
    CHECK(!OK(""));
    CHECK(!solmsg_signmsg_check(NULL, 0, OWNER, NULL, &sm));
    CHECK(!solmsg_signmsg_check((const uint8_t *)"a\0b", 3, OWNER, NULL, &sm));
    /* key namespaces of Privacy Rings, wherever they sit */
    CHECK(!OK("TSPP/derive/v1")); CHECK(!OK("please sign TSPP/decrypt/v1 for me")); CHECK(!OK("zolana/intent/v1 hello there"));
    /* a versioned transaction message: the parser does not read version 1, the first byte gives it away */
    memset(v1tx, 'a', sizeof v1tx); v1tx[0] = 0x81; v1tx[10] = ' ';
    CHECK(!solmsg_looks_like_message(v1tx, sizeof v1tx));
    CHECK(!solmsg_signmsg_check(v1tx, sizeof v1tx, OWNER, OTHER, &sm));
    v1tx[0] = 0x80; CHECK(!solmsg_signmsg_check(v1tx, sizeof v1tx, OWNER, OTHER, &sm));
    v1tx[0] = 0xfe; CHECK(!solmsg_signmsg_check(v1tx, sizeof v1tx, OWNER, OTHER, &sm));
    /* a sign-in text must name the signer's own account */
    {
        char addr[48], t[200];
        CHECK(pubkey_to_b58(OWNER, addr, sizeof addr) > 0);
        snprintf(t, sizeof t, "example.org wants you to sign in with your Solana account:\n%s\n\nNonce: 1", addr);
        CHECK(solmsg_signmsg_check((const uint8_t *)t, strlen(t), OWNER, OTHER, &sm));
        CHECK(!solmsg_signmsg_check((const uint8_t *)t, strlen(t), DEST, NULL, &sm)); /* signed by another key */
        snprintf(t, sizeof t, "example.org wants you to sign in with your Solana account:");
        CHECK(!solmsg_signmsg_check((const uint8_t *)t, strlen(t), OWNER, OTHER, &sm));
    }
    /* off-chain message, header version 0: domain, 0, application domain, format, 1 signer, length, body */
#define V0(fmt, cnt, signer, body) (n = 0, memcpy(buf, "\xffsolana offchain", 16), n = 16, buf[n++] = 0, memset(buf + n, 0, 32), n += 32, \
        buf[n++] = (fmt), buf[n++] = (cnt), memcpy(buf + n, (signer), 32), n += 32, buf[n++] = (uint8_t)(sizeof(body) - 1), buf[n++] = 0, \
        memcpy(buf + n, (body), sizeof(body) - 1), n += sizeof(body) - 1, solmsg_signmsg_check(buf, n, OWNER, OTHER, &sm))
    CHECK(V0(1, 1, OWNER, "Sign in to example.org\nnonce: 1"));
    CHECK_EQ(sm.shape, SOLMSG_SIGNMSG_OFFCHAIN_V0); CHECK_EQ((int)sm.text_len, 31); CHECK(sm.text == buf + 85); CHECK(sm.app_domain == NULL);
    CHECK(V0(0, 1, OWNER, "Sign in to example.org"));
    CHECK(!V0(0, 1, OWNER, "Sign in to example.org\nnonce: 1")); /* format 0 has no line breaks */
    CHECK(!V0(2, 1, OWNER, "Sign in to example.org"));           /* extended UTF-8: not shown */
    CHECK(!V0(1, 1, DEST, "Sign in to example.org"));            /* someone else's message */
    CHECK(!V0(1, 2, OWNER, "Sign in to example.org"));           /* a second signer the owner never sees */
    CHECK(!V0(1, 1, OWNER, "caf\xc3\xa9"));
    CHECK(V0(1, 1, OWNER, "Sign in to example.org")); buf[17] = 0x42;
    CHECK(solmsg_signmsg_check(buf, n, OWNER, OTHER, &sm)); CHECK(sm.app_domain == buf + 17); /* a named application: shown */
    buf[83]++; CHECK(!solmsg_signmsg_check(buf, n, OWNER, OTHER, &sm));        /* length says more than there is */
    buf[83]--; CHECK(!solmsg_signmsg_check(buf, n - 1, OWNER, OTHER, &sm));    /* and less */
    CHECK(!solmsg_signmsg_check(buf, 85, OWNER, OTHER, &sm));                  /* header only */
    CHECK(!solmsg_signmsg_check(buf, 16, OWNER, OTHER, &sm));                  /* the bare signing domain */
    /* header version 1: domain, 1, 1 signer, content */
#define V1(cnt, signer, body) (memcpy(buf, "\xffsolana offchain", 16), buf[16] = 1, buf[17] = (cnt), memcpy(buf + 18, (signer), 32), \
        memcpy(buf + 50, (body), sizeof(body) - 1), n = 50 + sizeof(body) - 1, solmsg_signmsg_check(buf, n, OWNER, OTHER, &sm))
    CHECK(V1(1, OWNER, "Sign in to example.org\nnonce: 1"));
    CHECK_EQ(sm.shape, SOLMSG_SIGNMSG_OFFCHAIN_V1); CHECK(sm.text == buf + 50); CHECK_EQ((int)sm.text_len, 31);
    CHECK(!V1(1, DEST, "Sign in to example.org"));
    CHECK(!V1(2, OWNER, "Sign in to example.org"));
    CHECK(!V1(1, OWNER, ""));
    CHECK(!V1(1, OWNER, "TSPP/derive/v1"));
    CHECK(V1(1, OWNER, "x")); buf[16] = 2; CHECK(!solmsg_signmsg_check(buf, n, OWNER, OTHER, &sm)); /* a later header version */
    /* the other chains' message kinds use the bare-text rule */
#define T(s) solmsg_bare_text_ok((const uint8_t *)(s), sizeof(s) - 1)
    CHECK(T("example.org wants you to sign in with your Ethereum account:\n0x1111111111111111111111111111111111111111\n\nURI: https://example.org"));
    CHECK(!T("0x6fe39bfe9ff8a903ed3fbc056c33b1fd8c35d7281e92501964d50541b86e2d01")); /* a hash as hex text */
    CHECK(!T("\x6f\xe3\x9b\xfe\x9f\xf8\xa9\x03\xed\x3f\xbc\x05\x6c\x33\xb1\xfd\x8c\x35\xd7\x28\x1e\x92\x50\x19\x64\xd5\x05\x41\xb8\x6e\x2d\x01")); /* a raw hash */
    CHECK(!T("abcdefghijklmnop qrstuvwxyz01234"));
#undef T
#undef V1
#undef V0
#undef OK
}

/* until the controller parses Privacy Rings instructions, a transaction that calls them is refused whole */
static void test_touches_privacy_rings(void)
{
    mb_t m;
    uint8_t keys[3][32], data[64], accts[2] = {0, 1};
    solmsg_t p;
    static const char *ids[2] = { "ring1GaLCuJnkUMVkX2JeZLTTX17wKsV4fZmPVzXANW", "regy3WAeRXMYuMYxr5iK3Bf9Arr3PcxGxyzAddbiqDQ" };
    for (int i = 0; i < 2; i++) {
        memcpy(keys[0], OWNER, 32); memcpy(keys[1], DEST, 32);
        CHECK(pubkey_from_b58(ids[i], keys[2]));
        mb_begin(&m, -1, 1, 0, 1, keys, 3);
        mb_n_ix(&m, 1);
        data[0] = 15;
        mb_ix(&m, 2, accts, 2, data, 1);
        CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); /* the old framing parses: it must not reach the unknown-program screen */
        CHECK(solmsg_touches_privacy_rings(&p));
    }
    memcpy(keys[2], SOLMSG_SYSTEM_PROGRAM, 32);
    mb_begin(&m, -1, 1, 0, 1, keys, 3);
    mb_n_ix(&m, 1);
    mb_ix(&m, 2, accts, 2, data, ix_sys_transfer(data, SOL / 2));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK(!solmsg_touches_privacy_rings(&p));
}

/* A version 1 message (SIMD-0385) with one System transfer: 0x81, header, mask, blockhash, counts, three
 * addresses, config values, one instruction header, its account indexes and data. Returns the length. */
static size_t build_v1_transfer(uint8_t *b, uint32_t mask, uint64_t priority, uint32_t cu_limit, uint64_t lamports, const uint8_t *third_key)
{
    size_t o = 0;
    uint8_t data[64];
    size_t dl = ix_sys_transfer(data, lamports);
    b[o++] = 0x81; b[o++] = 1; b[o++] = 0; b[o++] = 1;
    b[o++] = (uint8_t)mask; b[o++] = (uint8_t)(mask >> 8); b[o++] = (uint8_t)(mask >> 16); b[o++] = (uint8_t)(mask >> 24);
    memset(b + o, 0xbb, 32); o += 32;
    b[o++] = 1; b[o++] = 3;
    memcpy(b + o, OWNER, 32); o += 32; memcpy(b + o, DEST, 32); o += 32; memcpy(b + o, third_key, 32); o += 32;
    if (mask & 1u) { for (int i = 0; i < 4; i++) b[o++] = (uint8_t)(priority >> (8 * i)); }
    if (mask & 2u) { for (int i = 4; i < 8; i++) b[o++] = (uint8_t)(priority >> (8 * i)); }
    if (mask & 4u) { for (int i = 0; i < 4; i++) b[o++] = (uint8_t)(cu_limit >> (8 * i)); }
    if (mask & 8u) { b[o++] = 0; b[o++] = 0; b[o++] = 0; b[o++] = 4; }
    if (mask & 16u) { b[o++] = 0; b[o++] = 0x80; b[o++] = 0; b[o++] = 0; } /* 32 KiB heap */
    b[o++] = 2; b[o++] = 2; b[o++] = (uint8_t)dl; b[o++] = (uint8_t)(dl >> 8);
    b[o++] = 0; b[o++] = 1;
    memcpy(b + o, data, dl); o += dl;
    return o;
}

/* version 1: read like any other transaction, with its fee from the header, and refused when malformed */
static void test_v1_message(void)
{
    static uint8_t b[512], c[512];
    static solmsg_t p; /* large: keep it off the stack */
    solmsg_summary_t s;
    solmsg_signmsg_t sm;
    uint8_t pool[32];
    size_t n = build_v1_transfer(b, 0x1f, 20000, 200000, SOL / 2, SOLMSG_SYSTEM_PROGRAM);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_OK);
    CHECK_EQ(p.version, SOLMSG_VERSION_1);
    CHECK_EQ(p.num_required_signatures, 1); CHECK_EQ(p.n_static_keys, 3); CHECK_EQ(p.n_ix, 1);
    CHECK_EQ(p.n_lookup_tables, 0);
    CHECK_U64(p.v1_priority_lamports, 20000); CHECK_EQ((int)p.v1_compute_unit_limit, 200000);
    CHECK_EQ(p.ix[0].program_index, 2); CHECK_EQ(p.ix[0].n_accounts, 2); CHECK_EQ(p.ix[0].data_len, 12);
    solmsg_analyze(&p, OWNER, &s);
    CHECK(s.fee_payer_is_owner);
    CHECK_U64(s.lamports_out, SOL / 2);
    CHECK(s.has_dest); CHECK_MEM(s.dest, DEST, 32);
    CHECK_U64(s.priority_lamports, 20000);
    CHECK_U64(s.fee_lamports, 25000);
    CHECK_EQ(s.dangerous, 0);
    /* it is a transaction, so it can never be signed as a message: twice refused */
    CHECK(solmsg_looks_like_message(b, n));
    CHECK(!solmsg_signmsg_check(b, n, OWNER, OTHER, &sm));
    /* no config at all: no priority fee */
    n = build_v1_transfer(b, 0, 0, 0, SOL, SOLMSG_SYSTEM_PROGRAM);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.priority_lamports, 0); CHECK_U64(s.fee_lamports, 5000); CHECK_U64(s.lamports_out, SOL);
    /* a fee the size of a transfer is on the summary for policy and the display to act on */
    n = build_v1_transfer(b, 3, 2 * SOL, 0, 1000, SOLMSG_SYSTEM_PROGRAM);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_OK);
    solmsg_analyze(&p, OWNER, &s);
    CHECK_U64(s.priority_lamports, 2 * SOL);
    /* a Privacy Rings call in version 1 framing is recognised the same way */
    CHECK(pubkey_from_b58("ring1GaLCuJnkUMVkX2JeZLTTX17wKsV4fZmPVzXANW", pool));
    n = build_v1_transfer(b, 0, 0, 0, 1000, pool);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_OK);
    CHECK(solmsg_touches_privacy_rings(&p));
    /* malformed: each rule of the format */
    n = build_v1_transfer(b, 0x1f, 20000, 200000, SOL / 2, SOLMSG_SYSTEM_PROGRAM);
#define BAD(code, stmt) do { memcpy(c, b, n); size_t m = n; stmt; CHECK_EQ(solmsg_parse(c, m, &p), code); CHECK(!solmsg_looks_like_message(c, m)); } while (0)
    BAD(SOLMSG_E_HEADER, c[4] = 0x3f);                       /* a config bit this parser does not know */
    BAD(SOLMSG_E_HEADER, c[1] = 0);                          /* no signer */
    BAD(SOLMSG_E_HEADER, c[1] = 13);                         /* more signatures than a transaction may carry */
    BAD(SOLMSG_E_HEADER, c[2] = 1);                          /* a read-only fee payer */
    BAD(SOLMSG_E_HEADER, c[3] = 3);                          /* more read-only accounts than there are */
    BAD(SOLMSG_E_HEADER, memcpy(c + 42 + 32, c + 42, 32));   /* the same address twice */
    BAD(SOLMSG_E_TRAILING, c[m++] = 0);
    BAD(SOLMSG_E_SHORT, m--);
    BAD(SOLMSG_E_INDEX, c[n - 13] = 3);                      /* an account index past the address list */
    BAD(SOLMSG_E_INDEX, c[n - 18] = 9);                      /* a program index past the address list */
    BAD(SOLMSG_E_TOOMANY, c[40] = 65);                       /* more instructions than the format allows */
    BAD(SOLMSG_E_TOOMANY, c[41] = 65);                       /* more addresses than the format allows */
#undef BAD
    /* half a priority fee (one of its two bits) */
    n = build_v1_transfer(b, 1, 20000, 0, 1000, SOLMSG_SYSTEM_PROGRAM);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_E_HEADER);
    /* a heap size outside the allowed range */
    n = build_v1_transfer(b, 16, 0, 0, 1000, SOLMSG_SYSTEM_PROGRAM);
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_OK);
    b[42 + 96 + 1] = 0x10; /* 4 KiB */
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_E_HEADER);
    /* 0x82 and above are other versions: still refused */
    n = build_v1_transfer(b, 0, 0, 0, 1000, SOLMSG_SYSTEM_PROGRAM);
    b[0] = 0x82;
    CHECK_EQ(solmsg_parse(b, n, &p), SOLMSG_E_VERSION);
}

int main(void)
{
    init_keys();
    RUN(test_ring_key_derivation);
    RUN(test_v1_message);
    RUN(test_signmsg_check);
    RUN(test_touches_privacy_rings);
    RUN(test_legacy_transfer_half_sol);
    RUN(test_two_transfers);
    RUN(test_nonce_and_unknown_system_ixs);
    RUN(test_other_system_ixs);
    RUN(test_spl_transfer_checked);
    RUN(test_set_authority_and_friends);
    RUN(test_v0_lookup_table);
    RUN(test_compute_budget_fee);
    RUN(test_token_2022_outflows);
    RUN(test_jupiter_route);
    RUN(test_malformed);
    TEST_END();
}
