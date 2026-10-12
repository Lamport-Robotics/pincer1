#include "routers.h"

#include <string.h>

#include "util.h"

/* Jupiter aggregator v6 program (Solana), decoded from its published base58 form on first use; test_policy
 * checks the round trip. */
static const char JUPITER_V6_B58[] = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4";
/* 0x111111125421cA6dc452d289314280a0f8842A65 (1inch Aggregation Router v6) */
static const uint8_t ONEINCH_V6[20] = {0x11, 0x11, 0x11, 0x12, 0x54, 0x21, 0xca, 0x6d, 0xc4, 0x52,
                                       0xd2, 0x89, 0x31, 0x42, 0x80, 0xa0, 0xf8, 0x84, 0x2a, 0x65};
/* 0xDef1C0ded9bec7F1a1670819833240f027b25EfF (0x Exchange Proxy) */
static const uint8_t ZEROX_PROXY[20] = {0xde, 0xf1, 0xc0, 0xde, 0xd9, 0xbe, 0xc7, 0xf1, 0xa1, 0x67,
                                        0x08, 0x19, 0x83, 0x32, 0x40, 0xf0, 0x27, 0xb2, 0x5e, 0xff};

static router_entry_t g_builtin[3];
static bool g_builtin_ready;

const router_entry_t *routers_builtin(size_t *n)
{
    if (!g_builtin_ready) {
        memset(g_builtin, 0, sizeof g_builtin);
        g_builtin[0].chain = ROUTER_CHAIN_SOL;
        if (!pubkey_from_b58(JUPITER_V6_B58, g_builtin[0].addr)) memset(g_builtin[0].addr, 0xff, 32); /* never matches */
        g_builtin[1].chain = ROUTER_CHAIN_EVM; memcpy(g_builtin[1].addr, ONEINCH_V6, 20);
        g_builtin[2].chain = ROUTER_CHAIN_EVM; memcpy(g_builtin[2].addr, ZEROX_PROXY, 20);
        g_builtin_ready = true;
    }
    *n = sizeof g_builtin / sizeof g_builtin[0];
    return g_builtin;
}

size_t routers_addr_len(uint8_t chain)
{
    switch (chain) {
    case ROUTER_CHAIN_SOL: return 32;
    case ROUTER_CHAIN_EVM: return 20;
    default: return 0;
    }
}

static bool entry_is(const router_entry_t *e, uint8_t chain, const uint8_t *addr, size_t len)
{
    return e->chain == chain && memcmp(e->addr, addr, len) == 0;
}

bool routers_is_allowed(const router_list_t *owner, uint8_t chain, const uint8_t *addr)
{
    size_t len = routers_addr_len(chain), n, i;
    const router_entry_t *b;
    if (!len || !addr) return false;
    b = routers_builtin(&n);
    for (i = 0; i < n; i++)
        if (entry_is(&b[i], chain, addr, len)) return true;
    if (owner)
        for (i = 0; i < owner->n && i < ROUTERS_MAX_OWNER; i++)
            if (entry_is(&owner->e[i], chain, addr, len)) return true;
    return false;
}

int routers_add(router_list_t *owner, uint8_t chain, const uint8_t *addr)
{
    size_t len = routers_addr_len(chain);
    router_entry_t *e;
    if (!len || !addr) return -1;
    if (routers_is_allowed(owner, chain, addr)) return 1;
    if (owner->n >= ROUTERS_MAX_OWNER) return -1;
    e = &owner->e[owner->n++];
    memset(e, 0, sizeof *e);
    e->chain = chain;
    memcpy(e->addr, addr, len);
    return 0;
}

bool routers_remove(router_list_t *owner, uint8_t chain, const uint8_t *addr)
{
    size_t len = routers_addr_len(chain), i;
    if (!len || !addr) return false;
    for (i = 0; i < owner->n && i < ROUTERS_MAX_OWNER; i++) {
        if (entry_is(&owner->e[i], chain, addr, len)) {
            memmove(&owner->e[i], &owner->e[i + 1], (owner->n - i - 1) * sizeof owner->e[0]);
            owner->n--;
            memset(&owner->e[owner->n], 0, sizeof owner->e[0]);
            return true;
        }
    }
    return false;
}

const char *routers_chain_name(uint8_t chain)
{
    return chain == ROUTER_CHAIN_SOL ? "solana" : chain == ROUTER_CHAIN_EVM ? "evm" : "?";
}

bool routers_chain_from_name(const char *name, uint8_t *chain)
{
    if (!name) return false;
    if (strcmp(name, "solana") == 0 || strcmp(name, "sol") == 0) { *chain = ROUTER_CHAIN_SOL; return true; }
    if (strcmp(name, "evm") == 0) { *chain = ROUTER_CHAIN_EVM; return true; }
    return false;
}
