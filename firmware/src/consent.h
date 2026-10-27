/* Pincer1 - consent state machine. PURE LOGIC, no hardware includes.
 *
 *   idle --(request)--> pending(need touch | long_touch | pin) --(touch / rhythm)--> action
 *                                                              --(ttl)----------> denied:timeout
 *
 * Time: the controller has no clock. `now_ms` is a monotonic millisecond tick supplied by the caller;
 * wall-clock time arrives from the host via consent_set_time() and is only used for the daily
 * rollover and allowance expiry. TTLs are always measured in ticks.
 *
 * PIN: a tap rhythm (touch.h) whose 8-byte code is verified INSIDE the SE050 (UserID auth object).
 * This module only sequences it: it hands the code to the caller (consent_rhythm), the caller asks
 * the signer, and reports back (consent_pin_result). The failure counter here mirrors the chip's;
 * CONSENT_PIN_MAX_FAILURES wrong rhythms make consent_pin_result() demand a wipe.
 *
 * Trusted display: a sign request (and inheritance_set, and the seed-words flow after init) is
 * "display gated". It starts in CONSENT_NEED_DISPLAY and only becomes pending(touch) once the caller
 * reports that the trusted screen built from the controller's OWN parse was rendered
 * (consent_display_shown). Short touches turn pages until the final page has been shown; only then does
 * a touch count as consent. A long touch on a display-gated touch request cancels it (denied:user).
 *
 * Host time is untrusted: a day rollover of the spend counters needs BOTH a host day change AND
 * CONSENT_TIME_MIN_POWERED_S of the controller's own powered uptime since the last rollover; host time that
 * moves backwards or jumps more than CONSENT_TIME_MAX_JUMP_S forward is rejected. */
#ifndef PINCER_CONSENT_H
#define PINCER_CONSENT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "routers.h"
#include "u256.h"

#define CONSENT_ID_MAX 32
#define CONSENT_DEFAULT_TTL_MS 60000u
#define CONSENT_MAX_TTL_MS 300000u
#define CONSENT_ELEVATE_WINDOW_MS 120000u
#define CONSENT_ALLOWANCE_MAX_PROGRAMS 8
#define CONSENT_SLOT_REAL 0
#define CONSENT_SLOT_DECOY 1
#define CONSENT_PIN_CODE_LEN 8
#define CONSENT_PIN_MAX_FAILURES 5
#define CONSENT_INH_MAX_WARN 4
#define CONSENT_INH_MSG_MAX 860u          /* one SE05x APDU (SE05X_MAX_BUF_SIZE_CMD 892 - SCP03 overhead) */
#define CONSENT_INH_MAX_SIGNERS 8         /* num_required_signatures cap for the stored transaction */
#define CONSENT_POWERED_CHECKPOINT_S 3600u /* the uptime counter is persisted at least this often */
#define CONSENT_SECONDS_PER_DAY 86400ull
#define CONSENT_TIME_MIN_POWERED_S (20ull * 3600ull) /* powered uptime a day rollover needs since the last one */
#define CONSENT_TIME_MAX_JUMP_S (36ull * 3600ull)    /* a host time further ahead than this is rejected */
#define CONSENT_TIME_PERSIST_S 3600ull               /* the accepted time is written to flash this often at most */
#define CONSENT_INIT_WORDS_TTL_MS 600000u            /* the owner gets 10 minutes to copy the words */
#define CONSENT_MAX_PAGES 32

/* CONSENT_NEED_DISPLAY: the request is waiting for the trusted screen to be rendered; a touch does nothing. */
typedef enum { CONSENT_NEED_NONE = 0, CONSENT_NEED_TOUCH, CONSENT_NEED_LONG_TOUCH, CONSENT_NEED_PIN, CONSENT_NEED_DISPLAY } consent_need_t;

/* Chains with their own daily limit, spend counter and elevate. Amounts: Solana lamports (u64), Bitcoin sats
 * (u64), EVM wei (u256). */
typedef enum { CONSENT_CHAIN_SOL = 0, CONSENT_CHAIN_EVM = 1, CONSENT_CHAIN_BTC = 2, CONSENT_CHAIN_COUNT = 3 } consent_chain_t;

/* A `policy` request: any subset of the three limits, and/or one router allowlist change. */
typedef struct {
    bool has_sol, has_wei, has_sats;
    uint64_t sol;
    u256_t wei;
    uint64_t sats;
    bool has_router_add, has_router_remove;
    router_entry_t router;
} consent_policy_req_t;

typedef enum {
    CONSENT_OP_NONE = 0,
    CONSENT_OP_INIT,
    CONSENT_OP_SIGN,
    CONSENT_OP_ELEVATE,
    CONSENT_OP_UNLOCK,
    CONSENT_OP_POLICY,
    CONSENT_OP_ALLOWANCE,
    CONSENT_OP_SET_PIN,
    CONSENT_OP_INHERITANCE_SET,
    CONSENT_OP_INHERITANCE_CLEAR,
    CONSENT_OP_INIT_WORDS,        /* after INIT: the owner taps through the seed words on the display */
    CONSENT_OP_SET_DURESS_PIN,    /* a second rhythm that unlocks into the decoy slot */
    CONSENT_OP_DECOY_ADDRESSES,   /* PIN-gated read of the decoy slot's addresses */
    CONSENT_OP_UPDATE_APPLY,      /* reboot into a staged, verified firmware image (long touch) */
} consent_op_t;

typedef enum {
    CONSENT_DENY_NONE = 0,
    CONSENT_DENY_TIMEOUT,          /* "timeout" */
    CONSENT_DENY_CLAIMS_MISMATCH,  /* "claims_mismatch" */
    CONSENT_DENY_OVER_DAILY_LIMIT, /* "over_daily_limit" */
    CONSENT_DENY_LOCKED,           /* "locked" */
    CONSENT_DENY_POLICY,           /* "policy" */
    CONSENT_DENY_USER,             /* "user" */
    CONSENT_DENY_BUSY,             /* "busy" (extension: another request is pending) */
    CONSENT_DENY_NOT_INITIALIZED,  /* "not_initialized" (extension) */
    CONSENT_DENY_PIN_NOT_SET,      /* "pin_not_set": init done, set_pin not yet */
    CONSENT_DENY_WRONG_PIN,        /* "wrong_pin": the SE050 rejected the rhythm */
    CONSENT_DENY_PIN_MISMATCH,     /* "pin_mismatch": set_pin confirmation differed */
    CONSENT_DENY_WIPED,            /* "wiped": fifth failure, device reset */
    CONSENT_DENY_NOT_ARMED,        /* "not_armed": no inheritance stored */
    CONSENT_DENY_NOT_EXPIRED,      /* "not_expired": inheritance timer still running */
    CONSENT_DENY_NOT_SUPPORTED,    /* "not_supported": deliberate non-feature (reveal_seed) */
} consent_reason_t;

typedef enum { CONSENT_R_PENDING = 0, CONSENT_R_DENIED, CONSENT_R_DONE } consent_result_t;

typedef struct {
    bool active;
    uint64_t lamports;     /* total budget */
    uint64_t spent;        /* cumulative spend against the budget */
    uint64_t max_per_tx;
    uint64_t until_unix;
    uint8_t n_programs;
    uint8_t programs[CONSENT_ALLOWANCE_MAX_PROGRAMS][32];
} consent_allowance_t;

/* The persisted part (storage.c serialises exactly these fields). */
typedef struct {
    bool initialized;
    bool duress;
    uint64_t daily_limit;   /* lamports */
    uint64_t spent_today;
    uint32_t day; /* unix_days of spent_today (all chains); 0 = unknown */
    consent_allowance_t allowance;
    u256_t daily_limit_wei;
    u256_t spent_today_wei;
    uint64_t daily_limit_sats;
    uint64_t spent_today_sats;
    bool pin_set;         /* the UserID object exists in the chip */
    uint8_t pin_failures; /* mirror of the chip's attempt counter */
    bool duress_pin_set;  /* a second UserID object (decoy rhythm) exists */
    uint64_t rollover_powered_seconds; /* powered_seconds at the last accepted day rollover */
    router_list_t routers;             /* owner-added swap routers (built-ins live in routers.c) */
    /* The inactive slot's limits and counters. Each slot (real, decoy) has its own; entering or leaving
     * duress swaps the active set with this one, so every policy request works the same way in both slots
     * and nothing in a reply says which slot is live. */
    uint64_t last_unix; /* newest host time ever accepted (hourly granularity): a reboot cannot turn the clock back */
    bool shadow_valid; /* the decoy slot has limits of its own; until then it inherits the real ones on first use */
    struct {
        uint64_t daily_limit;
        uint64_t spent_today;
        u256_t daily_limit_wei;
        u256_t spent_today_wei;
        uint64_t daily_limit_sats;
        uint64_t spent_today_sats;
    } shadow;
} consent_persist_t;

/* Inheritance arrangement (dead-man's switch). The signed transaction itself is kept by the caller
 * (storage.c inheritance record); this is the policy part. */
typedef struct {
    bool armed;
    uint16_t inactivity_days;                 /* release allowed once days_since_touch >= this */
    uint8_t n_warn;
    uint16_t warn_days[CONSENT_INH_MAX_WARN]; /* "days_left" thresholds, each < inactivity_days */
    uint8_t warned;                           /* bit i set = warning i emitted since the last touch */
    uint8_t heir[32];
    uint8_t signer[32];                       /* real public key that signed it; clear / replace need it */
} consent_inheritance_t;

/* The dead-man timer. Persisted in its OWN flash region (storage.c), never touched by lock, duress or the
 * PIN wipe: a thief must not be able to cancel it. Time here is powered uptime in seconds, counted from
 * the tick; host time plays no part. The counter resumes from the stored value and never goes backwards. */
typedef struct {
    uint64_t powered_seconds;
    uint64_t last_touch_powered_seconds; /* any debounced touch (short, long, rhythm tap) */
    consent_inheritance_t inh;
} consent_inh_persist_t;

typedef struct {
    consent_op_t op;
    char id[CONSENT_ID_MAX];
    uint8_t slot;        /* for SIGN / INHERITANCE_SET: key slot to use */
    consent_chain_t chain; /* for SIGN: which daily limit the spend counts against */
    uint64_t lamports;   /* for SIGN: lamports_out (SOL) or sats_out (BTC) to account once signed */
    u256_t wei;          /* for SIGN on EVM: value_wei to account once signed */
    bool via_allowance;  /* for SIGN: counted against the allowance budget */
    uint8_t pin[CONSENT_PIN_CODE_LEN]; /* for SET_PIN / pin-gated ops: the rhythm code */
    consent_inheritance_t inh;         /* for INHERITANCE_SET: what to arm once the signature exists */
    uint8_t page;                      /* for CONSENT_TOUCH_PAGE: the page to render next */
    consent_policy_req_t policy;       /* for POLICY: what was applied (router add/remove for the display) */
    char update_version[16];           /* for UPDATE_APPLY: what the owner approved */
    uint8_t update_hash[4];
} consent_action_t;

typedef struct {
    consent_persist_t p;
    bool dirty; /* p changed since last storage save */
    bool locked;

    consent_inh_persist_t ih;
    bool ih_dirty;                  /* ih must be written to its flash region */
    uint64_t ih_persisted_seconds;  /* powered_seconds at the last successful ih save */
    uint32_t tick_last_ms;          /* uptime accounting */
    uint32_t tick_frac_ms;

    bool have_time;
    uint64_t unix_at_sync;
    uint32_t ms_at_sync;

    struct {
        consent_op_t op;
        char id[CONSENT_ID_MAX];
        consent_need_t need;
        consent_need_t approve_need; /* display-gated: the need once the screen has been shown */
        bool display_gated;          /* sign / inheritance_set / init_words: touches turn pages first */
        uint8_t n_pages, page;       /* as reported by consent_display_shown */
        bool final_shown;            /* the last page has been on the display: a touch is consent now */
        uint32_t deadline_ms;
        consent_chain_t chain; /* sign / elevate */
        uint64_t lamports;   /* sign: lamports or sats */
        u256_t wei;          /* sign: wei */
        uint64_t limit;      /* elevate (lamports / sats) */
        u256_t limit_wei;    /* elevate (wei) */
        consent_policy_req_t policy; /* policy request */
        consent_allowance_t allowance; /* allowance request */
        uint8_t pin_stage;   /* set_pin: 0 = first entry, 1 = confirm; set_duress_pin: 0 = real PIN verify,
                                1 = first entry, 2 = confirm */
        uint8_t pin[CONSENT_PIN_CODE_LEN]; /* set_pin / set_duress_pin: first entry */
        uint8_t pin_real[CONSENT_PIN_CODE_LEN]; /* set_duress_pin: the verified real code (must differ) */
        bool have_pin_real;
        consent_inheritance_t inh; /* inheritance_set request */
        char update_version[16];   /* update_apply: shown on the display */
        uint8_t update_hash[4];
    } pending;

    struct {
        bool active;
        consent_chain_t chain; /* an elevate raises one chain's limit for one request on that chain */
        uint64_t limit;
        u256_t limit_wei;
        uint32_t until_ms;
    } elevate;
} consent_t;

/* `persisted` may be NULL (fresh device). A booting initialised controller starts locked. */
void consent_init(consent_t *c, const consent_persist_t *persisted, uint32_t now_ms);
/* Loads the timer / inheritance record (NULL = none stored). Call right after consent_init(). */
void consent_init_inheritance(consent_t *c, const consent_inh_persist_t *persisted);

/* ---- time ---- */
typedef enum {
    CONSENT_TIME_SAME = 0,  /* accepted, nothing changed */
    CONSENT_TIME_CHANGED,   /* accepted; the day rolled over (counters reset) or an allowance expired */
    CONSENT_TIME_REJECTED,  /* ignored: moved backwards or jumped > CONSENT_TIME_MAX_JUMP_S ahead */
} consent_time_result_t;
/* Host wall clock (untrusted). The spend counters reset only when the host day differs from the stored one
 * AND at least CONSENT_TIME_MIN_POWERED_S of powered uptime passed since the last rollover. */
consent_time_result_t consent_set_time(consent_t *c, uint64_t unix_now, uint32_t now_ms);
bool consent_now_unix(const consent_t *c, uint32_t now_ms, uint64_t *out);
/* Call every tick. Returns true (and fills `out`) when the pending request timed out. Also advances
 * powered_seconds from the ms delta and raises ih_dirty when the hourly checkpoint is due. */
bool consent_tick(consent_t *c, uint32_t now_ms, consent_action_t *out);

/* ---- requests (host -> controller) ---- */
consent_result_t consent_begin_init(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why);
/* `programs` is the controller's own parse (used for the allowance path). `touch_only` is set for anything
 * the allowance must never cover: dangerous instructions, unknown programs, and every token outflow (the
 * allowance is a SOL budget; tokens have no budget, so moving them always takes a touch).
 * CONSENT_R_DONE means: sign right now without a touch (allowance). */
consent_result_t consent_begin_sign(consent_t *c, const char *id, uint64_t lamports_out,
                                    const uint8_t (*programs)[32], uint8_t n_programs, bool touch_only,
                                    uint32_t ttl_ms, uint32_t now_ms, consent_action_t *now, consent_reason_t *why);
/* EVM (amount in `wei`, `amount` ignored) or Bitcoin (`amount` = sats_out) transaction, or a message of any
 * chain (CONSENT_CHAIN_* with zero amount): always needs a touch, the allowance never applies. Check 6:
 * spent_today(chain) + amount <= daily_limit(chain), or the elevated limit when the elevate is for that chain. */
consent_result_t consent_begin_sign_chain(consent_t *c, const char *id, consent_chain_t chain, uint64_t amount,
                                          const u256_t *wei, uint32_t ttl_ms, uint32_t now_ms, consent_reason_t *why);
/* Same, with the kind of consent the request needs (a permit-like typed-data signature takes a long touch). */
consent_result_t consent_begin_sign_chain_need(consent_t *c, const char *id, consent_chain_t chain, uint64_t amount,
                                               const u256_t *wei, uint32_t ttl_ms, uint32_t now_ms, consent_need_t need, consent_reason_t *why);
consent_result_t consent_begin_elevate(consent_t *c, const char *id, uint64_t limit, uint32_t now_ms,
                                       consent_reason_t *why);
/* Per-chain elevate: `limit` for SOL/BTC, `limit_wei` for EVM. */
consent_result_t consent_begin_elevate_chain(consent_t *c, const char *id, consent_chain_t chain, uint64_t limit,
                                             const u256_t *limit_wei, uint32_t now_ms, consent_reason_t *why);
/* Needs the PIN rhythm once a PIN is set, a touch before that (init flow not finished). */
consent_result_t consent_begin_unlock(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why);
/* Needs the PIN rhythm (denied pin_not_set before set_pin). */
consent_result_t consent_begin_policy(consent_t *c, const char *id, uint64_t daily_limit, uint32_t now_ms,
                                      consent_reason_t *why);
/* Any subset of the three per-chain limits in one PIN-gated request (denied policy when none is set). */
/* Reboot into a staged firmware image. Owner authority when a wallet exists (PIN verified, not duress);
 * a blank controller may be updated with a long touch alone. */
consent_result_t consent_begin_update_apply(consent_t *c, const char *id, const char *version, const uint8_t hash[4],
                                            uint32_t now_ms, consent_reason_t *why);
consent_result_t consent_begin_policy_multi(consent_t *c, const char *id, const consent_policy_req_t *req,
                                            uint32_t now_ms, consent_reason_t *why);
consent_result_t consent_begin_allowance(consent_t *c, const char *id, const consent_allowance_t *a,
                                         uint32_t now_ms, consent_reason_t *why);
/* set_pin: after init, once. The rhythm is entered twice (CONSENT_RHYTHM_CONFIRM after the first). */
consent_result_t consent_begin_set_pin(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why);
/* set_duress_pin: a second rhythm, entered twice, that must differ from the real one (pin_mismatch otherwise).
 * `real_code` = the real rhythm the caller just saw verified (init flow step 3, right after set_pin): the
 * request then starts at the first entry. NULL = the owner first taps the real rhythm (CONSENT_RHYTHM_VERIFY,
 * then consent_pin_result -> CONSENT_PIN_CONTINUE). Refused in duress and while locked; replaceable. */
consent_result_t consent_begin_set_duress_pin(consent_t *c, const char *id, const uint8_t *real_code, uint32_t now_ms,
                                              consent_reason_t *why);
/* decoy_addresses: PIN-gated read, refused in duress. CONSENT_PIN_OK -> the caller sends the decoy slot's keys. */
consent_result_t consent_begin_decoy_addresses(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why);
/* inheritance_set: `req` = inactivity / warn thresholds / heir / signer (= the current real public key;
 * armed and warned are ignored). `durable_nonce` = the caller verified that the message starts with
 * System advanceNonce on an account that is not the owner key. Needs a long touch to arm, a plain touch
 * to replace an existing arrangement, and replacing is only possible with the key that signed it. */
consent_result_t consent_begin_inheritance_set(consent_t *c, const char *id, const consent_inheritance_t *req,
                                               bool durable_nonce, uint32_t now_ms, consent_reason_t *why);
/* inheritance_clear: long touch, owner authority (initialised, PIN set, unlocked, not in duress) and `owner`
 * (the current real public key) must be the key that signed the arrangement: after a wipe + re-init nobody
 * can clear it any more, it can only be released. Denied not_armed when nothing is stored. */
consent_result_t consent_begin_inheritance_clear(consent_t *c, const char *id, const uint8_t owner[32],
                                                 uint32_t now_ms, consent_reason_t *why);
/* inheritance_release: immediate verdict, independent of lock / duress / initialised state. */
consent_result_t consent_inheritance_release(const consent_t *c, consent_reason_t *why);
/* Immediate. Returns true and fills `cancelled` if a pending request was cancelled. */
bool consent_lock(consent_t *c, consent_action_t *cancelled);
/* Drops the pending request without any other effect (e.g. the chip could not be reached). */
bool consent_cancel_pending(consent_t *c, consent_action_t *cancelled);
bool consent_duress(consent_t *c, consent_action_t *cancelled);
bool consent_allowance_revoke(consent_t *c);

/* After a successful init (keys imported): the owner must tap through `n_pages` word pages on the trusted
 * display before the device counts as initialised. Display gated; TTL CONSENT_INIT_WORDS_TTL_MS; a long
 * touch cancels (the caller then destroys the keys). */
consent_result_t consent_begin_init_words(consent_t *c, const char *id, uint8_t n_pages, uint32_t now_ms,
                                          consent_reason_t *why);

/* ---- trusted display ---- */
/* The caller rendered page `page` of `n_pages` for the pending display-gated request. Moves
 * CONSENT_NEED_DISPLAY to the approving need; marks the final page as shown when page == n_pages - 1.
 * Returns false when nothing display-gated is pending. */
bool consent_display_shown(consent_t *c, uint8_t page, uint8_t n_pages);
bool consent_pending_display_gated(const consent_t *c);
uint8_t consent_pending_page(const consent_t *c);
uint8_t consent_pending_pages(const consent_t *c);

/* ---- physical input ---- */
typedef enum {
    CONSENT_TOUCH_NONE = 0,  /* nothing happened */
    CONSENT_TOUCH_APPROVED,  /* `out` = the approved action */
    CONSENT_TOUCH_PAGE,      /* display gated: render page `out->page` next, then call consent_display_shown */
    CONSENT_TOUCH_CANCELLED, /* display gated touch request: long touch = cancel; `out` carries op + id */
} consent_touch_result_t;
/* `long_touch` = pad held >= 3 s. Ignored while the pending request needs the PIN (taps are rhythm input
 * then) or the display (screen not rendered yet). Always counts as proof of life for the inheritance timer. */
consent_touch_result_t consent_touch_ex(consent_t *c, bool long_touch, uint32_t now_ms, consent_action_t *out);
/* Compatibility wrapper: true only for CONSENT_TOUCH_APPROVED. */
bool consent_touch(consent_t *c, bool long_touch, uint32_t now_ms, consent_action_t *out);
/* Proof of life without approval semantics: call on every debounced finger-down (covers rhythm taps). */
void consent_touch_seen(consent_t *c);

typedef enum {
    CONSENT_RHYTHM_NONE = 0, /* nothing pending that needs a PIN */
    CONSENT_RHYTHM_CONFIRM,  /* set_pin: first entry stored, ask for it again */
    CONSENT_RHYTHM_SET,      /* set_pin / set_duress_pin: both entries match; `out` carries the op and the code */
    CONSENT_RHYTHM_MISMATCH, /* set_pin: confirmation differed; request denied (pin_mismatch) */
    CONSENT_RHYTHM_VERIFY,   /* `out` = the gated op + code; caller verifies with the signer, then
                                calls consent_pin_result(). The request stays pending meanwhile. */
} consent_rhythm_result_t;
consent_rhythm_result_t consent_rhythm(consent_t *c, const uint8_t code[CONSENT_PIN_CODE_LEN], uint32_t now_ms,
                                       consent_action_t *out);

typedef enum {
    CONSENT_PIN_NONE = 0, /* nothing was waiting for a verification */
    CONSENT_PIN_OK,       /* op completed, `out` filled; failures reset */
    CONSENT_PIN_WRONG,    /* request denied (wrong_pin), failures incremented */
    CONSENT_PIN_WIPE,     /* fifth failure: caller must wipe storage and the chip; consent reset */
    CONSENT_PIN_CONTINUE, /* set_duress_pin: real rhythm verified, now the new rhythm (twice); request stays pending */
} consent_pin_result_t;
/* What the signer said about the rhythm. */
typedef enum {
    CONSENT_VERDICT_WRONG = 0, /* matched neither object */
    CONSENT_VERDICT_REAL,      /* the real PIN */
    CONSENT_VERDICT_DURESS,    /* the second rhythm: an `unlock` completes exactly like a real one but into the
                                  decoy slot (duress set, allowance and elevate cleared); the failure counter is
                                  untouched. For every other PIN-gated op it counts as WRONG. */
} consent_verdict_t;
consent_pin_result_t consent_pin_result(consent_t *c, bool verified, uint32_t now_ms, consent_action_t *out);
consent_pin_result_t consent_pin_verdict(consent_t *c, consent_verdict_t v, uint32_t now_ms, consent_action_t *out);

/* ---- completion ---- */
/* After a successful signature: accounts the spend (daily + allowance) and consumes an elevate. */
void consent_commit_spend(consent_t *c, const consent_action_t *a);
/* After `init` produced keys. */
void consent_mark_initialized(consent_t *c);
/* After the signer created the PIN object. */
void consent_mark_pin_set(consent_t *c);
/* After the signer created / replaced the second (duress) object. */
void consent_mark_duress_pin_set(consent_t *c);
/* Back to factory state (after a wipe). The inheritance timer (`ih`) is deliberately kept. */
void consent_wipe(consent_t *c);
/* After the caller stored the signed transaction: arms `ih.inh` from the completed INHERITANCE_SET action. */
void consent_commit_inheritance(consent_t *c, const consent_action_t *a);
/* After a successful save of `ih` to flash. */
void consent_inheritance_persisted(consent_t *c);
/* Next warning to emit, once per threshold: returns true and the threshold's days_left. Call until false. */
bool consent_inheritance_warning(consent_t *c, uint32_t *days_left);

/* ---- queries ---- */
uint64_t consent_seconds_since_touch(const consent_t *c);
/* Tenths of a day since the last touch (12.4 days -> 124), saturating. */
uint32_t consent_days_since_touch_tenths(const consent_t *c);
uint8_t consent_active_slot(const consent_t *c);
consent_need_t consent_pending_need(const consent_t *c);
bool consent_allowance_active(const consent_t *c, uint32_t now_ms);
const char *consent_reason_str(consent_reason_t r);
const char *consent_need_str(consent_need_t n);
#endif
