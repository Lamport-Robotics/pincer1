/* Pincer1 - EVM transaction parser: RLP, legacy (EIP-155) and EIP-1559 (type 2) unsigned transactions,
 * ERC-20 calldata recognition, signing digests, EIP-191 message digests. PURE LOGIC, no hardware.
 *
 * The daemon sends the exact bytes that get hashed for signing:
 *   legacy  RLP([nonce, gasPrice, gasLimit, to, value, data, chainId, 0, 0])      digest = keccak(bytes)
 *   type 2  0x02 || RLP([chainId, nonce, maxPriorityFee, maxFee, gasLimit, to, value, data, accessList])
 * Pre-EIP-155 legacy transactions (6 fields, no chain id) are refused: no replay protection.
 * Type 1 (EIP-2930) and any other type byte are refused. */
#ifndef PINCER_EVMTX_H
#define PINCER_EVMTX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "u256.h"

#define EVMTX_MAX_BYTES 4096
#define EVMTX_SELECTOR_TRANSFER 0xa9059cbbu
#define EVMTX_SELECTOR_APPROVE 0x095ea7b3u
#define EVMTX_SELECTOR_TRANSFER_FROM 0x23b872ddu
#define EVMTX_SELECTOR_SET_APPROVAL_FOR_ALL 0xa22cb465u
#define EVMTX_SELECTOR_INCREASE_ALLOWANCE 0x39509351u

/* Dangerous flags, named exactly as `claims.dangerous[]` strings. `approve` and `approve_unlimited` are
 * exclusive (2^256-1 raises only the latter); `unknown_contract` is decided by policy.c because it depends
 * on `claims.routers`. */
#define EVMTX_DANGER_APPROVE (1u << 0)              /* "approve" (incl. increaseAllowance) */
#define EVMTX_DANGER_APPROVE_UNLIMITED (1u << 1)    /* "approve_unlimited" */
#define EVMTX_DANGER_SET_APPROVAL_FOR_ALL (1u << 2) /* "set_approval_for_all" */
#define EVMTX_DANGER_UNKNOWN_CONTRACT (1u << 3)     /* "unknown_contract" */
#define EVMTX_DANGER_ALL 0x0fu

typedef enum {
    EVMTX_CALL_NONE = 0,       /* no calldata: plain value transfer */
    EVMTX_CALL_TRANSFER,       /* ERC-20 transfer(to, amount) */
    EVMTX_CALL_APPROVE,        /* approve(spender, amount) / increaseAllowance */
    EVMTX_CALL_TRANSFER_FROM,  /* transferFrom(from, to, amount) */
    EVMTX_CALL_SET_APPROVAL,   /* setApprovalForAll(operator, approved) */
    EVMTX_CALL_UNKNOWN,        /* anything else (router or unknown contract: policy decides) */
    EVMTX_CALL_CREATE,         /* no `to`: contract creation */
} evmtx_call_t;

typedef struct {
    uint8_t type;            /* 0 legacy, 2 EIP-1559 */
    uint64_t chain_id;
    uint64_t nonce;
    uint64_t gas_limit;
    u256_t max_fee_per_gas;  /* gasPrice for legacy */
    u256_t max_priority_fee; /* type 2 only */
    bool has_to;
    uint8_t to[20];
    u256_t value;
    const uint8_t *data;     /* points into the input bytes */
    size_t data_len;
    size_t access_list_len;  /* raw RLP payload length of the access list (type 2); contents ignored */
    /* analysis */
    evmtx_call_t call;
    uint8_t call_to[20];     /* transfer / transferFrom recipient, approve spender, setApprovalForAll operator */
    uint8_t call_from[20];   /* transferFrom source */
    u256_t call_amount;      /* ERC-20 amount / allowance */
    bool call_flag;          /* setApprovalForAll approved */
    uint32_t dangerous;      /* EVMTX_DANGER_* without unknown_contract */
    u256_t max_fee_total;    /* gas_limit * max_fee_per_gas, saturated */
} evmtx_t;

enum {
    EVMTX_OK = 0,
    EVMTX_E_RLP = -1,       /* malformed / non-canonical RLP */
    EVMTX_E_TYPE = -2,      /* unsupported transaction type */
    EVMTX_E_FIELDS = -3,    /* wrong field count or field shape */
    EVMTX_E_RANGE = -4,     /* nonce / gas / chain id outside u64, value outside u256 */
    EVMTX_E_SIZE = -5,
};

int evmtx_parse(const uint8_t *bytes, size_t len, evmtx_t *out);
/* Fills call / call_* / dangerous / max_fee_total from the parsed fields. `owner` is the controller's
 * EVM address (transferFrom(from == owner) counts as an outflow). */
void evmtx_analyze(evmtx_t *tx, const uint8_t owner[20]);
/* keccak of the bytes as given (legacy: EIP-155 payload; typed: 0x02 || RLP). */
void evmtx_signing_digest(const uint8_t *bytes, size_t len, uint8_t digest[32]);
/* r||s||v: v = 27 + recid for legacy (the daemon applies EIP-155: chainId*2 + 35 + recid), v = recid for typed. */
void evmtx_encode_signature(const evmtx_t *tx, const uint8_t sig[64], uint8_t recid, uint8_t out[65]);
/* EIP-191 personal_sign: keccak("\x19Ethereum Signed Message:\n" || decimal(len) || msg). */
void evmtx_personal_digest(const uint8_t *msg, size_t len, uint8_t digest[32]);
/* True when the bytes parse as a transaction (used to refuse disguised signMessage payloads). */
bool evmtx_looks_like_tx(const uint8_t *bytes, size_t len);

const char *evmtx_danger_name(uint32_t bit);
uint32_t evmtx_danger_from_name(const char *name);
#endif
