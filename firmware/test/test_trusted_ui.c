/* Trusted UI: screens are built from the controller's own parse; amounts in native units, destinations as
 * first 6 + last 6 of the chain's encoding, dangers boxed, pagination, footers, idle / PIN / notice / word pages. */
#include "trusted_ui.h"

#include "chainaddr.h"
#include "harness.h"
#include "util.h"

#define DOT "\xC2\xB7"
#define ELL "\xE2\x80\xA6"

static const char *line(const tui_doc_t *d, int page, int i) { return d->p[page].l[i].text; }
static int style(const tui_doc_t *d, int page, int i) { return d->p[page].l[i].style; }

/* true when some line of the document equals `text` */
static bool has_line(const tui_doc_t *d, const char *text)
{
    int p, i;
    for (p = 0; p < d->n_pages; p++)
        for (i = 0; i < d->p[p].n_lines; i++)
            if (strcmp(d->p[p].l[i].text, text) == 0) return true;
    return false;
}
static bool has_box(const tui_doc_t *d, const char *text)
{
    int p, i;
    for (p = 0; p < d->n_pages; p++)
        for (i = 0; i < d->p[p].n_lines; i++)
            if (d->p[p].l[i].style == TUI_BOX && strcmp(d->p[p].l[i].text, text) == 0) return true;
    return false;
}

static void test_formatting(void)
{
    char out[80];
    u256_t v;
    tui_fmt_units(500000000ull, 9, out, sizeof out); CHECK_STR(out, "0.5");
    tui_fmt_units(0, 9, out, sizeof out); CHECK_STR(out, "0");
    tui_fmt_units(1000000000ull, 9, out, sizeof out); CHECK_STR(out, "1");
    tui_fmt_units(1234567891ull, 9, out, sizeof out); CHECK_STR(out, "1.234567891");
    tui_fmt_units(1, 9, out, sizeof out); CHECK_STR(out, "0.000000001");
    tui_fmt_units(150000, 8, out, sizeof out); CHECK_STR(out, "0.0015");
    tui_fmt_units(2100, 8, out, sizeof out); CHECK_STR(out, "0.000021");
    tui_fmt_units(123456789012ull, 0, out, sizeof out); CHECK_STR(out, "123456789012");
    u256_from_dec("500000000000000000", &v); tui_fmt_units_u256(&v, 18, out, sizeof out); CHECK_STR(out, "0.5");
    u256_from_dec("420000000000000", &v); tui_fmt_units_u256(&v, 18, out, sizeof out); CHECK_STR(out, "0.00042");
    u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &v);
    tui_fmt_units_u256(&v, 18, out, sizeof out);
    CHECK_STR(out, "115792089237316195423570985008687907853269984665640564039457.584007913129639935");
    tui_short("abcdefghijklmnop", out, sizeof out); CHECK_STR(out, "abcdef" ELL "klmnop");
    tui_short("short", out, sizeof out); CHECK_STR(out, "short");
    tui_short("exactly13chars", out, sizeof out); CHECK_STR(out, "exactl" ELL "3chars"); /* 14 */
    tui_short("exactly13char", out, sizeof out); CHECK_STR(out, "exactly13char");
    {
        uint8_t k[32] = {0}, evm[20], pkh[20], spk[22];
        tui_short_b58(k, out, sizeof out); CHECK_STR(out, "111111" ELL "111111");
        /* JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4 */
        pubkey_from_b58("JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4", k);
        tui_short_b58(k, out, sizeof out); CHECK_STR(out, "JUP6Lk" ELL "yVTaV4");
        /* EIP-55 case is kept: 0x9858EfFD232B4033E47d90003D41EC34EcaEda94 */
        hex2bin("9858effd232b4033e47d90003d41ec34ecaeda94", evm, 20);
        tui_short_evm(evm, out, sizeof out); CHECK_STR(out, "0x9858Ef" ELL "aEda94");
        hex2bin("c0cebcd6c3d3ca8c75dc5ec62ebe55330ef910e2", pkh, 20); /* bc1qcr8te4kr609gcawutmrza0j4xv80jy8z306fyu */
        btc_p2wpkh_script(pkh, spk);
        tui_short_btc_script(spk, 22, out, sizeof out); CHECK_STR(out, "bc1qcr" ELL "306fyu");
        spk[0] = 0x6a; /* OP_RETURN: no address */
        tui_short_btc_script(spk, 22, out, sizeof out); CHECK_STR(out, "(no address)");
    }
    CHECK_STR(tui_evm_chain_name(1), "ETHEREUM"); CHECK_STR(tui_evm_unit(1), "ETH");
    CHECK_STR(tui_evm_chain_name(8453), "BASE"); CHECK_STR(tui_evm_unit(137), "POL");
    CHECK(tui_evm_chain_name(31337) == NULL); CHECK_STR(tui_evm_unit(31337), "NATIVE");
}

static void test_doc_pagination(void)
{
    tui_doc_t d;
    int i;
    tui_doc_init(&d);
    CHECK_EQ(d.n_pages, 0);
    tui_add(&d, TUI_HEAD, "H");
    CHECK_EQ(d.n_pages, 1);
    for (i = 0; i < 5; i++) tui_add(&d, TUI_TEXT, "t"); /* 6 x 8 px = 48 px: full */
    CHECK_EQ(d.n_pages, 1); CHECK_EQ(d.p[0].n_lines, 6);
    tui_add(&d, TUI_TEXT, "overflow");
    CHECK_EQ(d.n_pages, 2); CHECK_EQ(d.p[1].n_lines, 1);
    /* a BIG line is 16 px, a BOX 12: 16 + 12 + 12 + 8 = 48 fits, the next 8 does not */
    tui_doc_init(&d);
    tui_add(&d, TUI_BIG, "1 SOL"); tui_add(&d, TUI_BOX, "! A"); tui_add(&d, TUI_BOX, "! B"); tui_add(&d, TUI_TEXT, "x");
    CHECK_EQ(d.n_pages, 1);
    tui_add(&d, TUI_TEXT, "y");
    CHECK_EQ(d.n_pages, 2);
    /* over-long text is cut with an ellipsis at 21 cells (16 for BIG) */
    tui_doc_init(&d);
    tui_add(&d, TUI_TEXT, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    CHECK_STR(line(&d, 0, 0), "ABCDEFGHIJKLMNOPQRST" ELL);
    CHECK_EQ(display_text_cells(line(&d, 0, 0)), 21);
    tui_add(&d, TUI_BIG, "ABCDEFGHIJKLMNOPQRS");
    CHECK_STR(line(&d, 0, 1), "ABCDEFGHIJKLMNO" ELL);
    tui_add(&d, TUI_TEXT, "exactly twenty-one ch");
    CHECK_STR(line(&d, 0, 2), "exactly twenty-one ch");
    /* truncation flag after TUI_MAX_PAGES */
    tui_doc_init(&d);
    for (i = 0; i < TUI_MAX_PAGES * 6; i++) tui_add(&d, TUI_TEXT, "l");
    CHECK(!d.truncated); CHECK_EQ(d.n_pages, TUI_MAX_PAGES);
    tui_add(&d, TUI_TEXT, "one too many");
    CHECK(d.truncated);
}

static uint8_t OWNER[32], DEST[32], MINT[32], ATA[32], JUP[32];

static void test_solana_screens(void)
{
    solmsg_summary_t s;
    tui_sign_t r;
    tui_doc_t d;
    char sh[24];
    memset(&s, 0, sizeof s);
    s.fee_payer_is_owner = true;
    s.lamports_out = 500000000ull;
    s.has_dest = true; memcpy(s.dest, DEST, 32);
    memcpy(s.programs[0], SOLMSG_SYSTEM_PROGRAM, 32); s.n_programs = 1;
    memset(&r, 0, sizeof r);
    r.kind = TUI_SIGN_SOL_TX; r.sol = &s;
    tui_build_sign(&r, &d);
    CHECK_EQ(d.n_pages, 1);
    CHECK_EQ(style(&d, 0, 0), TUI_HEAD); CHECK_STR(line(&d, 0, 0), "SOLANA " DOT " SEND");
    CHECK_EQ(style(&d, 0, 1), TUI_BIG); CHECK_STR(line(&d, 0, 1), "0.5 SOL");
    tui_short_b58(DEST, sh, sizeof sh);
    CHECK_EQ(style(&d, 0, 2), TUI_TEXT); CHECK(strncmp(line(&d, 0, 2), "to ", 3) == 0); CHECK_STR(line(&d, 0, 2) + 3, sh);
    CHECK_EQ(d.p[0].n_lines, 3);
    CHECK_EQ(tui_sign_footer(&r), TUI_FOOT_SIGN);
    /* dangers are boxed; unknown programs are listed by short id */
    r.sol_dangerous = SOLMSG_DANGER_UNKNOWN_PROGRAM | SOLMSG_DANGER_APPROVE;
    memcpy(s.programs[1], JUP, 32); s.n_programs = 2;
    tui_build_sign(&r, &d);
    CHECK(has_box(&d, "! UNKNOWN PROGRAM"));
    CHECK(has_box(&d, "! APPROVE (delegate)"));
    tui_short_b58(JUP, sh, sizeof sh);
    { char l[40]; snprintf(l, sizeof l, "prog %s", sh); CHECK(has_line(&d, l)); }
    CHECK(!has_line(&d, "prog 111111" ELL "111111")); /* the System program is core: not listed */
    /* token transfer: raw units + mint + recipient */
    memset(&s, 0, sizeof s);
    s.fee_payer_is_owner = true;
    s.n_token_out = 1; s.token_out[0].has_mint = true; memcpy(s.token_out[0].mint, MINT, 32);
    s.token_out[0].has_dest = true; memcpy(s.token_out[0].dest, ATA, 32); s.token_out[0].amount = 1000000;
    r.sol_dangerous = 0;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "SOLANA " DOT " TRANSACTION");
    CHECK_STR(line(&d, 0, 1), "token 1000000 units");
    tui_short_b58(MINT, sh, sizeof sh); { char l[40]; snprintf(l, sizeof l, " mint %s", sh); CHECK_STR(line(&d, 0, 2), l); }
    tui_short_b58(ATA, sh, sizeof sh); { char l[40]; snprintf(l, sizeof l, " to %s", sh); CHECK_STR(line(&d, 0, 3), l); }
    s.token_out[0].has_mint = false; s.token_out[0].has_dest = false;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 2), " mint unknown"); CHECK_STR(line(&d, 0, 3), " burn");
    /* eight token moves paginate and never truncate */
    { int i; s.n_token_out = 8; for (i = 0; i < 8; i++) { s.token_out[i].has_mint = true; s.token_out[i].has_dest = true; s.token_out[i].amount = (uint64_t)i; } }
    tui_build_sign(&r, &d);
    CHECK(!d.truncated); CHECK(d.n_pages >= 4 && d.n_pages <= TUI_MAX_PAGES);
    /* a message: length and preview, never interpreted */
    {
        static const uint8_t msg[] = "Sign in to example.org nonce 42";
        memset(&r, 0, sizeof r);
        r.kind = TUI_SIGN_SOL_MSG; r.msg = msg; r.msg_len = sizeof msg - 1;
        tui_build_sign(&r, &d);
        CHECK_STR(line(&d, 0, 0), "SOLANA " DOT " MESSAGE");
        CHECK_STR(line(&d, 0, 1), "31 bytes");
        CHECK_STR(line(&d, 0, 2), "text:");
        CHECK_STR(line(&d, 0, 3), "Sign in to example.or");
        CHECK_STR(line(&d, 0, 4), "g nonce 42");
        {
            static const uint8_t bin[] = {0x01, 0x02, 0xff, 0x10, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
            r.msg = bin; r.msg_len = sizeof bin;
            tui_build_sign(&r, &d);
            CHECK_STR(line(&d, 0, 2), "hex 0102ff10aabbccdd" ELL);
        }
    }
    /* inheritance: same parse + arrangement, arming needs a long touch */
    memset(&s, 0, sizeof s);
    s.fee_payer_is_owner = true; s.lamports_out = 3000000000ull; s.has_dest = true; memcpy(s.dest, DEST, 32);
    memset(&r, 0, sizeof r);
    r.kind = TUI_SIGN_INHERITANCE; r.sol = &s; r.inh_days = 90; r.inh_heir = DEST;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "INHERITANCE " DOT " ARM");
    CHECK_STR(line(&d, 0, 1), "3 SOL");
    CHECK(has_line(&d, "after 90 days idle"));
    CHECK_EQ(tui_sign_footer(&r), TUI_FOOT_HOLD);
    r.inh_replace = true;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "INHERITANCE " DOT " REPLACE");
    CHECK_EQ(tui_sign_footer(&r), TUI_FOOT_SIGN);
}

static void test_evm_screens(void)
{
    evmtx_t tx;
    tui_sign_t r;
    tui_doc_t d;
    uint8_t owner[20], to[20], data[4 + 64];
    u256_t amt;
    memset(owner, 0x98, 20);
    hex2bin("111111125421ca6dc452d289314280a0f8842a65", to, 20);
    memset(&tx, 0, sizeof tx);
    tx.type = 2; tx.chain_id = 1; tx.nonce = 9; tx.gas_limit = 21000;
    u256_from_u64(&tx.max_fee_per_gas, 20000000000ull);
    tx.has_to = true; memcpy(tx.to, to, 20);
    u256_from_dec("500000000000000000", &tx.value);
    evmtx_analyze(&tx, owner);
    memset(&r, 0, sizeof r);
    r.kind = TUI_SIGN_EVM_TX; r.evm = &tx;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "ETHEREUM " DOT " SEND");
    CHECK_STR(line(&d, 0, 1), "0.5 ETH");
    CHECK_STR(line(&d, 0, 2), "to 0x111111" ELL "842A65"); /* EIP-55 case */
    CHECK_STR(line(&d, 0, 3), "chainId 1");
    CHECK_STR(line(&d, 0, 4), "nonce 9");
    CHECK_EQ(d.p[0].n_lines, 5); /* 8 + 16 + 8 + 8 + 8 = 48 px: the fee opens page 2 */
    CHECK_EQ(d.n_pages, 2);
    CHECK_STR(line(&d, 1, 0), "fee max 0.00042 ETH");
    /* a value that does not fit the big face is shown small and complete, never cut */
    u256_from_dec("1234567890123456789012", &tx.value); evmtx_analyze(&tx, owner);
    tui_build_sign(&r, &d);
    CHECK_EQ(style(&d, 0, 1), TUI_TEXT);
    CHECK_STR(line(&d, 0, 1), "1234.5678901234567890");
    CHECK_STR(line(&d, 0, 2), "12 ETH");
    u256_from_dec("500000000000000000", &tx.value); evmtx_analyze(&tx, owner);
    /* unknown chain id: named by number, unit NATIVE */
    tx.chain_id = 31337; evmtx_analyze(&tx, owner);
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "CHAIN 31337 " DOT " SEND");
    CHECK_STR(line(&d, 0, 1), "0.5 NATIVE");
    CHECK(has_line(&d, "chainId 31337"));
    tx.chain_id = 8453;
    /* unknown contract call: boxed DANGER, calldata length */
    data[0] = 0xde; data[1] = 0xad; data[2] = 0xbe; data[3] = 0xef;
    tx.data = data; tx.data_len = 36; u256_zero(&tx.value); evmtx_analyze(&tx, owner);
    r.evm_dangerous = EVMTX_DANGER_UNKNOWN_CONTRACT;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "BASE " DOT " CALL");
    CHECK(has_box(&d, "! UNKNOWN CONTRACT"));
    CHECK(has_line(&d, " calldata 36 bytes"));
    CHECK(!has_line(&d, "0 ETH")); /* zero value on a call is not shouted */
    r.evm_dangerous = 0;
    tui_build_sign(&r, &d);
    CHECK(!has_box(&d, "! UNKNOWN CONTRACT"));
    CHECK(has_line(&d, "known router call"));
    /* approve unlimited */
    {
        size_t n = 4;
        uint32_t sel = EVMTX_SELECTOR_APPROVE;
        data[0] = (uint8_t)(sel >> 24); data[1] = (uint8_t)(sel >> 16); data[2] = (uint8_t)(sel >> 8); data[3] = (uint8_t)sel;
        memset(data + n, 0, 12); memset(data + n + 12, 0x1a, 20); n += 32;
        u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &amt);
        u256_to_be32(&amt, data + n); n += 32;
        tx.data = data; tx.data_len = n; evmtx_analyze(&tx, owner);
        CHECK_EQ(tx.call, EVMTX_CALL_APPROVE);
        r.evm_dangerous = tx.dangerous;
        tui_build_sign(&r, &d);
        CHECK(has_box(&d, "! APPROVE UNLIMITED"));
        CHECK(has_line(&d, "spender:"));
        { uint8_t sp[20]; char sh[24], l[40]; memset(sp, 0x1a, 20); tui_short_evm(sp, sh, sizeof sh); snprintf(l, sizeof l, " %s", sh); CHECK(has_line(&d, l)); CHECK(strncmp(sh, "0x1", 3) == 0); }
        CHECK(!has_line(&d, "allow 115792089237316195423570985008687907853269984665640564039457584007913129639935 units"));
        /* finite approve shows the allowance */
        u256_from_u64(&amt, 1000000); u256_to_be32(&amt, data + 36); evmtx_analyze(&tx, owner);
        r.evm_dangerous = tx.dangerous;
        tui_build_sign(&r, &d);
        CHECK(has_box(&d, "! APPROVE"));
        CHECK(has_line(&d, "allow 1000000 units"));
        /* ERC-20 transfer: units, token, recipient */
        sel = EVMTX_SELECTOR_TRANSFER;
        data[0] = (uint8_t)(sel >> 24); data[1] = (uint8_t)(sel >> 16); data[2] = (uint8_t)(sel >> 8); data[3] = (uint8_t)sel;
        evmtx_analyze(&tx, owner);
        r.evm_dangerous = 0;
        tui_build_sign(&r, &d);
        CHECK(has_line(&d, "ERC20 transfer"));
        CHECK(has_line(&d, "1000000 units"));
        CHECK(has_line(&d, "token 0x111111" ELL "842A65"));
        { uint8_t sp[20]; char sh[24], l[40]; memset(sp, 0x1a, 20); tui_short_evm(sp, sh, sizeof sh); snprintf(l, sizeof l, "to %s", sh); CHECK(has_line(&d, l)); }
        /* a 78-digit token amount wraps over lines instead of being cut */
        u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &amt);
        u256_to_be32(&amt, data + 36); evmtx_analyze(&tx, owner);
        tui_build_sign(&r, &d);
        CHECK(has_line(&d, "115792089237316195423"));
        CHECK(has_line(&d, "570985008687907853269"));
        CHECK(has_line(&d, "007913129639935 units"));
        CHECK(!d.truncated);
    }
    /* contract creation */
    tx.has_to = false; tx.data_len = 0; evmtx_analyze(&tx, owner);
    r.evm_dangerous = EVMTX_DANGER_UNKNOWN_CONTRACT;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "BASE " DOT " CREATE");
    CHECK(has_box(&d, "! CONTRACT CREATE"));
    /* typed data: the digest the controller computed */
    {
        uint8_t dg[32];
        int i;
        for (i = 0; i < 32; i++) dg[i] = (uint8_t)i;
        memset(&r, 0, sizeof r);
        r.kind = TUI_SIGN_EVM_TYPED; r.digest = dg;
        tui_build_sign(&r, &d);
        CHECK_STR(line(&d, 0, 0), "EVM " DOT " TYPED DATA");
        CHECK_STR(line(&d, 0, 1), "digest:");
        CHECK_STR(line(&d, 0, 2), "0001020304050607");
        CHECK_STR(line(&d, 0, 5), "18191a1b1c1d1e1f");
        CHECK_EQ(d.n_pages, 1);
    }
}

static void test_btc_screens(void)
{
    btcpsbt_t p;
    tui_sign_t r;
    tui_doc_t d;
    memset(&p, 0, sizeof p);
    p.n_in = 2; p.n_owner_inputs = 2; p.n_out = 2;
    p.sats_in = 100000; p.sats_out = 50000; p.sats_change = 47900; p.fee = 2100; p.n_non_change = 1;
    p.has_dest = true; hex2bin("0014751e76e8199196d454941c45d1b3a323f1433bd6", p.dest_spk, 22); p.dest_spk_len = 22;
    memset(&r, 0, sizeof r);
    r.kind = TUI_SIGN_BTC_PSBT; r.btc = &p;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "BITCOIN " DOT " SEND");
    CHECK_STR(line(&d, 0, 1), "0.0005 BTC");
    CHECK_STR(line(&d, 0, 2), "to bc1qw5" ELL "v8f3t4");
    CHECK_STR(line(&d, 0, 3), "fee 0.000021 BTC");
    CHECK_STR(line(&d, 0, 4), " = 2100 sat");
    CHECK(has_line(&d, "inputs 2"));
    CHECK(has_line(&d, "change 0.000479 BTC"));
    r.btc_dangerous = BTCPSBT_DANGER_HIGH_FEE | BTCPSBT_DANGER_UNKNOWN_CHANGE | BTCPSBT_DANGER_SIGHASH_NOT_ALL;
    p.n_non_change = 2;
    tui_build_sign(&r, &d);
    CHECK(has_box(&d, "! HIGH FEE")); CHECK(has_box(&d, "! UNKNOWN OUTPUT")); CHECK(has_box(&d, "! SIGHASH NOT ALL"));
    CHECK(has_line(&d, "2 outputs leave"));
    CHECK(d.n_pages >= 2 && !d.truncated);
    p.sats_out = 0; p.has_dest = false; r.btc_dangerous = 0;
    tui_build_sign(&r, &d);
    CHECK_STR(line(&d, 0, 0), "BITCOIN " DOT " CONSOLIDATE");
    {
        static const uint8_t msg[] = "Hello World";
        memset(&r, 0, sizeof r);
        r.kind = TUI_SIGN_BTC_MSG; r.msg = msg; r.msg_len = 11;
        tui_build_sign(&r, &d);
        CHECK_STR(line(&d, 0, 0), "BITCOIN " DOT " MESSAGE");
        CHECK_STR(line(&d, 0, 1), "BIP-322 simple");
        CHECK(has_line(&d, "Hello World"));
    }
}

static void test_idle_pin_notice_router(void)
{
    tui_page_t pg;
    tui_idle_t st;
    uint8_t evm[20], pkh[20];
    router_entry_t e;
    hex2bin("9858effd232b4033e47d90003d41ec34ecaeda94", evm, 20);
    hex2bin("c0cebcd6c3d3ca8c75dc5ec62ebe55330ef910e2", pkh, 20);
    memset(&st, 0, sizeof st);
    tui_build_idle(&st, &pg);
    CHECK_STR(pg.l[0].text, "PINCER1 " DOT " NEW DEVICE");
    st.initialized = true; st.locked = true; st.pin_set = true; st.sol = OWNER; st.evm = evm; st.btc_pkh = pkh;
    tui_build_idle(&st, &pg);
    CHECK_STR(pg.l[0].text, "PINCER1 " DOT " LOCKED");
    CHECK(strncmp(pg.l[1].text, "SOL ", 4) == 0);
    CHECK_STR(pg.l[2].text, "EVM 0x9858Ef" ELL "aEda94");
    CHECK_STR(pg.l[3].text, "BTC bc1qcr" ELL "306fyu");
    CHECK_STR(pg.l[4].text, "unlock: PIN rhythm");
    st.locked = false; st.pin_set = false;
    tui_build_idle(&st, &pg);
    CHECK_STR(pg.l[0].text, "PINCER1 " DOT " READY");
    CHECK_EQ(pg.l[4].style, TUI_BOX); CHECK_STR(pg.l[4].text, "! PIN NOT SET");
    /* PIN: one dot per tap, nothing about gaps */
    tui_build_pin("UNLOCK", NULL, NULL, 3, false, &pg);
    CHECK_STR(pg.l[0].text, "PIN " DOT " UNLOCK");
    CHECK_STR(pg.l[1].text, "tap your rhythm");
    CHECK_EQ(pg.l[2].style, TUI_BIG); CHECK_STR(pg.l[2].text, "* * * ");
    tui_build_pin("SET PIN", "limit SOL 5", NULL, 0, true, &pg);
    CHECK_STR(pg.l[1].text, "limit SOL 5");
    CHECK_STR(pg.l[2].text, "tap it again");
    CHECK_STR(pg.l[3].text, "");
    tui_build_pin("X", NULL, NULL, 40, false, &pg); /* capped at 10 dots, never overflows the line */
    CHECK_EQ(display_text_cells(pg.l[2].text) <= 16, 1);
    /* notices and prompts */
    tui_build_notice("DENIED", "claims_mismatch", NULL, &pg);
    CHECK_STR(pg.l[0].text, "DENIED"); CHECK_EQ(pg.l[1].style, TUI_BIG); CHECK_STR(pg.l[1].text, "claims_mismatch");
    tui_build_prompt("ELEVATE", "SOL limit 20 SOL", "one request", &pg);
    CHECK_EQ(pg.n_lines, 3); CHECK_STR(pg.l[2].text, "one request");
    /* router screen: chain, short form boxed, full address wrapped */
    memset(&e, 0, sizeof e); e.chain = ROUTER_CHAIN_EVM; memcpy(e.addr, evm, 20);
    tui_build_router("ADD ROUTER", &e, &pg);
    CHECK_STR(pg.l[0].text, "ADD ROUTER " DOT " EVM");
    CHECK_EQ(pg.l[1].style, TUI_BOX); CHECK_STR(pg.l[1].text, "0x9858Ef" ELL "aEda94");
    CHECK_STR(pg.l[2].text, "0x9858EfFD232B4033E47");
    CHECK_STR(pg.l[3].text, "d90003D41EC34EcaEda94");
    CHECK_EQ(pg.n_lines, 4);
    tui_page_add(&pg, TUI_TEXT, "* * ");
    CHECK_EQ(pg.n_lines, 5); CHECK_STR(pg.l[4].text, "* * ");
    tui_page_add(&pg, TUI_TEXT, "no room"); /* 8+12+8+8+8 = 44, another 8 does not fit: dropped */
    CHECK_EQ(pg.n_lines, 5);
    e.chain = ROUTER_CHAIN_SOL; memcpy(e.addr, JUP, 32);
    tui_build_router("ROUTER ADDED", &e, &pg);
    CHECK_STR(pg.l[0].text, "ROUTER ADDED " DOT " SOLANA");
    { char full[48]; pubkey_to_b58(JUP, full, sizeof full); CHECK_EQ(strlen(full) <= 44, 1);
      CHECK_EQ(pg.n_lines, 2 + (int)((strlen(full) + 20) / 21)); /* every character of the id is on the screen */ }
}

static void test_words_pages(void)
{
    static const char REAL[] = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                               "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon art";
    static const char DECOY[] = "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong";
    tui_page_t pg;
    uint8_t i;
    CHECK_EQ(TUI_WORDS_PAGES, 28);
    CHECK_EQ(tui_build_words_page(0, REAL, DECOY, &pg), TUI_FOOT_NEXT);
    CHECK_STR(pg.l[0].text, "SEED WORDS 1/6");
    CHECK_STR(pg.l[1].text, " 1. abandon"); CHECK_STR(pg.l[4].text, " 4. abandon");
    CHECK_STR(pg.l[5].text, "write these down");
    tui_build_words_page(5, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "SEED WORDS 6/6"); CHECK_STR(pg.l[1].text, "21. abandon"); CHECK_STR(pg.l[4].text, "24. art");
    tui_build_words_page(6, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "SEED " DOT " CHECK");
    tui_build_words_page(7, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "SEED CHECK 1/6"); CHECK_STR(pg.l[5].text, "compare to your copy");
    tui_build_words_page(12, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "SEED CHECK 6/6"); CHECK_STR(pg.l[4].text, "24. art");
    tui_build_words_page(13, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "DECOY " DOT " SECOND SEED");
    CHECK_STR(pg.l[2].text, "duress word or rhythm");
    CHECK_EQ(pg.n_lines, 6);
    tui_build_words_page(14, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "DECOY WORDS 1/6"); CHECK_STR(pg.l[1].text, " 1. zoo");
    tui_build_words_page(19, REAL, DECOY, &pg);
    CHECK_STR(pg.l[4].text, "24. wrong");
    tui_build_words_page(20, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "DECOY " DOT " CHECK");
    tui_build_words_page(26, REAL, DECOY, &pg);
    CHECK_STR(pg.l[0].text, "DECOY CHECK 6/6"); CHECK_STR(pg.l[4].text, "24. wrong");
    CHECK_EQ(tui_build_words_page(27, REAL, DECOY, &pg), TUI_FOOT_FINISH);
    CHECK_STR(pg.l[0].text, "SEED " DOT " DONE");
    /* every real word appears exactly twice across the 28 pages, every decoy word twice, never mixed */
    {
        int real_hits = 0, decoy_hits = 0;
        for (i = 0; i < TUI_WORDS_PAGES; i++) {
            int l;
            tui_build_words_page(i, REAL, DECOY, &pg);
            for (l = 1; l <= 4; l++) {
                if (strstr(pg.l[l].text, "abandon") || strstr(pg.l[l].text, " art")) real_hits++;
                if (strstr(pg.l[l].text, "zoo") || strstr(pg.l[l].text, "wrong")) decoy_hits++;
            }
        }
        CHECK_EQ(real_hits, 48); CHECK_EQ(decoy_hits, 48);
    }
}

/* Rendering: header bar inverted, footer text per page, the final sign page says TOUCH TO SIGN / hold 3s = cancel. */
static void test_render(void)
{
    display_t d;
    tui_doc_t doc;
    tui_page_t pg;
    int x, lit = 0;
    display_init(&d, NULL);
    tui_doc_init(&doc);
    tui_add(&doc, TUI_HEAD, "HEAD"); tui_add(&doc, TUI_BIG, "0.5 SOL"); tui_add(&doc, TUI_BOX, "! APPROVE");
    tui_add(&doc, TUI_TEXT, "a"); tui_add(&doc, TUI_TEXT, "b"); tui_add(&doc, TUI_TEXT, "c"); tui_add(&doc, TUI_TEXT, "d"); /* -> 2 pages */
    CHECK_EQ(doc.n_pages, 2);
    tui_render_doc(&d, &doc, 0, TUI_FOOT_SIGN, true);
    /* header bar: row 7 of the top band is lit across the width (inverted cell background) */
    for (x = 0; x < 128; x++) lit += display_get_pixel(&d, x, 7);
    CHECK_EQ(lit, 128);
    /* big text occupies y 8..23: the '0' glyph's left column lands at x=0 */
    CHECK(display_get_pixel(&d, 0, 9 + 2) || display_get_pixel(&d, 0, 9 + 4));
    /* box outline at y=24 and y=35 */
    CHECK(display_get_pixel(&d, 0, 24)); CHECK(display_get_pixel(&d, 0, 35)); CHECK(display_get_pixel(&d, 30, 24));
    /* footer bar lit at y=48..55, and the second footer row has text ("hold 3s = cancel") */
    lit = 0; for (x = 0; x < 128; x++) lit += display_get_pixel(&d, x, 48);
    CHECK(lit > 100);
    lit = 0; for (x = 0; x < 128; x++) lit += display_get_pixel(&d, x, 59);
    CHECK(lit > 10);
    {
        /* compare footer text by rendering the expected string into a scratch display */
        display_t e;
        display_init(&e, NULL);
        display_fill_rect(&e, 0, 48, 128, 8, true);
        display_text(&e, 1, 48, "TOUCH = next  1/2", true);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 128);
        display_text(&e, 0, 56, "hold 3s = cancel", false);
        CHECK_MEM(&e.fb[7 * 128], &d.fb[7 * 128], 128);
        tui_render_doc(&d, &doc, 1, TUI_FOOT_SIGN, true);
        display_clear(&e);
        display_fill_rect(&e, 0, 48, 128, 8, true);
        display_text(&e, 1, 48, "TOUCH TO SIGN " DOT, true);
        display_text(&e, 0, 56, "hold 3s = cancel", false);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 256);
        /* long-touch footer, not cancelable */
        tui_render_doc(&d, &doc, 1, TUI_FOOT_HOLD, false);
        display_clear(&e);
        display_fill_rect(&e, 0, 48, 128, 8, true);
        display_text(&e, 1, 48, "HOLD 3s TO APPROVE", true);
        display_text(&e, 0, 56, "release = wait", false);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 256);
        /* idle: no footer at all */
        memset(&pg, 0, sizeof pg);
        tui_render(&d, &pg, TUI_FOOT_NONE, 1, 1, false);
        display_clear(&e);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 256);
        /* approve footer (unlock before a PIN exists) */
        tui_render(&d, &pg, TUI_FOOT_APPROVE, 1, 1, false);
        display_clear(&e);
        display_fill_rect(&e, 0, 48, 128, 8, true);
        display_text(&e, 1, 48, "TOUCH TO APPROVE", true);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 256);
        /* PIN footer */
        display_clear(&e);
        tui_render(&d, &pg, TUI_FOOT_PIN, 1, 1, false);
        display_fill_rect(&e, 0, 48, 128, 8, true);
        display_text(&e, 1, 48, "tap " DOT " pause 1.5s = done", true);
        CHECK_MEM(&e.fb[6 * 128], &d.fb[6 * 128], 256);
    }
    /* out-of-range page index renders the last page */
    tui_render_doc(&d, &doc, 9, TUI_FOOT_SIGN, true);
    CHECK(d.dirty);
}

int main(void)
{
    memset(OWNER, 0x11, 32); memset(DEST, 0x22, 32); memset(MINT, 0x66, 32); memset(ATA, 0x41, 32); memset(JUP, 0x99, 32);
    RUN(test_formatting);
    RUN(test_doc_pagination);
    RUN(test_solana_screens);
    RUN(test_evm_screens);
    RUN(test_btc_screens);
    RUN(test_idle_pin_notice_router);
    RUN(test_words_pages);
    RUN(test_render);
    TEST_END();
}
