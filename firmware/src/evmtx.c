#include "evmtx.h"

#include <string.h>

#include "keccak.h"

/* ---- strict RLP ---- */
typedef struct {
    const uint8_t *p;
    size_t len;
    bool is_list;
} rlp_item_t;

/* Decodes the item at `in`; returns bytes consumed or 0 on error. Enforces canonical encoding. */
static size_t rlp_item(const uint8_t *in, size_t len, rlp_item_t *it)
{
    uint8_t b;
    size_t hdr, plen, i;
    if (len == 0) return 0;
    b = in[0];
    if (b < 0x80) {
        it->p = in; it->len = 1; it->is_list = false;
        return 1;
    }
    if (b <= 0xb7) {
        plen = b - 0x80;
        hdr = 1;
        if (plen == 1 && len >= 2 && in[1] < 0x80) return 0; /* single byte must be encoded as itself */
        it->is_list = false;
    } else if (b <= 0xbf) {
        size_t ll = b - 0xb7;
        if (ll > 4 || 1 + ll > len) return 0;
        if (in[1] == 0) return 0; /* leading zero in length */
        plen = 0;
        for (i = 0; i < ll; i++) plen = (plen << 8) | in[1 + i];
        if (plen < 56) return 0; /* should have used the short form */
        hdr = 1 + ll;
        it->is_list = false;
    } else if (b <= 0xf7) {
        plen = b - 0xc0;
        hdr = 1;
        it->is_list = true;
    } else {
        size_t ll = b - 0xf7;
        if (ll > 4 || 1 + ll > len) return 0;
        if (in[1] == 0) return 0;
        plen = 0;
        for (i = 0; i < ll; i++) plen = (plen << 8) | in[1 + i];
        if (plen < 56) return 0;
        hdr = 1 + ll;
        it->is_list = true;
    }
    if (hdr + plen > len) return 0;
    it->p = in + hdr;
    it->len = plen;
    return hdr + plen;
}

/* Splits a list payload into at most `max` items; returns the count or -1. */
static int rlp_list_items(const rlp_item_t *list, rlp_item_t *items, int max)
{
    size_t off = 0;
    int n = 0;
    while (off < list->len) {
        size_t used;
        if (n >= max) return -1;
        used = rlp_item(list->p + off, list->len - off, &items[n]);
        if (!used) return -1;
        off += used;
        n++;
    }
    return n;
}

/* Canonical unsigned integer: no leading zeros, at most 32 bytes. */
static bool rlp_u256(const rlp_item_t *it, u256_t *out)
{
    if (it->is_list || it->len > 32) return false;
    if (it->len > 0 && it->p[0] == 0) return false;
    return u256_from_be(out, it->p, it->len);
}

static bool rlp_u64(const rlp_item_t *it, uint64_t *out)
{
    u256_t v;
    if (!rlp_u256(it, &v) || !u256_fits_u64(&v)) return false;
    *out = u256_low64(&v);
    return true;
}

static bool rlp_address(const rlp_item_t *it, bool *has, uint8_t out[20])
{
    if (it->is_list) return false;
    if (it->len == 0) { *has = false; memset(out, 0, 20); return true; }
    if (it->len != 20) return false;
    memcpy(out, it->p, 20);
    *has = true;
    return true;
}

int evmtx_parse(const uint8_t *bytes, size_t len, evmtx_t *out)
{
    rlp_item_t list, f[12];
    int n;
    size_t used, off = 0;
    memset(out, 0, sizeof *out);
    if (len == 0 || len > EVMTX_MAX_BYTES) return EVMTX_E_SIZE;
    if (bytes[0] < 0x80) {
        /* typed envelope */
        if (bytes[0] != 0x02) return EVMTX_E_TYPE;
        out->type = 2;
        off = 1;
    } else if (bytes[0] >= 0xc0) {
        out->type = 0;
    } else {
        return EVMTX_E_RLP;
    }
    used = rlp_item(bytes + off, len - off, &list);
    if (!used || !list.is_list) return EVMTX_E_RLP;
    if (off + used != len) return EVMTX_E_RLP; /* trailing bytes */
    n = rlp_list_items(&list, f, 12);
    if (n < 0) return EVMTX_E_RLP;
    if (out->type == 0) {
        /* [nonce, gasPrice, gasLimit, to, value, data, chainId, 0, 0] */
        if (n != 9) return EVMTX_E_FIELDS;
        if (!rlp_u64(&f[0], &out->nonce) || !rlp_u256(&f[1], &out->max_fee_per_gas) || !rlp_u64(&f[2], &out->gas_limit))
            return EVMTX_E_RANGE;
        if (!rlp_address(&f[3], &out->has_to, out->to)) return EVMTX_E_FIELDS;
        if (!rlp_u256(&f[4], &out->value)) return EVMTX_E_RANGE;
        if (f[5].is_list) return EVMTX_E_FIELDS;
        out->data = f[5].p;
        out->data_len = f[5].len;
        if (!rlp_u64(&f[6], &out->chain_id) || out->chain_id == 0) return EVMTX_E_RANGE;
        if (f[7].is_list || f[7].len != 0 || f[8].is_list || f[8].len != 0) return EVMTX_E_FIELDS;
    } else {
        /* [chainId, nonce, maxPriorityFeePerGas, maxFeePerGas, gasLimit, to, value, data, accessList] */
        if (n != 9) return EVMTX_E_FIELDS;
        if (!rlp_u64(&f[0], &out->chain_id) || out->chain_id == 0 || !rlp_u64(&f[1], &out->nonce) ||
            !rlp_u256(&f[2], &out->max_priority_fee) || !rlp_u256(&f[3], &out->max_fee_per_gas) ||
            !rlp_u64(&f[4], &out->gas_limit))
            return EVMTX_E_RANGE;
        if (!rlp_address(&f[5], &out->has_to, out->to)) return EVMTX_E_FIELDS;
        if (!rlp_u256(&f[6], &out->value)) return EVMTX_E_RANGE;
        if (f[7].is_list) return EVMTX_E_FIELDS;
        out->data = f[7].p;
        out->data_len = f[7].len;
        if (!f[8].is_list) return EVMTX_E_FIELDS;
        out->access_list_len = f[8].len;
        if (u256_cmp(&out->max_priority_fee, &out->max_fee_per_gas) > 0) return EVMTX_E_RANGE;
    }
    return EVMTX_OK;
}

static uint32_t selector(const uint8_t *d)
{
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}

/* ABI address word: 12 zero bytes then 20 */
static bool abi_address(const uint8_t *w, uint8_t out[20])
{
    static const uint8_t zero12[12] = {0};
    if (memcmp(w, zero12, 12) != 0) return false;
    memcpy(out, w + 12, 20);
    return true;
}

static bool abi_bool(const uint8_t *w, bool *out)
{
    static const uint8_t zero31[31] = {0};
    if (memcmp(w, zero31, 31) != 0 || w[31] > 1) return false;
    *out = w[31] == 1;
    return true;
}

void evmtx_analyze(evmtx_t *tx, const uint8_t owner[20])
{
    u256_t gl;
    tx->call = EVMTX_CALL_NONE;
    tx->dangerous = 0;
    memset(tx->call_to, 0, 20);
    memset(tx->call_from, 0, 20);
    u256_zero(&tx->call_amount);
    tx->call_flag = false;
    u256_from_u64(&gl, tx->gas_limit);
    u256_mul(&tx->max_fee_total, &gl, &tx->max_fee_per_gas); /* saturates to max on overflow */
    if (!tx->has_to) {
        tx->call = EVMTX_CALL_CREATE;
        return;
    }
    if (tx->data_len == 0) return;
    if (tx->data_len < 4) { tx->call = EVMTX_CALL_UNKNOWN; return; }
    tx->call = EVMTX_CALL_UNKNOWN;
    switch (selector(tx->data)) {
    case EVMTX_SELECTOR_TRANSFER:
        if (tx->data_len != 4 + 64 || !abi_address(tx->data + 4, tx->call_to)) return;
        u256_from_be(&tx->call_amount, tx->data + 36, 32);
        tx->call = EVMTX_CALL_TRANSFER;
        break;
    case EVMTX_SELECTOR_APPROVE:
    case EVMTX_SELECTOR_INCREASE_ALLOWANCE:
        if (tx->data_len != 4 + 64 || !abi_address(tx->data + 4, tx->call_to)) return;
        u256_from_be(&tx->call_amount, tx->data + 36, 32);
        tx->call = EVMTX_CALL_APPROVE;
        tx->dangerous |= u256_is_max(&tx->call_amount) ? EVMTX_DANGER_APPROVE_UNLIMITED : EVMTX_DANGER_APPROVE;
        break;
    case EVMTX_SELECTOR_TRANSFER_FROM:
        if (tx->data_len != 4 + 96 || !abi_address(tx->data + 4, tx->call_from) || !abi_address(tx->data + 36, tx->call_to))
            return;
        u256_from_be(&tx->call_amount, tx->data + 68, 32);
        tx->call = EVMTX_CALL_TRANSFER_FROM;
        (void)owner;
        break;
    case EVMTX_SELECTOR_SET_APPROVAL_FOR_ALL:
        if (tx->data_len != 4 + 64 || !abi_address(tx->data + 4, tx->call_to) || !abi_bool(tx->data + 36, &tx->call_flag))
            return;
        tx->call = EVMTX_CALL_SET_APPROVAL;
        if (tx->call_flag) tx->dangerous |= EVMTX_DANGER_SET_APPROVAL_FOR_ALL;
        break;
    default:
        break;
    }
}

void evmtx_signing_digest(const uint8_t *bytes, size_t len, uint8_t digest[32]) { keccak256(bytes, len, digest); }

void evmtx_encode_signature(const evmtx_t *tx, const uint8_t sig[64], uint8_t recid, uint8_t out[65])
{
    memcpy(out, sig, 64);
    out[64] = (uint8_t)(tx->type == 0 ? 27 + (recid & 1) : (recid & 1));
}

void evmtx_personal_digest(const uint8_t *msg, size_t len, uint8_t digest[32])
{
    static const char prefix[] = "\x19" "Ethereum Signed Message:\n"; /* split: \x19E would be one escape */
    char dec[21];
    int n = 0;
    size_t v = len;
    keccak_ctx c;
    do { dec[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    keccak256_init(&c);
    keccak256_update(&c, (const uint8_t *)prefix, sizeof prefix - 1);
    while (n) keccak256_update(&c, (const uint8_t *)&dec[--n], 1);
    keccak256_update(&c, msg, len);
    keccak256_final(&c, digest);
}

bool evmtx_looks_like_tx(const uint8_t *bytes, size_t len)
{
    evmtx_t t;
    return evmtx_parse(bytes, len, &t) == EVMTX_OK;
}

const char *evmtx_danger_name(uint32_t bit)
{
    switch (bit) {
    case EVMTX_DANGER_APPROVE: return "approve";
    case EVMTX_DANGER_APPROVE_UNLIMITED: return "approve_unlimited";
    case EVMTX_DANGER_SET_APPROVAL_FOR_ALL: return "set_approval_for_all";
    case EVMTX_DANGER_UNKNOWN_CONTRACT: return "unknown_contract";
    default: return NULL;
    }
}

uint32_t evmtx_danger_from_name(const char *name)
{
    uint32_t bit;
    if (!name) return 0;
    for (bit = 1; bit <= EVMTX_DANGER_ALL; bit <<= 1) {
        const char *n = evmtx_danger_name(bit);
        if (n && strcmp(n, name) == 0) return bit;
    }
    return 0;
}
