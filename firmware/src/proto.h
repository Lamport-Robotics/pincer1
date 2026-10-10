/* Pincer1 - newline-delimited JSON framing and message (de)serialisation for PROTOCOL.md section 1. */
#ifndef PINCER_PROTO_H
#define PINCER_PROTO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btcpsbt.h"
#include "consent.h"
#include "evmtx.h"
#include "policy.h"
#include "solmsg.h"
#include "u256.h"

#define PROTO_LINE_MAX 8192
#define PROTO_ID_MAX CONSENT_ID_MAX
#define PROTO_MAX_BYTES 4096 /* >= SOLMSG_MAX_BYTES, EVMTX_MAX_BYTES, BTCPSBT_MAX_BYTES; each parser re-checks its own bound */
#define PROTO_FW_VERSION "0.4.0"

typedef enum {
    PROTO_KIND_TX = 0,   /* Solana message */
    PROTO_KIND_MSG,      /* Solana signMessage */
    PROTO_KIND_EVM_TX,   /* unsigned RLP, legacy or type 2 */
    PROTO_KIND_BTC_PSBT, /* BIP-174 PSBT */
    PROTO_KIND_EVM_MSG,  /* EIP-191 bytes, or EIP-712 typed_data + digest */
    PROTO_KIND_BTC_MSG,  /* BIP-322 simple */
} proto_sign_kind_t;

/* ---- framing ---- */
typedef struct {
    char buf[PROTO_LINE_MAX];
    size_t len;
    bool overflow;
} proto_framer_t;

void proto_framer_init(proto_framer_t *f);
/* Push one byte. Returns true when a complete line is available in `*line` (NUL-terminated,
 * without the newline). Over-long lines are discarded up to the next newline. */
bool proto_framer_push(proto_framer_t *f, char c, const char **line);

/* ---- requests ---- */
typedef enum {
    PROTO_REQ_UNKNOWN = 0,
    PROTO_REQ_HELLO,
    PROTO_REQ_TIME,
    PROTO_REQ_INIT,
    PROTO_REQ_SIGN,
    PROTO_REQ_ELEVATE,
    PROTO_REQ_LOCK,
    PROTO_REQ_UNLOCK,
    PROTO_REQ_POLICY,
    PROTO_REQ_DURESS,
    PROTO_REQ_ALLOWANCE,
    PROTO_REQ_SET_PIN,
    PROTO_REQ_INHERITANCE_SET,     /* uses .sign (bytes + claims) and .inheritance */
    PROTO_REQ_INHERITANCE_STATUS,
    PROTO_REQ_INHERITANCE_RELEASE,
    PROTO_REQ_INHERITANCE_CLEAR,
    PROTO_REQ_REVEAL_SEED,         /* always refused (not_supported): the words never cross the link */
    PROTO_REQ_SET_DURESS_PIN,      /* second rhythm -> decoy slot */
    PROTO_REQ_DECOY_ADDRESSES,     /* PIN-gated read of the decoy addresses */
} proto_req_type_t;

typedef struct {
    proto_req_type_t type;
    char id[PROTO_ID_MAX];
    uint64_t unix_time; /* time */
    struct {
        proto_sign_kind_t kind;
        bool is_msg; /* kind == "msg" (Solana) */
        uint8_t bytes[PROTO_MAX_BYTES];
        uint16_t len;              /* 0 only for an EIP-712 request (typed.present) */
        policy_claims_t claims;    /* kind tx */
        policy_evm_claims_t evm_claims; /* kind evm_tx */
        policy_btc_claims_t btc_claims; /* kind btc_psbt */
        struct {
            bool present;          /* evm_msg carried typed_data + digest */
            int rc;                /* eip712_digest() result over the JSON */
            uint8_t digest[32];    /* what the controller computed */
            uint8_t claimed[32];   /* what the daemon says it is */
        } typed;
        uint32_t ttl_ms;
    } sign;
    uint64_t limit_lamports; /* elevate.limit_lamports|limit_sats / policy.daily_limit_lamports (compat) */
    consent_chain_t elevate_chain; /* elevate: which limit (limit_lamports | limit_wei | limit_sats) */
    u256_t limit_wei;        /* elevate.limit_wei */
    consent_policy_req_t policy; /* policy: any subset of the three limits */
    struct {
        bool revoke;
        consent_allowance_t a;
    } allowance;
    consent_inheritance_t inheritance; /* inheritance_set: inactivity_days, warn_days, heir */
} proto_req_t;

enum {
    PROTO_OK = 0,
    PROTO_E_JSON = -1,
    PROTO_E_TYPE = -2,
    PROTO_E_FIELD = -3,
    PROTO_E_ENCODING = -4,
    PROTO_E_TOOBIG = -5,
};

int proto_parse_request(const char *line, proto_req_t *req);
const char *proto_error_str(int err);

/* ---- responses (each writes one JSON object + '\n', NUL-terminated; returns length or -1) ---- */
typedef struct {
    const char *fw;
    bool se050;
    bool initialized;
    const uint8_t *pubkey; /* NULL when not initialised */
    bool locked;
    uint64_t daily_limit;
    uint64_t spent_today;
    uint32_t day;
    const consent_allowance_t *allowance; /* NULL -> "allowance":null */
    bool pin_set;
    bool duress_pin_set; /* a decoy rhythm exists (not whether duress is active: that is never reported) */
    uint8_t pin_failures;
    bool dev_keys; /* SCP03 keys compiled in (bench) rather than read from OTP */
    bool inh_armed;
    uint16_t inh_inactivity_days;
    uint32_t inh_days_since_touch_tenths; /* 124 -> "days_since_touch":12.4 */
    /* multi-chain (appended so positional initialisers of the Solana-only fields keep working) */
    const uint8_t *evm_addr;  /* 20 bytes or NULL */
    const uint8_t *btc_pkh;   /* 20 bytes or NULL */
    const u256_t *daily_limit_wei; /* NULL prints "0" */
    const u256_t *spent_today_wei;
    uint64_t daily_limit_sats;
    uint64_t spent_today_sats;
    bool display;                  /* a trusted display is driven by this firmware (false: PINCER_NO_DISPLAY build) */
    const router_list_t *routers;  /* owner-added routers (NULL = none); built-ins are always listed */
} proto_hello_t;

typedef struct {
    const consent_inheritance_t *inh;     /* heir / warned / thresholds; armed=false prints heir:null */
    uint32_t days_since_touch_tenths;
    uint32_t powered_days_tenths;
} proto_inh_status_t;

int proto_fmt_hello(char *out, size_t cap, const proto_hello_t *h);
int proto_fmt_pending(char *out, size_t cap, const char *id, const char *need, const solmsg_summary_t *parsed);
/* Same, with the effective dangerous flags as policy.c computed them (adds unknown_program). */
int proto_fmt_pending_sol(char *out, size_t cap, const char *id, const char *need, const solmsg_summary_t *parsed, uint32_t dangerous);
/* {"t":"pending","id":id,"need":"pin","confirm":true}: set_pin asks for the rhythm a second time. */
int proto_fmt_pending_confirm(char *out, size_t cap, const char *id);
int proto_fmt_signed(char *out, size_t cap, const char *id, const uint8_t sig[64]);
/* {"t":"signed","id":id,"sig":"<base64 of len bytes>"} (65-byte r||s||v, BIP-322 witness) */
int proto_fmt_signed_bytes(char *out, size_t cap, const char *id, const uint8_t *sig, size_t len);
/* {"t":"signed","id":id,"psbt":"<base64>"}: the PSBT with partial signatures */
int proto_fmt_signed_psbt(char *out, size_t cap, const char *id, const uint8_t *psbt, size_t len);
/* pending with the EVM parse (effective dangerous flags as policy.c computed them) */
int proto_fmt_pending_evm(char *out, size_t cap, const char *id, const char *need, const evmtx_t *tx, uint32_t dangerous);
int proto_fmt_pending_btc(char *out, size_t cap, const char *id, const char *need, const btcpsbt_t *p, uint32_t dangerous);
int proto_fmt_denied(char *out, size_t cap, const char *id, const char *reason);
/* One slot's three addresses, as printed under "addresses" / "decoy_addresses". */
typedef struct {
    const uint8_t *pubkey;   /* 32, required */
    const uint8_t *evm_addr; /* 20 or NULL */
    const uint8_t *btc_pkh;  /* 20 or NULL */
} proto_addrs_t;
/* {"t":"init","id":id,"pubkey":"<b58>","addresses":{…},"decoy_addresses":{…}}: NO mnemonic, ever. The words are
 * shown on the trusted display only. The decoy addresses appear here exactly once (never in hello) so the
 * owner can fund the decoy wallet; `decoy` may be NULL (printed as null). */
int proto_fmt_init(char *out, size_t cap, const char *id, const proto_addrs_t *real, const proto_addrs_t *decoy);
/* {"t":"decoy_addresses","id":id,"addresses":{…}}: the PIN-gated read of the decoy slot. */
int proto_fmt_decoy_addresses(char *out, size_t cap, const char *id, const proto_addrs_t *decoy);
int proto_fmt_elevated(char *out, size_t cap, const char *id, uint32_t until_ms);
int proto_fmt_ok(char *out, size_t cap, const char *t, const char *id); /* {"t":t,"id":id,"ok":true} */
int proto_fmt_simple(char *out, size_t cap, const char *t, const char *id); /* {"t":t[,"id":id]} */
int proto_fmt_event(char *out, size_t cap, const char *ev, const char *what);
/* {"t":"event","ev":"inheritance_warning","days_left":n} */
int proto_fmt_event_days_left(char *out, size_t cap, uint32_t days_left);
/* {"t":"event","ev":"display","page":n,"pages":m} (1-based): what the trusted display shows right now. */
int proto_fmt_event_display(char *out, size_t cap, uint8_t page, uint8_t pages);
int proto_fmt_error(char *out, size_t cap, const char *id, const char *error);
/* {"t":"inheritance_set","id":id,"ok":true,"sig":"<base64>"}: the transaction bytes are never echoed. */
int proto_fmt_inheritance_set(char *out, size_t cap, const char *id, const uint8_t sig[64]);
int proto_fmt_inheritance_status(char *out, size_t cap, const char *id, const proto_inh_status_t *st);
/* {"t":"inheritance_release","id":id,"tx":"<base64 wire transaction>"} */
int proto_fmt_inheritance_release(char *out, size_t cap, const char *id, const uint8_t *tx, size_t len);
#endif
