#include "policy.h"

#include <string.h>

static bool claims_have_program(const policy_claims_t *c, const uint8_t pid[32])
{
    uint8_t i;
    for (i = 0; i < c->n_programs; i++)
        if (memcmp(c->programs[i], pid, 32) == 0) return true;
    return false;
}

static uint64_t sat_add(uint64_t a, uint64_t b) { return (a > UINT64_MAX - b) ? UINT64_MAX : a + b; }

static bool claimed_amount_for_mint(const policy_claims_t *c, const uint8_t mint[32], uint64_t *out)
{
    uint8_t i;
    uint64_t sum = 0;
    bool found = false;
    for (i = 0; i < c->n_token_out; i++) {
        if (memcmp(c->token_out[i].mint, mint, 32) == 0) {
            sum = sat_add(sum, c->token_out[i].amount);
            found = true;
        }
    }
    *out = sum;
    return found;
}

/* A parsed transfer's recipient token account must appear in a claimed entry: for a known mint, in an entry
 * of that mint; for a plain Token.transfer (mint unknown on the controller), in any entry. */
static bool claims_cover_dest(const policy_claims_t *c, const solmsg_token_out_t *t)
{
    uint8_t i;
    for (i = 0; i < c->n_token_out; i++) {
        if (!c->token_out[i].has_dest) continue;
        if (t->has_mint && memcmp(c->token_out[i].mint, t->mint, 32) != 0) continue;
        if (memcmp(c->token_out[i].dest, t->dest, 32) == 0) return true;
    }
    return false;
}

static policy_verdict_t check_tokens(const solmsg_summary_t *p, const policy_claims_t *c)
{
    uint8_t i, j;
    uint64_t total_parsed = 0, total_claimed = 0;
    if (p->token_out_overflow) return POLICY_TOKEN;
    for (i = 0; i < c->n_token_out; i++) total_claimed = sat_add(total_claimed, c->token_out[i].amount);
    for (i = 0; i < p->n_token_out; i++) {
        const solmsg_token_out_t *t = &p->token_out[i];
        total_parsed = sat_add(total_parsed, t->amount);
        if (t->has_mint) {
            uint64_t per_mint = 0, claimed;
            for (j = 0; j < p->n_token_out; j++)
                if (p->token_out[j].has_mint && memcmp(p->token_out[j].mint, t->mint, 32) == 0)
                    per_mint = sat_add(per_mint, p->token_out[j].amount);
            if (!claimed_amount_for_mint(c, t->mint, &claimed)) return POLICY_TOKEN;
            if (per_mint > claimed) return POLICY_TOKEN;
        }
        /* every recipient token account must have been named (burns have no recipient) */
        if (t->has_dest && !claims_cover_dest(c, t)) return POLICY_TOKEN;
    }
    /* Plain Token.transfer carries no mint: the controller can only bound it by the claimed total.
     * (Minted + mint-less together may never exceed everything the daemon claimed.) */
    if (total_parsed > total_claimed) return POLICY_TOKEN;
    return POLICY_OK;
}

policy_verdict_t policy_check(const solmsg_summary_t *p, const policy_claims_t *c, const router_list_t *routers,
                              uint32_t *effective_dangerous)
{
    uint8_t i;
    policy_verdict_t v;
    uint32_t eff = p->dangerous;
    /* a program that is neither core nor an allowlisted router must be declared */
    for (i = 0; i < p->n_programs; i++)
        if (!solmsg_is_core_program(p->programs[i]) && !routers_is_allowed(routers, ROUTER_CHAIN_SOL, p->programs[i]))
            eff |= SOLMSG_DANGER_UNKNOWN_PROGRAM;
    if (effective_dangerous) *effective_dangerous = eff;
    /* 1. fee payer / first signer is the controller's key */
    if (!p->fee_payer_is_owner) return POLICY_FEE_PAYER;
    /* 2. what leaves the owner is at most what was claimed, and goes where the daemon said */
    if (p->lamports_out > c->lamports_out) return POLICY_LAMPORTS;
    if (p->lamports_out > 0) {
        if (!p->has_dest || !c->has_dest) return POLICY_DEST;
        if (memcmp(p->dest, c->dest, 32) != 0) return POLICY_DEST;
    }
    v = check_tokens(p, c);
    if (v != POLICY_OK) return v;
    /* 3. every program id in the message is claimed */
    if (p->programs_overflow) return POLICY_PROGRAM;
    for (i = 0; i < p->n_programs; i++)
        if (!claims_have_program(c, p->programs[i])) return POLICY_PROGRAM;
    /* 4. dangerous instructions (and unknown programs) must have been declared */
    if (eff & ~c->dangerous) return POLICY_DANGEROUS;
    return POLICY_OK;
}

/* ---- EVM ---- */

/* Sum of claimed amounts for `token` whose recipient is `to` (entries without a recipient cover nothing). */
static bool claimed_token_amount(const policy_evm_claims_t *c, const uint8_t token[20], const uint8_t to[20], u256_t *out)
{
    uint8_t i;
    bool found = false;
    u256_zero(out);
    for (i = 0; i < c->n_token_out; i++) {
        if (memcmp(c->token_out[i].token, token, 20) != 0) continue;
        if (!c->token_out[i].has_to || memcmp(c->token_out[i].to, to, 20) != 0) continue;
        u256_add_sat(out, out, &c->token_out[i].amount);
        found = true;
    }
    return found;
}

policy_verdict_t policy_check_evm(const evmtx_t *tx, const uint8_t owner[20], const policy_evm_claims_t *c,
                                  const router_list_t *routers, uint32_t *effective_dangerous)
{
    uint32_t eff = tx->dangerous;
    /* what the daemon will have to have declared */
    if (tx->call == EVMTX_CALL_CREATE) eff |= EVMTX_DANGER_UNKNOWN_CONTRACT;
    if (tx->call == EVMTX_CALL_UNKNOWN && !routers_is_allowed(routers, ROUTER_CHAIN_EVM, tx->to)) eff |= EVMTX_DANGER_UNKNOWN_CONTRACT;
    if (effective_dangerous) *effective_dangerous = eff;
    /* 1. chain id and destination */
    if (tx->chain_id != c->chain_id) return POLICY_CHAIN;
    if (c->has_to != tx->has_to) return POLICY_DEST;
    if (tx->has_to && memcmp(tx->to, c->to, 20) != 0) return POLICY_DEST;
    /* 2. native value */
    if (u256_cmp(&tx->value, &c->value_wei) > 0) return POLICY_VALUE;
    /* 3. worst-case fee: gas limit x max fee per gas */
    if (u256_cmp(&tx->max_fee_total, &c->max_fee_wei) > 0) return POLICY_FEE;
    /* 4. ERC-20 units leaving the owner: transfer, and transferFrom with the owner as source; the recipient
     *    must be the claimed one for that token */
    if (tx->call == EVMTX_CALL_TRANSFER || (tx->call == EVMTX_CALL_TRANSFER_FROM && memcmp(tx->call_from, owner, 20) == 0)) {
        u256_t claimed;
        if (!claimed_token_amount(c, tx->to, tx->call_to, &claimed)) return POLICY_TOKEN;
        if (u256_cmp(&tx->call_amount, &claimed) > 0) return POLICY_TOKEN;
    }
    /* 5. approvals, operator grants and calls into contracts that are neither ERC-20 nor an allowlisted router */
    if (eff & ~c->dangerous) return POLICY_DANGEROUS;
    return POLICY_OK;
}

/* ---- Bitcoin ---- */

policy_verdict_t policy_check_btc(const btcpsbt_t *p, const policy_btc_claims_t *c, uint32_t *effective_dangerous)
{
    uint32_t eff = p->dangerous;
    uint8_t i;
    /* 4. every non-change output must be the claimed destination */
    for (i = 0; i < p->n_out; i++) {
        const btcpsbt_output_t *o = &p->out[i];
        if (o->is_change) continue;
        if (!c->has_dest || o->spk_len != c->dest_spk_len || memcmp(o->spk, c->dest_spk, o->spk_len) != 0)
            eff |= BTCPSBT_DANGER_UNKNOWN_CHANGE;
    }
    if (effective_dangerous) *effective_dangerous = eff;
    /* 1. network: the controller only holds mainnet keys (coin type 0') */
    if (!c->mainnet) return POLICY_CHAIN;
    if (c->has_inputs && c->inputs != p->n_owner_inputs) return POLICY_INPUTS;
    /* a spend must name where it goes, and the first non-change output must be exactly that */
    if (p->sats_out > 0 && !c->has_dest) return POLICY_DEST;
    if (c->has_dest && p->has_dest && (p->dest_spk_len != c->dest_spk_len || memcmp(p->dest_spk, c->dest_spk, p->dest_spk_len) != 0))
        return POLICY_DEST;
    /* 2. what leaves (non-change outputs) */
    if (p->sats_out > c->sats_out) return POLICY_VALUE;
    /* 3. fee */
    if (p->fee > c->fee_sats) return POLICY_FEE;
    /* 4 + 5. extra outputs, high fee, non-ALL sighash must have been declared */
    if (eff & ~c->dangerous) return POLICY_DANGEROUS;
    return POLICY_OK;
}

const char *policy_verdict_reason(policy_verdict_t v)
{
    switch (v) {
    case POLICY_OK: return NULL;
    case POLICY_FEE_PAYER: return "policy";
    case POLICY_LAMPORTS: return "claims_mismatch";
    case POLICY_TOKEN: return "claims_mismatch";
    case POLICY_PROGRAM: return "claims_mismatch";
    case POLICY_DANGEROUS: return "policy";
    case POLICY_CHAIN: return "claims_mismatch";
    case POLICY_DEST: return "claims_mismatch";
    case POLICY_VALUE: return "claims_mismatch";
    case POLICY_FEE: return "claims_mismatch";
    case POLICY_INPUTS: return "claims_mismatch";
    }
    return "policy";
}
