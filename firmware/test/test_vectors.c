/* The public corpus in /vectors through the controller's own parsers: the same files and the same
 * expectations the daemon test reads (daemon/test/vectors.test.ts). One loop per chain; what a parser does
 * not do by design (resolving Solana lookup tables, typed data with nested structs) is checked as a refusal
 * or a declared danger, never skipped silently. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btcpsbt.h"
#include "cJSON.h"
#include "eip712.h"
#include "evmtx.h"
#include "harness.h"
#include "signer.h"
#include "solmsg.h"
#include "trusted_ui.h"
#include "u256.h"
#include "util.h"

#ifndef PINCER_VECTORS_DIR
#error "PINCER_VECTORS_DIR must point at the vectors directory"
#endif

static const char JUPITER_B58[] = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4";
static const char *EVM_ROUTERS[] = {"111111125421ca6dc452d289314280a0f8842a65", "def1c0ded9bec7f1a1670819833240f027b25eff"};

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    buf[n] = 0;
    fclose(f);
    if (len) *len = (size_t)n;
    return buf;
}

typedef void (*vector_fn)(const char *name, const cJSON *v);

static int walk(const char *chain, vector_fn fn)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", PINCER_VECTORS_DIR, chain);
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "  cannot open %s\n", dir); return 0; }
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL) {
        size_t l = strlen(e->d_name);
        if (l < 6 || strcmp(e->d_name + l - 5, ".json") != 0) continue;
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        char *text = read_file(path, NULL);
        cJSON *v = text ? cJSON_Parse(text) : NULL;
        if (!v) { fprintf(stderr, "  FAIL unreadable %s\n", path); t_fail++; free(text); continue; }
        printf("  %s/%s\n", chain, cJSON_GetObjectItem(v, "name")->valuestring);
        fn(cJSON_GetObjectItem(v, "name")->valuestring, v);
        n++;
        cJSON_Delete(v);
        free(text);
    }
    closedir(d);
    return n;
}

static const cJSON *expect_of(const cJSON *v) { return cJSON_GetObjectItem(v, "expect"); }
static const char *str(const cJSON *o, const char *k) { const cJSON *x = cJSON_GetObjectItem(o, k); return cJSON_IsString(x) ? x->valuestring : NULL; }
static double num(const cJSON *o, const char *k) { const cJSON *x = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(x) ? x->valuedouble : -1; }
static bool refused(const cJSON *v) { const char *c = str(expect_of(v), "controller"); return c && strncmp(c, "refuse", 6) == 0; }
static bool has_warning(const cJSON *v, const char *name)
{
    const cJSON *w = cJSON_GetObjectItem(expect_of(v), "warnings"), *x;
    cJSON_ArrayForEach(x, w) if (cJSON_IsString(x) && strcmp(x->valuestring, name) == 0) return true;
    return false;
}
static int decode_bytes(const cJSON *v, uint8_t *out, size_t cap)
{
    const char *b = str(v, "bytes");
    return b ? base64_decode(b, strlen(b), out, cap) : -1;
}
static uint64_t u64_of(const char *dec) { return dec ? strtoull(dec, NULL, 10) : 0; }

/* ---- solana messages (signMessage): which ones the controller must never sign ---- */
static void solana_msg_vector(const char *name, const cJSON *v)
{
    static uint8_t bytes[1024];
    uint8_t owner[32];
    solmsg_signmsg_t sm;
    const cJSON *e = expect_of(v);
    const char *shown = str(e, "shown");
    bool signs;
    int n = decode_bytes(v, bytes, sizeof bytes);
    CHECK(n > 0);
    CHECK(pubkey_from_b58(str(v, "owner"), owner));
    /* only the real version 1 message is a transaction; as a message it is refused twice over, by the parser and by its first byte */
    CHECK_EQ(solmsg_looks_like_message(bytes, (size_t)n), cJSON_IsTrue(cJSON_GetObjectItem(e, "parses_as_transaction")));
    CHECK_EQ(solmsg_is_ring_key_derivation(bytes, (size_t)n), cJSON_IsTrue(cJSON_GetObjectItem(e, "ring_key_derivation")));
    /* what handle_sign does for kind msg: the size bound, then the shape */
    signs = (size_t)n <= SIGNER_ED25519_MSG_MAX && solmsg_signmsg_check(bytes, (size_t)n, owner, NULL, &sm);
    if (signs == refused(v)) printf("  vector %s: signs=%d\n", name, (int)signs);
    CHECK_EQ(signs, !refused(v));
    if (signs && shown) {
        /* the screen shows exactly this text, and all of it */
        tui_sign_t r;
        static tui_doc_t d;
        CHECK_EQ((int)sm.text_len, (int)strlen(shown));
        CHECK(memcmp(sm.text, shown, sm.text_len) == 0);
        memset(&r, 0, sizeof r);
        r.kind = TUI_SIGN_SOL_MSG; r.msg = sm.text; r.msg_len = sm.text_len;
        r.msg_env = sm.shape == SOLMSG_SIGNMSG_OFFCHAIN_V0 ? 1 : sm.shape == SOLMSG_SIGNMSG_OFFCHAIN_V1 ? 2 : 0;
        r.msg_app = sm.app_domain;
        tui_build_sign(&r, &d);
        CHECK(!d.truncated);
    }
}

/* ---- solana ---- */
static void solana_vector(const char *name, const cJSON *v)
{
    static uint8_t bytes[SOLMSG_MAX_BYTES + 64];
    uint8_t owner[32], jup[32], key[32];
    solmsg_t m;
    solmsg_summary_t s;
    const cJSON *e = expect_of(v);
    (void)name;
    int n = decode_bytes(v, bytes, sizeof bytes);
    CHECK(n > 0);
    CHECK(pubkey_from_b58(str(v, "owner"), owner));
    pubkey_from_b58(JUPITER_B58, jup);
    CHECK_EQ(solmsg_parse(bytes, (size_t)n, &m), SOLMSG_OK);
    CHECK(solmsg_looks_like_message(bytes, (size_t)n));
    solmsg_analyze(&m, owner, &s);
    CHECK(s.fee_payer_is_owner);
    CHECK_U64(s.fee_lamports, u64_of(str(e, "fee_lamports")));
    CHECK_EQ(m.num_required_signatures, (int)num(e, "signers"));
    const cJSON *lookups = cJSON_GetObjectItem(v, "lookup_tables");
    if (lookups) {
        /* the controller cannot resolve lookup tables: it must say so, and what it cannot see is not claimed
         * (a nonce advance whose accounts come from a table is not recognised either) */
        CHECK(m.n_lookup_tables > 0);
        CHECK(s.dangerous & SOLMSG_DANGER_LOOKUP_TABLE);
        return;
    }
    CHECK(!(s.dangerous & SOLMSG_DANGER_LOOKUP_TABLE));
    bool nonce = cJSON_IsTrue(cJSON_GetObjectItem(e, "durable_nonce"));
    CHECK_EQ(solmsg_starts_with_advance_nonce(&m, NULL), nonce);
    CHECK_U64(s.lamports_out, u64_of(str(e, "lamports_out")));
    if (num(e, "lamport_recipients") > 1) {
        /* sol towards several recipients: refused before any claim is compared */
        CHECK(s.multi_dest);
        CHECK(refused(v));
        return;
    }
    const char *dest = str(e, "dest");
    if (s.lamports_out > 0 && dest) {
        CHECK(s.has_dest && !s.multi_dest);
        CHECK(pubkey_from_b58(dest, key));
        CHECK_MEM(s.dest, key, 32);
    }
    const cJSON *tok = cJSON_GetObjectItem(e, "token_out"), *t;
    if (cJSON_GetArraySize(tok) > SOLMSG_MAX_TOKEN_OUT) {
        /* more transfers than the display can list: the controller refuses the message */
        CHECK(s.token_out_overflow);
        CHECK(refused(v));
        return;
    }
    CHECK_EQ(s.n_token_out, cJSON_GetArraySize(tok));
    int i = 0;
    cJSON_ArrayForEach(t, tok) {
        if (i >= s.n_token_out) break;
        CHECK_U64(s.token_out[i].amount, u64_of(str(t, "amount")));
        CHECK(s.token_out[i].has_dest && pubkey_from_b58(str(t, "dest"), key));
        CHECK_MEM(s.token_out[i].dest, key, 32);
        if (cJSON_IsTrue(cJSON_GetObjectItem(t, "mint_in_instruction"))) {
            CHECK(s.token_out[i].has_mint && pubkey_from_b58(str(t, "mint"), key));
            CHECK_MEM(s.token_out[i].mint, key, 32);
        } else CHECK(!s.token_out[i].has_mint);
        i++;
    }
    CHECK_EQ(s.n_programs, cJSON_GetArraySize(cJSON_GetObjectItem(e, "programs")));
    /* unknown_program is policy's call (it needs the router list); the parser gives it the program ids */
    bool unknown = false;
    for (int p = 0; p < s.n_programs; p++)
        if (!solmsg_is_core_program(s.programs[p]) && memcmp(s.programs[p], jup, 32) != 0) unknown = true;
    CHECK_EQ(unknown, has_warning(v, "unknown_program"));
    static const char *names[] = {"set_authority", "approve", "close_account", "assign", "unknown_instruction"};
    for (size_t k = 0; k < sizeof names / sizeof names[0]; k++) {
        uint32_t bit = solmsg_danger_from_name(names[k]);
        CHECK(bit != 0);
        if (((s.dangerous & bit) != 0) != has_warning(v, names[k])) { t_fail++; fprintf(stderr, "  FAIL %s: danger %s parser=%d expected=%d\n", name, names[k], (s.dangerous & bit) != 0, has_warning(v, names[k])); }
        else t_pass++;
    }
}

/* ---- evm ---- */
static void evm_vector(const char *name, const cJSON *v)
{
    static uint8_t bytes[EVMTX_MAX_BYTES];
    uint8_t owner[20], want20[20];
    char dec[U256_DEC_MAX];
    evmtx_t tx;
    const cJSON *e = expect_of(v);
    int n = decode_bytes(v, bytes, sizeof bytes);
    CHECK(n > 0);
    hex2bin(str(v, "owner") + 2, owner, 20);
    CHECK_EQ(evmtx_parse(bytes, (size_t)n, &tx), EVMTX_OK);
    CHECK(evmtx_looks_like_tx(bytes, (size_t)n));
    evmtx_analyze(&tx, owner);
    CHECK_EQ(tx.type, strcmp(str(e, "type"), "legacy") == 0 ? 0 : 2);
    CHECK_U64(tx.chain_id, (uint64_t)num(e, "chain_id"));
    CHECK_U64(tx.nonce, (uint64_t)num(e, "nonce"));
    CHECK_U64(tx.gas_limit, u64_of(str(e, "gas_limit")));
    const char *to = str(e, "to");
    CHECK_EQ(tx.has_to, to != NULL);
    if (to) { hex2bin(to + 2, want20, 20); CHECK_MEM(tx.to, want20, 20); }
    CHECK(u256_to_dec(&tx.value, dec, sizeof dec) > 0);
    CHECK_STR(dec, str(e, "value_wei"));
    CHECK(u256_to_dec(&tx.max_fee_per_gas, dec, sizeof dec) > 0);
    CHECK_STR(dec, str(e, "max_fee_per_gas_wei"));
    CHECK(u256_to_dec(&tx.max_fee_total, dec, sizeof dec) > 0);
    CHECK_STR(dec, str(e, "max_fee_wei"));
    const char *call = str(e, "call");
    evmtx_call_t want = EVMTX_CALL_UNKNOWN;
    if (strcmp(call, "none") == 0) want = EVMTX_CALL_NONE;
    else if (strcmp(call, "transfer") == 0) want = EVMTX_CALL_TRANSFER;
    else if (strcmp(call, "approve") == 0 || strcmp(call, "increaseAllowance") == 0) want = EVMTX_CALL_APPROVE;
    else if (strcmp(call, "transferFrom") == 0) want = EVMTX_CALL_TRANSFER_FROM;
    else if (strcmp(call, "setApprovalForAll") == 0) want = EVMTX_CALL_SET_APPROVAL;
    else if (strcmp(call, "create") == 0) want = EVMTX_CALL_CREATE;
    CHECK_EQ(tx.call, want);
    const cJSON *tok = cJSON_GetObjectItem(e, "token_out");
    if (cJSON_GetArraySize(tok) == 1) {
        const cJSON *t = cJSON_GetArrayItem(tok, 0);
        CHECK(u256_to_dec(&tx.call_amount, dec, sizeof dec) > 0);
        CHECK_STR(dec, str(t, "amount"));
        hex2bin(str(t, "to") + 2, want20, 20);
        CHECK_MEM(tx.call_to, want20, 20);
    }
    const char *spender = str(e, "spender");
    if (spender) {
        hex2bin(spender + 2, want20, 20);
        CHECK_MEM(tx.call_to, want20, 20);
        CHECK(u256_to_dec(&tx.call_amount, dec, sizeof dec) > 0);
        CHECK_STR(dec, str(e, "allowance"));
    }
    if (want == EVMTX_CALL_SET_APPROVAL) { hex2bin(str(e, "call_to") + 2, want20, 20); CHECK_MEM(tx.call_to, want20, 20); }
    /* approve / approve_unlimited / set_approval_for_all come from the parser; unknown_contract is policy's
     * call from the call kind and its router list */
    static const char *names[] = {"approve", "approve_unlimited", "set_approval_for_all"};
    for (size_t k = 0; k < 3; k++) {
        uint32_t bit = evmtx_danger_from_name(names[k]);
        CHECK(bit != 0);
        if (((tx.dangerous & bit) != 0) != has_warning(v, names[k])) { t_fail++; fprintf(stderr, "  FAIL %s: danger %s parser=%d expected=%d\n", name, names[k], (tx.dangerous & bit) != 0, has_warning(v, names[k])); }
        else t_pass++;
    }
    bool router = false;
    if (tx.has_to) {
        char hex[41];
        for (int i = 0; i < 20; i++) sprintf(hex + 2 * i, "%02x", tx.to[i]);
        for (size_t k = 0; k < 2; k++) if (strcmp(hex, EVM_ROUTERS[k]) == 0) router = true;
    }
    bool unknown = tx.call == EVMTX_CALL_CREATE || (tx.call == EVMTX_CALL_UNKNOWN && !router);
    CHECK_EQ(unknown, has_warning(v, "unknown_contract"));
}

/* ---- bitcoin ---- */
static void bitcoin_vector(const char *name, const cJSON *v)
{
    static uint8_t bytes[BTCPSBT_MAX_BYTES];
    uint8_t owner[33], spk[BTCPSBT_MAX_SCRIPT];
    btcpsbt_t p;
    const cJSON *e = expect_of(v);
    (void)name;
    int n = decode_bytes(v, bytes, sizeof bytes);
    CHECK(n > 0);
    int rc = btcpsbt_parse(bytes, (size_t)n, &p);
    if (refused(v)) { CHECK_EQ(rc, BTCPSBT_E_VERSION); return; }
    CHECK_EQ(rc, BTCPSBT_OK);
    const cJSON *o = cJSON_GetObjectItem(v, "owner");
    hex2bin(str(o, "pubkey"), owner, 33);
    CHECK_EQ(btcpsbt_analyze(&p, owner), BTCPSBT_OK);
    CHECK_EQ(p.n_in, (int)num(e, "inputs"));
    CHECK_EQ(p.n_owner_inputs, (int)num(e, "owner_inputs"));
    CHECK_EQ(p.n_out, (int)num(e, "outputs"));
    CHECK_U64(p.sats_in, u64_of(str(e, "sats_in")));
    CHECK_U64(p.sats_out, u64_of(str(e, "sats_out")));
    CHECK_U64(p.fee, u64_of(str(e, "fee_sats")));
    CHECK_U64(p.sats_change, u64_of(str(e, "change_sats")));
    const char *ds = str(e, "dest_script");
    if (ds) {
        size_t l = strlen(ds) / 2;
        CHECK(p.has_dest);
        CHECK_EQ(p.dest_spk_len, (int)l);
        hex2bin(ds, spk, l);
        CHECK_MEM(p.dest_spk, spk, l);
    }
    CHECK_EQ((p.dangerous & BTCPSBT_DANGER_HIGH_FEE) != 0, has_warning(v, "high_fee"));
    CHECK_EQ((p.dangerous & BTCPSBT_DANGER_SIGHASH_NOT_ALL) != 0, has_warning(v, "sighash_not_all"));
    /* unknown_change is policy's call against claims.dest; the parser's count says whether it would arise */
    CHECK_EQ(p.n_non_change > 1, has_warning(v, "unknown_change"));
}

/* ---- eip-712 ---- */
static void typed_vector(const char *name, const cJSON *v)
{
    uint8_t digest[32], want[32];
    eip712_desc_t d;
    const cJSON *e = expect_of(v);
    (void)name;
    const cJSON *td = cJSON_GetObjectItem(v, "typed_data");
    CHECK(td != NULL);
    int rc = eip712_digest(td, digest);
    if (refused(v)) { CHECK_EQ(rc, EIP712_E_UNSUPPORTED); return; }
    CHECK_EQ(rc, EIP712_OK);
    hex2bin(str(e, "digest") + 2, want, 32);
    CHECK_MEM(digest, want, 32);
    CHECK_EQ(eip712_describe(td, &d), EIP712_OK);
    CHECK_EQ(d.permit_like, str(e, "spender") != NULL);
    CHECK(!d.fields_truncated);
}

int main(void)
{
    printf("- solana_msg\n");
    CHECK(walk("solana_msg", solana_msg_vector) >= 35);
    printf("- solana\n");
    CHECK(walk("solana", solana_vector) >= 11);
    printf("- evm\n");
    CHECK(walk("evm", evm_vector) >= 12);
    printf("- bitcoin\n");
    CHECK(walk("bitcoin", bitcoin_vector) >= 7);
    printf("- eip-712\n");
    CHECK(walk("evm_typed", typed_vector) >= 4);
    TEST_END();
}
