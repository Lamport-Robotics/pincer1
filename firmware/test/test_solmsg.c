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
    m.buf[0] = 0x81; CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_E_VERSION); /* v1 */
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

int main(void)
{
    init_keys();
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
