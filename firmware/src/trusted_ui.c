#include "trusted_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "chainaddr.h"
#include "util.h"

#define DOT "\xC2\xB7"
#define ELL "\xE2\x80\xA6"

static int style_height(uint8_t style)
{
    switch (style) {
    case TUI_BIG: return 16;
    case TUI_BOX: return 12;
    default: return 8;
    }
}

static int page_height(const tui_page_t *p)
{
    int h = 0, i;
    for (i = 0; i < p->n_lines; i++) h += style_height(p->l[i].style);
    return h;
}

/* Copies at most DISPLAY_COLS cells of UTF-8 text, ending with "…" when cut. */
static void fit_text(char *dst, size_t cap, const char *src, size_t max_cells)
{
    size_t cells = 0, o = 0;
    const char *s = src;
    bool cut = display_text_cells(src) > max_cells;
    size_t keep = cut ? max_cells - 1 : max_cells;
    while (*s && cells < keep) {
        const char *start = s;
        size_t n;
        if ((uint8_t)s[0] == 0xC2 && (uint8_t)s[1] == 0xB7) s += 2;
        else if ((uint8_t)s[0] == 0xE2 && (uint8_t)s[1] == 0x80 && (uint8_t)s[2] == 0xA6) s += 3;
        else if ((uint8_t)s[0] >= 0x80) { s++; while ((uint8_t)*s >= 0x80 && (uint8_t)*s < 0xC0) s++; }
        else s++;
        n = (size_t)(s - start);
        if (o + n + 4 > cap) break;
        memcpy(dst + o, start, n);
        o += n;
        cells++;
    }
    if (cut && o + 4 <= cap) { memcpy(dst + o, ELL, 3); o += 3; }
    dst[o] = 0;
}

void tui_doc_init(tui_doc_t *doc) { memset(doc, 0, sizeof *doc); }

void tui_add(tui_doc_t *doc, tui_style_t style, const char *text)
{
    tui_page_t *pg;
    tui_line_t *ln;
    size_t max_cells = style == TUI_BIG ? DISPLAY_BIG_COLS : (style == TUI_BOX ? DISPLAY_COLS - 1 : DISPLAY_COLS);
    if (doc->truncated) return;
    if (doc->n_pages == 0) doc->n_pages = 1;
    pg = &doc->p[doc->n_pages - 1];
    if (pg->n_lines >= TUI_MAX_LINES || page_height(pg) + style_height(style) > TUI_CONTENT_H) {
        if (doc->n_pages >= TUI_MAX_PAGES) { doc->truncated = true; return; }
        pg = &doc->p[doc->n_pages++];
    }
    ln = &pg->l[pg->n_lines++];
    ln->style = (uint8_t)style;
    fit_text(ln->text, sizeof ln->text, text, max_cells);
}

/* Like tui_add but never cuts: text longer than a line continues on the next line(s). Used for amounts and
 * digests, where a hidden digit would be a lie. */
void tui_add_wrap(tui_doc_t *doc, tui_style_t style, const char *text)
{
    size_t max_cells = style == TUI_BIG ? DISPLAY_BIG_COLS : DISPLAY_COLS;
    char part[TUI_LINE_BYTES];
    const char *s = text;
    while (*s) {
        size_t cells = 0, o = 0;
        while (*s && cells < max_cells && o + 4 < sizeof part) {
            size_t n = ((uint8_t)s[0] == 0xC2 && (uint8_t)s[1] == 0xB7) ? 2
                     : ((uint8_t)s[0] == 0xE2 && (uint8_t)s[1] == 0x80 && (uint8_t)s[2] == 0xA6) ? 3 : 1;
            memcpy(part + o, s, n);
            o += n; s += n; cells++;
        }
        part[o] = 0;
        tui_add(doc, style, part);
        if (doc->truncated) return;
    }
}

/* An amount: big when it fits the 16-cell big face, otherwise small and wrapped, never cut. */
static void add_amount(tui_doc_t *doc, const char *amt, const char *unit)
{
    char buf[U256_DEC_MAX + 16];
    snprintf(buf, sizeof buf, "%s %s", amt, unit);
    if (display_text_cells(buf) <= DISPLAY_BIG_COLS) tui_add(doc, TUI_BIG, buf);
    else tui_add_wrap(doc, TUI_TEXT, buf);
}

void tui_addf(tui_doc_t *doc, tui_style_t style, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    tui_add(doc, style, buf);
}

/* single-page helpers */
static void page_add(tui_page_t *pg, tui_style_t style, const char *text)
{
    tui_line_t *ln;
    size_t max_cells = style == TUI_BIG ? DISPLAY_BIG_COLS : (style == TUI_BOX ? DISPLAY_COLS - 1 : DISPLAY_COLS);
    if (pg->n_lines >= TUI_MAX_LINES || page_height(pg) + style_height(style) > TUI_CONTENT_H) return;
    ln = &pg->l[pg->n_lines++];
    ln->style = (uint8_t)style;
    fit_text(ln->text, sizeof ln->text, text, max_cells);
}

static void page_addf(tui_page_t *pg, tui_style_t style, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    page_add(pg, style, buf);
}

/* ---- formatting ---- */

static void trim_units(char *s)
{
    char *dot = strchr(s, '.');
    size_t n;
    if (!dot) return;
    n = strlen(s);
    while (n > 0 && s[n - 1] == '0') s[--n] = 0;
    if (n > 0 && s[n - 1] == '.') s[--n] = 0;
}

/* `dec` is a decimal integer string; inserts the point `decimals` digits from the right. */
static void insert_point(const char *dec, uint8_t decimals, char *out, size_t cap)
{
    size_t n = strlen(dec), o = 0, i;
    if (decimals == 0) { snprintf(out, cap, "%s", dec); return; }
    if (n <= (size_t)decimals) {
        size_t zeros = (size_t)decimals - n;
        if (cap < 3u + decimals) { out[0] = 0; return; }
        out[o++] = '0'; out[o++] = '.';
        for (i = 0; i < zeros; i++) out[o++] = '0';
        memcpy(out + o, dec, n); o += n;
        out[o] = 0;
    } else {
        size_t ip = n - decimals;
        if (cap < n + 2) { out[0] = 0; return; }
        memcpy(out, dec, ip); o = ip;
        out[o++] = '.';
        memcpy(out + o, dec + ip, decimals); o += decimals;
        out[o] = 0;
    }
    trim_units(out);
}

void tui_fmt_units(uint64_t v, uint8_t decimals, char *out, size_t cap)
{
    char dec[21];
    u64_to_dec(v, dec, sizeof dec);
    insert_point(dec, decimals, out, cap);
}

void tui_fmt_units_u256(const u256_t *v, uint8_t decimals, char *out, size_t cap)
{
    char dec[U256_DEC_MAX];
    u256_to_dec(v, dec, sizeof dec);
    insert_point(dec, decimals, out, cap);
}

void tui_short(const char *full, char *out, size_t cap)
{
    size_t n = strlen(full);
    if (n <= 13) { snprintf(out, cap, "%s", full); return; }
    if (cap < 6 + 3 + 6 + 1) { out[0] = 0; return; }
    memcpy(out, full, 6);
    memcpy(out + 6, ELL, 3);
    memcpy(out + 9, full + n - 6, 6);
    out[15] = 0;
}

void tui_short_b58(const uint8_t key[32], char *out, size_t cap)
{
    char s[48];
    pubkey_to_b58(key, s, sizeof s);
    tui_short(s, out, cap);
}

void tui_short_evm(const uint8_t addr[20], char *out, size_t cap)
{
    char hex[EVM_ADDR_HEX_LEN], sh[20];
    evm_address_to_hex(addr, hex);
    tui_short(hex + 2, sh, sizeof sh);
    snprintf(out, cap, "0x%s", sh);
}

void tui_short_btc_script(const uint8_t *spk, size_t len, char *out, size_t cap)
{
    char addr[BTC_ADDR_MAX];
    if (btc_script_to_address(spk, len, addr)) tui_short(addr, out, cap);
    else snprintf(out, cap, "(no address)");
}

const char *tui_evm_chain_name(uint64_t chain_id)
{
    switch (chain_id) {
    case 1: return "ETHEREUM";
    case 10: return "OPTIMISM";
    case 56: return "BNB CHAIN";
    case 137: return "POLYGON";
    case 8453: return "BASE";
    case 42161: return "ARBITRUM";
    case 43114: return "AVALANCHE";
    default: return NULL;
    }
}

const char *tui_evm_unit(uint64_t chain_id)
{
    switch (chain_id) {
    case 1: case 10: case 8453: case 42161: return "ETH";
    case 56: return "BNB";
    case 137: return "POL";
    case 43114: return "AVAX";
    default: return "NATIVE";
    }
}

/* ---- sign documents ---- */

static void add_danger(tui_doc_t *doc, const char *name) { tui_addf(doc, TUI_BOX, "! %s", name); }

static void add_sol_dangers(tui_doc_t *doc, uint32_t flags)
{
    if (flags & SOLMSG_DANGER_UNKNOWN_PROGRAM) add_danger(doc, "UNKNOWN PROGRAM");
    if (flags & SOLMSG_DANGER_SET_AUTHORITY) add_danger(doc, "SET AUTHORITY");
    if (flags & SOLMSG_DANGER_APPROVE) add_danger(doc, "APPROVE (delegate)");
    if (flags & SOLMSG_DANGER_CLOSE_ACCOUNT) add_danger(doc, "CLOSE ACCOUNT");
    if (flags & SOLMSG_DANGER_ASSIGN) add_danger(doc, "ASSIGN OWNER");
    if (flags & SOLMSG_DANGER_LOOKUP_TABLE) add_danger(doc, "LOOKUP TABLE");
}

static void build_sol_tx(const tui_sign_t *r, tui_doc_t *doc, const char *head)
{
    const solmsg_summary_t *s = r->sol;
    char amt[32], sh[64];
    uint8_t i;
    tui_add(doc, TUI_HEAD, head);
    if (s->lamports_out > 0) {
        tui_fmt_units(s->lamports_out, 9, amt, sizeof amt);
        add_amount(doc, amt, "SOL");
        if (s->has_dest) { tui_short_b58(s->dest, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "to %s", sh); }
    } else if (s->n_token_out == 0) {
        tui_add(doc, TUI_TEXT, "no SOL leaves");
    }
    add_sol_dangers(doc, r->sol_dangerous);
    for (i = 0; i < s->n_token_out; i++) {
        const solmsg_token_out_t *t = &s->token_out[i];
        u64_to_dec(t->amount, amt, sizeof amt);
        snprintf(sh, sizeof sh, "token %s units", amt); /* <= 20 digits + text: fits one line */
        tui_add_wrap(doc, TUI_TEXT, sh);
        if (t->has_mint) { tui_short_b58(t->mint, sh, sizeof sh); tui_addf(doc, TUI_TEXT, " mint %s", sh); }
        else tui_add(doc, TUI_TEXT, " mint unknown");
        if (t->has_dest) { tui_short_b58(t->dest, sh, sizeof sh); tui_addf(doc, TUI_TEXT, " to %s", sh); }
        else tui_add(doc, TUI_TEXT, " burn");
    }
    if (s->token_out_overflow) add_danger(doc, "MORE TOKEN MOVES");
    for (i = 0; i < s->n_programs; i++) {
        if (solmsg_is_core_program(s->programs[i])) continue;
        tui_short_b58(s->programs[i], sh, sizeof sh);
        tui_addf(doc, TUI_TEXT, "prog %s", sh);
    }
    if (s->programs_overflow) add_danger(doc, "MORE PROGRAMS");
}

static void build_evm_tx(const tui_sign_t *r, tui_doc_t *doc)
{
    const evmtx_t *tx = r->evm;
    const char *name = tui_evm_chain_name(tx->chain_id), *unit = tui_evm_unit(tx->chain_id);
    char amt[U256_DEC_MAX + 2], sh[24], buf[U256_DEC_MAX + 32];
    const char *verb = tx->call == EVMTX_CALL_NONE ? "SEND" : tx->call == EVMTX_CALL_CREATE ? "CREATE" : "CALL";
    if (name) tui_addf(doc, TUI_HEAD, "%s " DOT " %s", name, verb);
    else tui_addf(doc, TUI_HEAD, "CHAIN %llu " DOT " %s", (unsigned long long)tx->chain_id, verb);
    if (!u256_is_zero(&tx->value) || tx->call == EVMTX_CALL_NONE) {
        tui_fmt_units_u256(&tx->value, 18, amt, sizeof amt);
        add_amount(doc, amt, unit);
    }
    if (tx->has_to) { tui_short_evm(tx->to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "to %s", sh); }
    tui_addf(doc, TUI_TEXT, "chainId %llu", (unsigned long long)tx->chain_id);
    tui_addf(doc, TUI_TEXT, "nonce %llu", (unsigned long long)tx->nonce);
    tui_fmt_units_u256(&tx->max_fee_total, 18, amt, sizeof amt);
    snprintf(buf, sizeof buf, "fee max %s %s", amt, unit);
    tui_add_wrap(doc, TUI_TEXT, buf);
    if (r->evm_dangerous & EVMTX_DANGER_UNKNOWN_CONTRACT) add_danger(doc, tx->call == EVMTX_CALL_CREATE ? "CONTRACT CREATE" : "UNKNOWN CONTRACT");
    switch (tx->call) {
    case EVMTX_CALL_TRANSFER:
    case EVMTX_CALL_TRANSFER_FROM:
        tui_add(doc, TUI_TEXT, tx->call == EVMTX_CALL_TRANSFER ? "ERC20 transfer" : "ERC20 transferFrom");
        u256_to_dec(&tx->call_amount, amt, sizeof amt);
        snprintf(buf, sizeof buf, "%s units", amt);
        tui_add_wrap(doc, TUI_TEXT, buf);
        tui_short_evm(tx->to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "token %s", sh);
        tui_short_evm(tx->call_to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "to %s", sh);
        if (tx->call == EVMTX_CALL_TRANSFER_FROM) { tui_short_evm(tx->call_from, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "from %s", sh); }
        break;
    case EVMTX_CALL_APPROVE:
        add_danger(doc, (r->evm_dangerous & EVMTX_DANGER_APPROVE_UNLIMITED) ? "APPROVE UNLIMITED" : "APPROVE");
        tui_add(doc, TUI_TEXT, "spender:");
        tui_short_evm(tx->call_to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, " %s", sh);
        if (!(r->evm_dangerous & EVMTX_DANGER_APPROVE_UNLIMITED)) {
            u256_to_dec(&tx->call_amount, amt, sizeof amt);
            snprintf(buf, sizeof buf, "allow %s units", amt);
            tui_add_wrap(doc, TUI_TEXT, buf);
        }
        tui_short_evm(tx->to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "token %s", sh);
        break;
    case EVMTX_CALL_SET_APPROVAL:
        if (tx->call_flag) add_danger(doc, "SET APPROVAL ALL");
        else tui_add(doc, TUI_TEXT, "revoke operator");
        tui_add(doc, TUI_TEXT, "operator:");
        tui_short_evm(tx->call_to, sh, sizeof sh); tui_addf(doc, TUI_TEXT, " %s", sh);
        break;
    case EVMTX_CALL_UNKNOWN:
        if (!(r->evm_dangerous & EVMTX_DANGER_UNKNOWN_CONTRACT)) tui_add(doc, TUI_TEXT, "known router call");
        tui_addf(doc, TUI_TEXT, " calldata %u bytes", (unsigned)tx->data_len);
        break;
    default:
        break;
    }
}

static void build_btc(const tui_sign_t *r, tui_doc_t *doc)
{
    const btcpsbt_t *p = r->btc;
    char amt[32], sh[24];
    tui_add(doc, TUI_HEAD, p->sats_out ? "BITCOIN " DOT " SEND" : "BITCOIN " DOT " CONSOLIDATE");
    tui_fmt_units(p->sats_out, 8, amt, sizeof amt);
    add_amount(doc, amt, "BTC");
    if (p->has_dest) { tui_short_btc_script(p->dest_spk, p->dest_spk_len, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "to %s", sh); }
    tui_fmt_units(p->fee, 8, amt, sizeof amt);
    tui_addf(doc, TUI_TEXT, "fee %s BTC", amt);
    tui_addf(doc, TUI_TEXT, " = %llu sat", (unsigned long long)p->fee);
    tui_addf(doc, TUI_TEXT, "inputs %u", (unsigned)p->n_owner_inputs);
    tui_fmt_units(p->sats_change, 8, amt, sizeof amt);
    tui_addf(doc, TUI_TEXT, "change %s BTC", amt);
    if (r->btc_dangerous & BTCPSBT_DANGER_HIGH_FEE) add_danger(doc, "HIGH FEE");
    if (r->btc_dangerous & BTCPSBT_DANGER_UNKNOWN_CHANGE) add_danger(doc, "UNKNOWN OUTPUT");
    if (r->btc_dangerous & BTCPSBT_DANGER_SIGHASH_NOT_ALL) add_danger(doc, "SIGHASH NOT ALL");
    if (p->n_non_change > 1) tui_addf(doc, TUI_TEXT, "%u outputs leave", (unsigned)p->n_non_change);
}

static void add_msg_preview(tui_doc_t *doc, const uint8_t *msg, size_t len)
{
    size_t i, printable = 0;
    char line[24];
    tui_addf(doc, TUI_TEXT, "%u bytes", (unsigned)len);
    for (i = 0; i < len; i++) if (msg[i] >= 0x20 && msg[i] < 0x7f) printable++;
    if (len > 0 && printable == len) {
        size_t o = 0;
        tui_add(doc, TUI_TEXT, "text:");
        for (i = 0; i < len && i < 2 * DISPLAY_COLS; i++) {
            line[o++] = (char)msg[i];
            if (o == DISPLAY_COLS || i + 1 == len) { line[o] = 0; tui_add(doc, TUI_TEXT, line); o = 0; }
        }
        if (len > 2 * DISPLAY_COLS) tui_add(doc, TUI_TEXT, ELL);
    } else {
        static const char HEX[] = "0123456789abcdef";
        size_t o = 0;
        for (i = 0; i < len && i < 8; i++) { line[o++] = HEX[msg[i] >> 4]; line[o++] = HEX[msg[i] & 15]; }
        line[o] = 0;
        tui_addf(doc, TUI_TEXT, "hex %s%s", line, len > 8 ? ELL : "");
    }
}

/* The whole 32-byte digest, 8 bytes per line: it is what gets signed, so no digit is hidden. */
static void add_digest(tui_doc_t *doc, const uint8_t *digest)
{
    static const char HEX[] = "0123456789abcdef";
    char line[17];
    int i, j;
    tui_add(doc, TUI_TEXT, "digest:");
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 8; j++) { line[2 * j] = HEX[digest[8 * i + j] >> 4]; line[2 * j + 1] = HEX[digest[8 * i + j] & 15]; }
        line[16] = 0;
        tui_add(doc, TUI_TEXT, line);
    }
}

void tui_build_sign(const tui_sign_t *r, tui_doc_t *doc)
{
    char sh[24];
    tui_doc_init(doc);
    switch (r->kind) {
    case TUI_SIGN_SOL_TX:
        build_sol_tx(r, doc, r->sol->lamports_out ? "SOLANA " DOT " SEND" : "SOLANA " DOT " TRANSACTION");
        break;
    case TUI_SIGN_INHERITANCE:
        build_sol_tx(r, doc, r->inh_replace ? "INHERITANCE " DOT " REPLACE" : "INHERITANCE " DOT " ARM");
        tui_addf(doc, TUI_TEXT, "after %u days idle", (unsigned)r->inh_days);
        if (r->inh_heir) { tui_short_b58(r->inh_heir, sh, sizeof sh); tui_addf(doc, TUI_TEXT, "heir %s", sh); }
        break;
    case TUI_SIGN_SOL_MSG:
        tui_add(doc, TUI_HEAD, "SOLANA " DOT " MESSAGE");
        add_msg_preview(doc, r->msg, r->msg_len);
        break;
    case TUI_SIGN_EVM_TX:
        build_evm_tx(r, doc);
        break;
    case TUI_SIGN_EVM_MSG:
        tui_add(doc, TUI_HEAD, "EVM " DOT " MESSAGE");
        add_msg_preview(doc, r->msg, r->msg_len);
        break;
    case TUI_SIGN_EVM_TYPED:
        tui_add(doc, TUI_HEAD, "EVM " DOT " TYPED DATA");
        if (r->digest) add_digest(doc, r->digest);
        break;
    case TUI_SIGN_BTC_PSBT:
        build_btc(r, doc);
        break;
    case TUI_SIGN_BTC_MSG:
        tui_add(doc, TUI_HEAD, "BITCOIN " DOT " MESSAGE");
        tui_add(doc, TUI_TEXT, "BIP-322 simple");
        add_msg_preview(doc, r->msg, r->msg_len);
        break;
    }
}

tui_foot_t tui_sign_footer(const tui_sign_t *r)
{
    if (r->kind == TUI_SIGN_INHERITANCE && !r->inh_replace) return TUI_FOOT_HOLD;
    return TUI_FOOT_SIGN;
}

/* ---- single screens ---- */

void tui_build_idle(const tui_idle_t *st, tui_page_t *pg)
{
    char sh[24];
    memset(pg, 0, sizeof *pg);
    if (!st->initialized) {
        page_add(pg, TUI_HEAD, "PINCER1 " DOT " NEW DEVICE");
        page_add(pg, TUI_TEXT, "no keys yet");
        page_add(pg, TUI_TEXT, "run init from the");
        page_add(pg, TUI_TEXT, "setup app, then");
        page_add(pg, TUI_TEXT, "hold the pad 3s");
        return;
    }
    page_addf(pg, TUI_HEAD, "PINCER1 " DOT " %s", st->locked ? "LOCKED" : "READY");
    if (st->sol) { tui_short_b58(st->sol, sh, sizeof sh); page_addf(pg, TUI_TEXT, "SOL %s", sh); }
    if (st->evm) { tui_short_evm(st->evm, sh, sizeof sh); page_addf(pg, TUI_TEXT, "EVM %s", sh); }
    if (st->btc_pkh) {
        uint8_t spk[22];
        btc_p2wpkh_script(st->btc_pkh, spk);
        tui_short_btc_script(spk, 22, sh, sizeof sh);
        page_addf(pg, TUI_TEXT, "BTC %s", sh);
    }
    if (!st->pin_set) page_add(pg, TUI_BOX, "! PIN NOT SET");
    else page_add(pg, TUI_TEXT, st->locked ? "unlock: PIN rhythm" : "idle");
}

void tui_build_pin(const char *what, const char *detail, const char *detail2, uint8_t taps, bool confirm, tui_page_t *pg)
{
    char dots[TUI_LINE_BYTES];
    size_t i, o = 0;
    memset(pg, 0, sizeof *pg);
    page_addf(pg, TUI_HEAD, "PIN " DOT " %s", what);
    if (detail) page_add(pg, TUI_TEXT, detail);
    if (detail2) page_add(pg, TUI_TEXT, detail2);
    page_add(pg, TUI_TEXT, confirm ? "tap it again" : "tap your rhythm");
    if (taps > 10) taps = 10;
    for (i = 0; i < taps; i++) { dots[o++] = '*'; dots[o++] = ' '; }
    dots[o] = 0;
    page_add(pg, TUI_BIG, dots); /* one dot per tap; the gaps are never shown */
}

void tui_build_notice(const char *head, const char *line1, const char *line2, tui_page_t *pg)
{
    memset(pg, 0, sizeof *pg);
    page_add(pg, TUI_HEAD, head);
    if (line1) page_add(pg, TUI_BIG, line1);
    if (line2) page_add(pg, TUI_TEXT, line2);
}

void tui_build_prompt(const char *head, const char *line1, const char *line2, tui_page_t *pg)
{
    memset(pg, 0, sizeof *pg);
    page_add(pg, TUI_HEAD, head);
    if (line1) page_add(pg, TUI_TEXT, line1);
    if (line2) page_add(pg, TUI_TEXT, line2);
}

void tui_page_add(tui_page_t *pg, tui_style_t style, const char *text) { page_add(pg, style, text); }

void tui_build_router(const char *head, const router_entry_t *e, tui_page_t *pg)
{
    char full[48], sh[24];
    memset(pg, 0, sizeof *pg);
    page_addf(pg, TUI_HEAD, "%s " DOT " %s", head, e->chain == ROUTER_CHAIN_SOL ? "SOLANA" : "EVM");
    if (e->chain == ROUTER_CHAIN_SOL) {
        pubkey_to_b58(e->addr, full, sizeof full);
        tui_short_b58(e->addr, sh, sizeof sh);
    } else {
        evm_address_to_hex(e->addr, full);
        tui_short_evm(e->addr, sh, sizeof sh);
    }
    page_add(pg, TUI_BOX, sh);
    /* the full address, wrapped at 21 cells, so it can be compared character by character */
    {
        size_t n = strlen(full), i;
        char part[DISPLAY_COLS + 1];
        for (i = 0; i < n; i += DISPLAY_COLS) {
            size_t k = n - i < DISPLAY_COLS ? n - i : DISPLAY_COLS;
            memcpy(part, full + i, k); part[k] = 0;
            page_add(pg, TUI_TEXT, part);
        }
    }
}

/* ---- seed words ---- */

/* word `i` (0..23) of a space separated mnemonic */
static void nth_word(const char *mnemonic, uint8_t i, char *out, size_t cap)
{
    const char *s = mnemonic, *e;
    size_t n;
    out[0] = 0;
    if (!s) return;
    while (i-- && *s) { s = strchr(s, ' '); if (!s) return; s++; }
    e = strchr(s, ' ');
    n = e ? (size_t)(e - s) : strlen(s);
    if (n + 1 > cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static void words_page(tui_page_t *pg, const char *set_name, const char *mnemonic, uint8_t group, bool again)
{
    uint8_t i;
    char w[12];
    page_addf(pg, TUI_HEAD, "%s %s %u/6", set_name, again ? "CHECK" : "WORDS", (unsigned)group + 1);
    for (i = 0; i < 4; i++) {
        uint8_t idx = (uint8_t)(group * 4 + i);
        nth_word(mnemonic, idx, w, sizeof w);
        page_addf(pg, TUI_TEXT, "%2u. %s", (unsigned)idx + 1, w);
    }
    page_add(pg, TUI_TEXT, again ? "compare to your copy" : "write these down");
}

tui_foot_t tui_build_words_page(uint8_t idx, const char *real, const char *decoy, tui_page_t *pg)
{
    memset(pg, 0, sizeof *pg);
    if (idx < 6) { words_page(pg, "SEED", real, idx, false); return TUI_FOOT_NEXT; }
    if (idx == 6) {
        page_add(pg, TUI_HEAD, "SEED " DOT " CHECK");
        page_add(pg, TUI_TEXT, "all 24 written down?");
        page_add(pg, TUI_TEXT, "they will show once");
        page_add(pg, TUI_TEXT, "more, compare each");
        page_add(pg, TUI_TEXT, "word with your copy");
        return TUI_FOOT_NEXT;
    }
    if (idx < 13) { words_page(pg, "SEED", real, (uint8_t)(idx - 7), true); return TUI_FOOT_NEXT; }
    if (idx == 13) {
        page_add(pg, TUI_HEAD, "DECOY " DOT " SECOND SEED");
        page_add(pg, TUI_TEXT, "a separate wallet the");
        page_add(pg, TUI_TEXT, "duress word or rhythm");
        page_add(pg, TUI_TEXT, "switches to. keep its");
        page_add(pg, TUI_TEXT, "words apart from the");
        page_add(pg, TUI_TEXT, "first set");
        return TUI_FOOT_NEXT;
    }
    if (idx < 20) { words_page(pg, "DECOY", decoy, (uint8_t)(idx - 14), false); return TUI_FOOT_NEXT; }
    if (idx == 20) {
        page_add(pg, TUI_HEAD, "DECOY " DOT " CHECK");
        page_add(pg, TUI_TEXT, "decoy words written?");
        page_add(pg, TUI_TEXT, "they show once more");
        return TUI_FOOT_NEXT;
    }
    if (idx < 27) { words_page(pg, "DECOY", decoy, (uint8_t)(idx - 21), true); return TUI_FOOT_NEXT; }
    page_add(pg, TUI_HEAD, "SEED " DOT " DONE");
    page_add(pg, TUI_TEXT, "the words never leave");
    page_add(pg, TUI_TEXT, "this device again.");
    page_add(pg, TUI_TEXT, "hold 3s to start over");
    page_add(pg, TUI_TEXT, "(keys are destroyed)");
    return TUI_FOOT_FINISH;
}

/* ---- rendering ---- */

static void render_line(display_t *d, int y, const tui_line_t *ln)
{
    switch (ln->style) {
    case TUI_HEAD:
        display_fill_rect(d, 0, y, DISPLAY_W, 8, true);
        display_text(d, 1, y, ln->text, true);
        break;
    case TUI_BIG:
        display_text_big(d, 0, y, ln->text);
        break;
    case TUI_BOX: {
        int w = (int)display_text_cells(ln->text) * DISPLAY_CELL_W + 5;
        if (w > DISPLAY_W) w = DISPLAY_W;
        display_rect(d, 0, y, w, 12, true);
        display_text(d, 3, y + 2, ln->text, false);
        break;
    }
    default:
        display_text(d, 0, y, ln->text, false);
        break;
    }
}

static void render_footer(display_t *d, tui_foot_t foot, uint8_t page, uint8_t pages, bool cancelable)
{
    char top[TUI_LINE_BYTES];
    const char *bottom = cancelable ? "hold 3s = cancel" : "";
    if (pages > 1 && page < pages) {
        snprintf(top, sizeof top, "TOUCH = next  %u/%u", (unsigned)page, (unsigned)pages);
    } else {
        switch (foot) {
        case TUI_FOOT_SIGN: snprintf(top, sizeof top, "TOUCH TO SIGN " DOT); break;
        case TUI_FOOT_HOLD: snprintf(top, sizeof top, "HOLD 3s TO APPROVE"); bottom = cancelable ? "hold 3s = cancel" : "release = wait"; break;
        case TUI_FOOT_NEXT: snprintf(top, sizeof top, "TOUCH = next"); break;
        case TUI_FOOT_FINISH: snprintf(top, sizeof top, "TOUCH TO FINISH " DOT); break;
        case TUI_FOOT_APPROVE: snprintf(top, sizeof top, "TOUCH TO APPROVE"); bottom = ""; break;
        case TUI_FOOT_PIN: snprintf(top, sizeof top, "tap " DOT " pause 1.5s = done"); bottom = ""; break;
        default: return;
        }
    }
    display_fill_rect(d, 0, TUI_FOOTER_Y, DISPLAY_W, 8, true);
    display_text(d, 1, TUI_FOOTER_Y, top, true);
    display_text(d, 0, TUI_FOOTER_Y + 8, bottom, false);
}

void tui_render(display_t *d, const tui_page_t *pg, tui_foot_t foot, uint8_t page, uint8_t pages, bool cancelable)
{
    int y = 0, i;
    display_clear(d);
    for (i = 0; i < pg->n_lines && y < TUI_CONTENT_H; i++) {
        render_line(d, y, &pg->l[i]);
        y += style_height(pg->l[i].style);
    }
    render_footer(d, foot, page, pages, cancelable);
}

void tui_render_doc(display_t *d, const tui_doc_t *doc, uint8_t page_idx, tui_foot_t final_foot, bool cancelable)
{
    if (page_idx >= doc->n_pages) page_idx = doc->n_pages ? (uint8_t)(doc->n_pages - 1) : 0;
    tui_render(d, &doc->p[page_idx], final_foot, (uint8_t)(page_idx + 1), doc->n_pages, cancelable);
}
