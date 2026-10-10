/* Pincer1 - compare the controller's own parse against the daemon's claims. PURE LOGIC.
 * Solana: PROTOCOL.md section 1 "Controller-side checks" 1-4. EVM and Bitcoin: the chain checks (1)-(5) of
 * "Multi-chain signing". The daily limits (Solana check 5, EVM/Bitcoin check 6) live in consent.c because
 * it owns the spend accounting.
 *
 * Destinations are compared, not just amounts: a Solana transfer's recipient must equal `claims.dest`, every
 * token transfer's recipient token account must be covered by a `claims.token_out` entry, an ERC-20
 * transfer's recipient must be the claimed one, and a Bitcoin spend must name its destination. The router
 * allowlist comes from routers.h (controller-side), never from the host. */
#ifndef PINCER_POLICY_H
#define PINCER_POLICY_H
#include <stdbool.h>
#include <stdint.h>

#include "btcpsbt.h"
#include "evmtx.h"
#include "routers.h"
#include "solmsg.h"
#include "u256.h"

#define POLICY_MAX_PROGRAMS 16
#define POLICY_MAX_TOKEN_OUT 8

typedef struct {
    uint64_t lamports_out;
    struct {
        uint8_t mint[32];
        uint64_t amount;
        bool has_dest;
        uint8_t dest[32]; /* destination token account */
    } token_out[POLICY_MAX_TOKEN_OUT];
    uint8_t n_token_out;
    bool has_dest;
    uint8_t dest[32];
    uint8_t programs[POLICY_MAX_PROGRAMS][32];
    uint8_t n_programs;
    uint32_t dangerous; /* SOLMSG_DANGER_* bitmask */
} policy_claims_t;

/* claims for kind "evm_tx". `routers` is no longer a field: the controller keeps its own allowlist. */
typedef struct {
    uint64_t chain_id;
    bool has_to;
    uint8_t to[20];
    u256_t value_wei;
    struct {
        uint8_t token[20];
        u256_t amount;
        bool has_to;
        uint8_t to[20]; /* recipient of the ERC-20 transfer */
    } token_out[POLICY_MAX_TOKEN_OUT];
    uint8_t n_token_out;
    u256_t max_fee_wei;
    uint32_t dangerous; /* EVMTX_DANGER_* bitmask */
} policy_evm_claims_t;

/* claims for kind "btc_psbt" */
typedef struct {
    bool mainnet;                        /* "network":"mainnet" (the only network the controller has keys for) */
    uint64_t sats_out;
    bool has_dest;
    uint8_t dest_spk[BTCPSBT_MAX_SCRIPT]; /* claims.dest decoded to a scriptPubKey */
    uint8_t dest_spk_len;
    uint64_t fee_sats;
    bool has_inputs;
    uint8_t inputs;                      /* owner inputs the daemon expects */
    uint32_t dangerous;                  /* BTCPSBT_DANGER_* bitmask */
} policy_btc_claims_t;

typedef enum {
    POLICY_OK = 0,
    POLICY_FEE_PAYER,  /* Solana check 1 */
    POLICY_LAMPORTS,   /* Solana check 2: lamports */
    POLICY_TOKEN,      /* Solana check 2 / EVM check 4: token_out (amount or recipient) */
    POLICY_PROGRAM,    /* Solana check 3 */
    POLICY_DANGEROUS,  /* Solana check 4 / EVM check 5 / Bitcoin checks 4-5 */
    POLICY_CHAIN,      /* EVM check 1: chain id / Bitcoin check 1: network */
    POLICY_DEST,       /* Solana check 2: dest / EVM check 1: `to` / Bitcoin: dest */
    POLICY_VALUE,      /* EVM check 2: value_wei / Bitcoin check 2: sats_out */
    POLICY_FEE,        /* EVM check 3: gas * fee / Bitcoin check 3: fee */
    POLICY_INPUTS,     /* Bitcoin: owner input count */
} policy_verdict_t;

/* `routers` = the owner-added list (NULL = built-ins only). `effective_dangerous` receives parsed->dangerous
 * plus unknown_program for every program that is neither core nor an allowlisted router. */
policy_verdict_t policy_check(const solmsg_summary_t *parsed, const policy_claims_t *claims, const router_list_t *routers,
                              uint32_t *effective_dangerous);
/* `tx` must have been analysed (evmtx_analyze). `effective_dangerous` receives the flags the controller
 * will report in `pending.parsed` (tx->dangerous plus unknown_contract when `to` is not an allowlisted router). */
policy_verdict_t policy_check_evm(const evmtx_t *tx, const uint8_t owner[20], const policy_evm_claims_t *claims,
                                  const router_list_t *routers, uint32_t *effective_dangerous);
/* `p` must have been analysed (btcpsbt_analyze). `effective_dangerous` adds unknown_change for every
 * non-change output that is not the claimed destination. */
policy_verdict_t policy_check_btc(const btcpsbt_t *p, const policy_btc_claims_t *claims, uint32_t *effective_dangerous);
/* Protocol `denied.reason` string for a verdict (NULL for POLICY_OK). */
const char *policy_verdict_reason(policy_verdict_t v);
#endif
