/* Pincer1 - Solana serialized *message* parser (legacy + v0). PURE LOGIC, no hardware includes.
 *
 * The controller cannot resolve address-lookup tables, so for v0 messages every instruction that
 * references a looked-up account (index >= number of static keys) is treated as `unknown` and is
 * reported under the dangerous flag SOLMSG_DANGER_LOOKUP_TABLE ("lookup_table"). */
#ifndef PINCER_SOLMSG_H
#define PINCER_SOLMSG_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLMSG_MAX_BYTES 1232 /* Solana packet payload limit */
#define SOLMSG_MAX_IX 48
#define SOLMSG_MAX_LOOKUPS 8
#define SOLMSG_MAX_PROGRAMS 16
#define SOLMSG_MAX_TOKEN_OUT 8

#define SOLMSG_VERSION_LEGACY 0xff

typedef struct {
    uint8_t program_index;
    uint8_t n_accounts;
    const uint8_t *account_indexes;
    uint16_t data_len;
    const uint8_t *data;
    bool touches_lookup; /* program id or any account comes from a lookup table */
} solmsg_ix_t;

typedef struct {
    uint8_t version; /* SOLMSG_VERSION_LEGACY or 0 */
    uint8_t num_required_signatures;
    uint8_t num_readonly_signed;
    uint8_t num_readonly_unsigned;
    uint8_t n_static_keys;
    const uint8_t *static_keys; /* key i at static_keys + 32*i */
    const uint8_t *recent_blockhash;
    uint8_t n_ix;
    solmsg_ix_t ix[SOLMSG_MAX_IX];
    uint8_t n_lookup_tables;
    uint16_t n_lookup_accounts; /* total writable + readonly loaded addresses */
} solmsg_t;

/* Dangerous-instruction flags, named exactly as `claims.dangerous[]` strings. */
#define SOLMSG_DANGER_SET_AUTHORITY (1u << 0) /* "set_authority" */
#define SOLMSG_DANGER_APPROVE (1u << 1)       /* "approve" */
#define SOLMSG_DANGER_CLOSE_ACCOUNT (1u << 2) /* "close_account" */
#define SOLMSG_DANGER_ASSIGN (1u << 3)        /* "assign" */
#define SOLMSG_DANGER_LOOKUP_TABLE (1u << 4)  /* "lookup_table" */
#define SOLMSG_DANGER_UNKNOWN_PROGRAM (1u << 5) /* "unknown_program": a program that is neither a core program nor an
                                                   allowlisted router (decided by policy.c, needs the router list) */
#define SOLMSG_DANGER_ALL 0x3fu

typedef struct {
    uint8_t source[32];
    uint8_t mint[32];
    bool has_mint; /* false for plain Token `transfer` (3): the mint is not in the instruction */
    uint8_t dest[32];
    bool has_dest; /* destination token account (transfer / transferChecked); false for burn */
    uint64_t amount;
} solmsg_token_out_t;

typedef struct {
    uint64_t lamports_out;            /* saturating sum of lamports leaving the owner */
    solmsg_token_out_t token_out[SOLMSG_MAX_TOKEN_OUT];
    uint8_t n_token_out;
    bool token_out_overflow;          /* more token transfers than we can list -> treat as mismatch */
    uint32_t dangerous;               /* SOLMSG_DANGER_* bitmask */
    uint8_t programs[SOLMSG_MAX_PROGRAMS][32];
    uint8_t n_programs;               /* deduplicated static program ids */
    bool programs_overflow;
    bool has_dest;
    uint8_t dest[32];                 /* destination of the first lamport / token transfer */
    bool fee_payer_is_owner;
} solmsg_summary_t;

enum {
    SOLMSG_OK = 0,
    SOLMSG_E_SHORT = -1,
    SOLMSG_E_VERSION = -2,
    SOLMSG_E_HEADER = -3,
    SOLMSG_E_INDEX = -4,
    SOLMSG_E_TOOMANY = -5,
    SOLMSG_E_TRAILING = -6,
};

/* Parses `len` bytes; `out` keeps pointers into `bytes` (must stay alive). */
int solmsg_parse(const uint8_t *bytes, size_t len, solmsg_t *out);
/* Computes what leaves `owner` and which programs are touched. */
void solmsg_analyze(const solmsg_t *m, const uint8_t owner[32], solmsg_summary_t *s);
/* True when the bytes are a well-formed legacy/v0 message (used to refuse disguised signMessage payloads). */
bool solmsg_looks_like_message(const uint8_t *bytes, size_t len);
/* Durable-nonce transaction: the FIRST instruction is System `AdvanceNonceAccount` (tag 4) on static
 * accounts. Fills `nonce_account` (its first account) when non-NULL. */
bool solmsg_starts_with_advance_nonce(const solmsg_t *m, uint8_t nonce_account[32]);

const char *solmsg_danger_name(uint32_t bit);  /* single bit -> name or NULL */
uint32_t solmsg_danger_from_name(const char *name); /* 0 if unknown */

/* True for the core programs every wallet transaction may touch without being "unknown": System, Token,
 * Token-2022, Associated Token, Compute Budget, Memo. */
bool solmsg_is_core_program(const uint8_t pid[32]);

extern const uint8_t SOLMSG_SYSTEM_PROGRAM[32];
extern const uint8_t SOLMSG_TOKEN_PROGRAM[32];
extern const uint8_t SOLMSG_TOKEN2022_PROGRAM[32];
#endif
