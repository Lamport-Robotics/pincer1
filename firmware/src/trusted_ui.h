/* Pincer1 - trusted UI: what the consent controller shows on its own display. PURE LOGIC.
 *
 * Every screen is built from the controller's OWN parse of a request (solmsg / evmtx / btcpsbt summaries and
 * the effective dangerous flags policy.c computed), never from host-supplied text: the `summary` the daemon
 * speaks is a convenience, the display is the consent channel. Screens are documents of up to TUI_MAX_PAGES
 * pages of up to TUI_MAX_LINES lines (48 px of content above a 16 px footer); a short touch turns the page,
 * the final page's footer says what the next touch means ("TOUCH TO SIGN · hold 3s = cancel").
 *
 * Addresses are shown as the first 6 and last 6 characters of the chain's own encoding (base58 for Solana,
 * EIP-55 hex after the 0x prefix for EVM, bech32 / base58check for Bitcoin), amounts in native units
 * (SOL / ETH / BTC) with trailing zeros trimmed, token amounts in raw units with the mint / contract short form,
 * approvals and unknown contracts in a boxed line. */
#ifndef PINCER_TRUSTED_UI_H
#define PINCER_TRUSTED_UI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btcpsbt.h"
#include "display.h"
#include "evmtx.h"
#include "routers.h"
#include "solmsg.h"
#include "u256.h"
#include "eip712.h"

#define TUI_LINE_BYTES 40 /* 21 cells; UTF-8 for "·" and "…" takes extra bytes */
#define TUI_MAX_LINES 6
#define TUI_MAX_PAGES 8
#define TUI_CONTENT_H 48
#define TUI_FOOTER_Y 48

typedef enum {
    TUI_TEXT = 0, /* 5x7, 8 px */
    TUI_HEAD,     /* inverted bar, 8 px */
    TUI_BIG,      /* 8x16, 16 px */
    TUI_BOX,      /* boxed 5x7 line (warnings), 12 px */
} tui_style_t;

typedef struct {
    uint8_t style;
    char text[TUI_LINE_BYTES];
} tui_line_t;

typedef struct {
    uint8_t n_lines;
    tui_line_t l[TUI_MAX_LINES];
} tui_page_t;

typedef struct {
    uint8_t n_pages;
    bool truncated; /* ran out of pages: the caller must NOT let this request be signed */
    tui_page_t p[TUI_MAX_PAGES];
} tui_doc_t;

/* What the final page asks for. */
typedef enum {
    TUI_FOOT_NONE = 0, /* idle / notices: nothing */
    TUI_FOOT_SIGN,     /* "TOUCH TO SIGN · hold 3s = cancel" */
    TUI_FOOT_HOLD,     /* "HOLD 3s TO APPROVE" (long-touch requests) */
    TUI_FOOT_NEXT,     /* "TOUCH = next" (word pages) */
    TUI_FOOT_PIN,      /* "tap your rhythm · pause 1.5s = done" */
    TUI_FOOT_FINISH,   /* "TOUCH TO FINISH" (last word page) */
    TUI_FOOT_APPROVE,  /* "TOUCH TO APPROVE" (unlock before a PIN exists) */
} tui_foot_t;

/* ---- document building ---- */
void tui_doc_init(tui_doc_t *doc);
/* Appends a line, starting a new page when it does not fit; over-long text is cut with "…". */
void tui_add(tui_doc_t *doc, tui_style_t style, const char *text);
/* Appends text without cutting: continues on further lines (amounts, digests). */
void tui_add_wrap(tui_doc_t *doc, tui_style_t style, const char *text);
/* printf-style convenience (small formats only). */
void tui_addf(tui_doc_t *doc, tui_style_t style, const char *fmt, ...);

/* ---- sign requests ---- */
typedef enum {
    TUI_SIGN_SOL_TX = 0,
    TUI_SIGN_SOL_MSG,
    TUI_SIGN_EVM_TX,
    TUI_SIGN_EVM_MSG,
    TUI_SIGN_EVM_TYPED,
    TUI_SIGN_BTC_PSBT,
    TUI_SIGN_BTC_MSG,
    TUI_SIGN_INHERITANCE, /* Solana tx + arrangement */
} tui_sign_kind_t;

typedef struct {
    tui_sign_kind_t kind;
    const solmsg_summary_t *sol; /* SOL_TX / INHERITANCE */
    uint32_t sol_dangerous;      /* effective flags from policy_check */
    const evmtx_t *evm;          /* EVM_TX */
    uint32_t evm_dangerous;
    const btcpsbt_t *btc;        /* BTC_PSBT */
    uint32_t btc_dangerous;
    const uint8_t *msg;          /* *_MSG: the bytes */
    size_t msg_len;
    const uint8_t *digest;       /* EVM_TYPED: the 32-byte digest the controller computed */
    const eip712_desc_t *typed;  /* EVM_TYPED: the struct, domain and fields the digest covers */
    uint16_t inh_days;           /* INHERITANCE: inactivity days */
    const uint8_t *inh_heir;     /* INHERITANCE: heir key */
    bool inh_replace;            /* INHERITANCE: replacing (touch) vs arming (long touch) */
} tui_sign_t;

void tui_build_sign(const tui_sign_t *req, tui_doc_t *doc);
/* The footer the final page of a sign document needs. */
tui_foot_t tui_sign_footer(const tui_sign_t *req);

/* ---- other screens (single page) ---- */
typedef struct {
    bool initialized, locked, pin_set;
    const uint8_t *sol;     /* 32 or NULL */
    const uint8_t *evm;     /* 20 or NULL */
    const uint8_t *btc_pkh; /* 20 or NULL */
} tui_idle_t;
void tui_build_idle(const tui_idle_t *st, tui_page_t *page);
/* PIN entry: one dot per tap, never the rhythm. `what` = "UNLOCK" / "SET PIN" / "POLICY"; `detail` may be NULL. */
void tui_build_pin(const char *what, const char *detail, const char *detail2, uint8_t taps, bool confirm, tui_page_t *page);
/* Notices (denied reason, "SIGNED", "LOCKED", "ROUTER ADDED" ...). line2 may be NULL. */
void tui_build_notice(const char *head, const char *line1, const char *line2, tui_page_t *page);
/* Long-touch prompts (init, elevate, allowance, inheritance_clear). */
void tui_build_prompt(const char *head, const char *line1, const char *line2, tui_page_t *page);
/* A router being added / removed: "<head> · EVM|SOLANA", the short form boxed, the full address wrapped. */
void tui_build_router(const char *head, const router_entry_t *e, tui_page_t *page);
/* Appends a line to a single page (dropped when it does not fit). */
void tui_page_add(tui_page_t *page, tui_style_t style, const char *text);

/* ---- seed words after init ----
 * 28 pages: real words 1-24 (4 per page), "write them down? tap through again", real words again, decoy intro,
 * decoy words, confirm, decoy words again, finish. The caller zeroises the mnemonics afterwards. */
#define TUI_WORDS_PAGES 28
/* Builds page `idx` (0-based) from the two space-separated 24-word mnemonics; returns its footer. */
tui_foot_t tui_build_words_page(uint8_t idx, const char *real, const char *decoy, tui_page_t *page);

/* ---- rendering (framebuffer only; the caller flushes) ---- */
/* `page`/`pages` are 1-based for the footer; `foot` applies to the final page, earlier pages get "TOUCH = next";
 * `cancelable` prints "hold 3s = cancel". */
void tui_render(display_t *d, const tui_page_t *pg, tui_foot_t foot, uint8_t page, uint8_t pages, bool cancelable);
void tui_render_doc(display_t *d, const tui_doc_t *doc, uint8_t page_idx, tui_foot_t final_foot, bool cancelable);

/* ---- formatting helpers (exposed for tests) ---- */
/* 500000000, 9 -> "0.5"; 0 -> "0"; trailing zeros trimmed. */
void tui_fmt_units(uint64_t v, uint8_t decimals, char *out, size_t cap);
void tui_fmt_units_u256(const u256_t *v, uint8_t decimals, char *out, size_t cap);
/* "abcdefghijklmnop" -> "abcdef…klmnop" (unchanged when 13 chars or shorter). */
void tui_short(const char *full, char *out, size_t cap);
void tui_short_b58(const uint8_t key[32], char *out, size_t cap);
/* "0x" + first 6 + "…" + last 6 of the EIP-55 hex digits. */
void tui_short_evm(const uint8_t addr[20], char *out, size_t cap);
/* Address form of a scriptPubKey, shortened; "(no address)" for non-standard scripts. */
void tui_short_btc_script(const uint8_t *spk, size_t len, char *out, size_t cap);
/* Chain name / native unit for an EVM chain id ("Ethereum"/"ETH"; unknown -> "chain N"/"NATIVE"). */
const char *tui_evm_chain_name(uint64_t chain_id);
const char *tui_evm_unit(uint64_t chain_id);
#endif
