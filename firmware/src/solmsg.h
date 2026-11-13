/* Pincer1 - Solana serialized *message* parser (legacy, v0 and version 1). PURE LOGIC, no hardware includes.
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
#define SOLMSG_VERSION_1 1 /* SIMD-0385: first byte 0x81, no lookup tables, fee and limits in the header */
/* A version 1 transaction is at most 4096 bytes and carries at least one 64-byte signature after the message. */
#define SOLMSG_V1_MAX_BYTES (4096 - 64)

typedef struct {
    uint8_t program_index;
    uint8_t n_accounts;
    const uint8_t *account_indexes;
    uint16_t data_len;
    const uint8_t *data;
    bool touches_lookup; /* program id or any account comes from a lookup table */
} solmsg_ix_t;

typedef struct {
    uint8_t version; /* SOLMSG_VERSION_LEGACY, 0 or SOLMSG_VERSION_1 */
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
    /* version 1 only: the flat priority fee from the header (0 when its config bits are clear). ComputeBudget
     * instructions set nothing in a version 1 transaction. */
    uint64_t v1_priority_lamports;
    uint32_t v1_compute_unit_limit;
} solmsg_t;

/* Dangerous-instruction flags, named exactly as `claims.dangerous[]` strings. */
#define SOLMSG_DANGER_SET_AUTHORITY (1u << 0) /* "set_authority" */
#define SOLMSG_DANGER_APPROVE (1u << 1)       /* "approve" */
#define SOLMSG_DANGER_CLOSE_ACCOUNT (1u << 2) /* "close_account" */
#define SOLMSG_DANGER_ASSIGN (1u << 3)        /* "assign" */
#define SOLMSG_DANGER_LOOKUP_TABLE (1u << 4)  /* "lookup_table" */
#define SOLMSG_DANGER_UNKNOWN_PROGRAM (1u << 5) /* "unknown_program": a program that is neither a core program nor an
                                                   allowlisted router (decided by policy.c, needs the router list) */
#define SOLMSG_DANGER_UNKNOWN_IX (1u << 6) /* "unknown_instruction": a System instruction this parser cannot classify */
#define SOLMSG_DANGER_ROUTE (1u << 7) /* "route_dest": a Jupiter route whose output does not land in the owner's own
                                       * associated token account, whose authority is not the owner, or whose shape
                                       * this parser does not know */
#define SOLMSG_DANGER_ALL 0xffu

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
    bool multi_dest;                  /* lamports leave the owner towards more than one recipient: the display
                                         can only show one, so policy refuses the message */
    bool fee_payer_is_owner;
    /* What the fee payer (the owner) pays the leader: 5000 lamports per required signature plus the
     * prioritisation fee set through ComputeBudget (unit limit x unit price, rounded up). A compute budget
     * is the one way a message spends SOL without a transfer, so it is parsed, shown and claimed. */
    uint64_t fee_lamports;      /* base + priority, saturating */
    uint64_t priority_lamports; /* the ComputeBudget part alone: counted against limits and the allowance */
    /* Programs that are neither core nor a Jupiter route this parser verified end to end. Anything here
     * means the message can move assets the display cannot show, so the allowance never covers it. */
    uint8_t n_unverified_programs;
    bool has_swap;              /* a verified Jupiter route: output goes to the owner's ATA of swap_mint */
    uint8_t swap_mint[32];
} solmsg_summary_t;

/* Associated token account of `owner` for `mint` under `token_program` (sha256 + off-curve search, like
 * find_program_address). False only if no bump works (cannot happen for real keys). */
bool solmsg_ata_derive(const uint8_t owner[32], const uint8_t token_program[32], const uint8_t mint[32], uint8_t out[32]);

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
/* True when the bytes are a well-formed legacy, v0 or version 1 message (used to refuse disguised signMessage payloads). */
bool solmsg_looks_like_message(const uint8_t *bytes, size_t len);

/* True when the bytes are, or contain, a Privacy Rings key-derivation message: the text "TSPP/derive/" of any
 * version, raw or inside any off-chain envelope, or the application domain the canonical envelope carries
 * (SHA-256 of "TSPP/derive/v1"). The signature over that message is deterministic and every privacy
 * key of the wallet is derived from it, so signMessage must never produce it: one touch on an innocent-looking
 * request would hand over the viewing and nullifier keys for good. */
bool solmsg_is_ring_key_derivation(const uint8_t *bytes, size_t len);

/* ---- what signMessage may sign ----
 * A message signature is a raw Ed25519 signature by the wallet key over the given bytes, so anything the
 * owner cannot read in full on the display is something they cannot consent to. The controller signs only:
 *   - bare text: every byte 0x20..0x7e or '\n', not exactly 32 bytes, and with a space once it is 32 bytes
 *     or longer (a digest, a hex, base58 or base64 string and compact JSON all fail this);
 *   - a Solana off-chain message, header version 0 (application domain, format 0 or 1, length prefix) or
 *     version 1, with exactly one signer that is the active key and a body that is such text.
 * It refuses every payload whose first byte is 0x80 or above and is not an off-chain message (every versioned
 * transaction message, including any version the parser above does not read yet), anything that names the
 * Privacy Rings key namespaces, and anything that carries one of the wallet's own public keys outside the
 * envelope's signer field. solmsg_looks_like_message() stays a separate, earlier check. */
typedef enum {
    SOLMSG_SIGNMSG_TEXT = 0,    /* bare text */
    SOLMSG_SIGNMSG_OFFCHAIN_V0, /* "\xffsolana offchain", header version 0 */
    SOLMSG_SIGNMSG_OFFCHAIN_V1, /* "\xffsolana offchain", header version 1 */
} solmsg_signmsg_shape_t;

typedef struct {
    solmsg_signmsg_shape_t shape;
    const uint8_t *text;       /* the text to show, inside `bytes` */
    size_t text_len;
    const uint8_t *app_domain; /* version 0 only: the 32-byte application domain when it is not all zero */
} solmsg_signmsg_t;

/* `own_key` is the active slot's key; `other_key` (may be NULL) is the other slot's. Returns false to refuse. */
bool solmsg_signmsg_check(const uint8_t *bytes, size_t len, const uint8_t own_key[32], const uint8_t *other_key,
                          solmsg_signmsg_t *out);
/* The bare-text rule alone, for the message kinds of other chains (EIP-191 personal_sign, BIP-322). */
bool solmsg_bare_text_ok(const uint8_t *bytes, size_t len);
/* True when the message names the Privacy Rings pool or registry program among its static account keys.
 * Until the controller parses those instructions itself, such a transaction is refused in every framing:
 * shown as an unknown program it could deposit public funds into another wallet's private balance, write a
 * permanent registry record, or spend the private balance. */
bool solmsg_touches_privacy_rings(const solmsg_t *m);
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
