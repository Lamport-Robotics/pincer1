/* Pincer1 - swap-router allowlist kept ON THE CONTROLLER. PURE LOGIC.
 *
 * The daemon used to hand the controller a `claims.routers` list, which meant a compromised daemon could
 * whitelist any contract. The list now lives here: a built-in default set compiled into the firmware plus
 * up to ROUTERS_MAX_OWNER entries the owner adds through a PIN-gated `policy` request (persisted in the main
 * flash record, shown on the trusted display when added). `claims.routers` is informational only.
 *
 * A call into an address that is on neither list is `unknown_contract` (EVM) / `unknown_program` (Solana):
 * it must be declared by the daemon and is rendered as DANGER on the trusted display. */
#ifndef PINCER_ROUTERS_H
#define PINCER_ROUTERS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUTERS_MAX_OWNER 8

typedef enum { ROUTER_CHAIN_SOL = 0, ROUTER_CHAIN_EVM = 1 } router_chain_t;

typedef struct {
    uint8_t chain;    /* router_chain_t */
    uint8_t addr[32]; /* Solana: 32-byte program id; EVM: 20-byte address in addr[0..19], rest zero */
} router_entry_t;

/* Owner-added entries (persisted). */
typedef struct {
    uint8_t n;
    router_entry_t e[ROUTERS_MAX_OWNER];
} router_list_t;

/* Built-in defaults: Solana Jupiter v6 program; EVM 1inch v6 and 0x Exchange Proxy (same address on every
 * EVM chain). THORChain routers are chain-specific and deliberately absent: the owner adds the one they use. */
const router_entry_t *routers_builtin(size_t *n);

/* Byte length of an address for a chain: 32 (Solana) or 20 (EVM). 0 for an unknown chain. */
size_t routers_addr_len(uint8_t chain);
/* True when `addr` is a built-in router or an owner-added one for `chain`. */
bool routers_is_allowed(const router_list_t *owner, uint8_t chain, const uint8_t *addr);
/* 0 added, 1 already present (built-in or owner), -1 full / bad chain. */
int routers_add(router_list_t *owner, uint8_t chain, const uint8_t *addr);
/* True when an owner entry was removed (built-ins cannot be removed). */
bool routers_remove(router_list_t *owner, uint8_t chain, const uint8_t *addr);
/* "solana" / "evm" <-> chain. */
const char *routers_chain_name(uint8_t chain);
bool routers_chain_from_name(const char *name, uint8_t *chain);
#endif
