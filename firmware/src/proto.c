#include "proto.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "chainaddr.h"
#include "eip712.h"
#include "routers.h"
#include "util.h"

void proto_framer_init(proto_framer_t *f) { memset(f, 0, sizeof *f); }

bool proto_framer_push(proto_framer_t *f, char c, const char **line)
{
    if (c == '\n') {
        bool ok = !f->overflow && f->len > 0;
        f->buf[f->len] = 0;
        f->len = 0;
        f->overflow = false;
        if (ok) {
            *line = f->buf;
            return true;
        }
        return false;
    }
    if (c == '\r') return false;
    if (f->overflow) return false;
    if (f->len + 1 >= sizeof f->buf) {
        f->overflow = true;
        f->len = 0;
        return false;
    }
    f->buf[f->len++] = c;
    return false;
}

/* ---- parsing helpers ---- */

static bool get_u64(const cJSON *obj, const char *key, uint64_t *out, bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!v) return !required;
    if (cJSON_IsString(v)) return u64_from_dec(v->valuestring, out);
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d < 0 || d > 9007199254740992.0 || d != (double)(uint64_t)d) return false;
        *out = (uint64_t)d;
        return true;
    }
    return false;
}

/* decimal string, "0x" hex string or exact JSON integer -> u256 */
static bool get_u256(const cJSON *obj, const char *key, u256_t *out, bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    u256_zero(out);
    if (!v) return !required;
    if (cJSON_IsString(v) && v->valuestring) {
        if (v->valuestring[0] == '0' && (v->valuestring[1] == 'x' || v->valuestring[1] == 'X')) return u256_from_hex(v->valuestring, out);
        return u256_from_dec(v->valuestring, out);
    }
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        if (d < 0 || d > 9007199254740992.0 || d != (double)(uint64_t)d) return false;
        u256_from_u64(out, (uint64_t)d);
        return true;
    }
    return false;
}

static bool get_evm_addr(const cJSON *obj, const char *key, uint8_t out[20], bool *present)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    *present = false;
    memset(out, 0, 20);
    if (!v || cJSON_IsNull(v)) return true;
    if (!cJSON_IsString(v) || !evm_address_from_hex(v->valuestring, out)) return false;
    *present = true;
    return true;
}

static int hexval(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}
static bool hex_decode(const char *hex, uint8_t *out, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int hi = hexval(hex[2 * i]), lo = hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool get_str(const cJSON *obj, const char *key, char *out, size_t cap, bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    size_t n;
    if (!v) { out[0] = 0; return !required; }
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    n = strlen(v->valuestring);
    if (n + 1 > cap) return false;
    memcpy(out, v->valuestring, n + 1);
    return true;
}

static int get_pubkey_array(const cJSON *obj, const char *key, uint8_t (*out)[32], uint8_t max, uint8_t *n)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(obj, key);
    const cJSON *it;
    *n = 0;
    if (!arr) return PROTO_OK;
    if (!cJSON_IsArray(arr)) return PROTO_E_FIELD;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it)) return PROTO_E_FIELD;
        if (*n >= max) return PROTO_E_TOOBIG;
        if (!pubkey_from_b58(it->valuestring, out[*n])) return PROTO_E_ENCODING;
        (*n)++;
    }
    return PROTO_OK;
}

static int parse_claims(const cJSON *c, policy_claims_t *cl)
{
    const cJSON *arr, *it;
    char dest[64];
    int rc;
    memset(cl, 0, sizeof *cl);
    if (!c || !cJSON_IsObject(c)) return PROTO_E_FIELD;
    if (!get_u64(c, "lamports_out", &cl->lamports_out, false)) return PROTO_E_FIELD;
    if (!get_u64(c, "fee_lamports", &cl->fee_lamports, false)) return PROTO_E_FIELD;
    if (!get_str(c, "dest", dest, sizeof dest, false)) return PROTO_E_FIELD;
    if (dest[0]) {
        if (!pubkey_from_b58(dest, cl->dest)) return PROTO_E_ENCODING;
        cl->has_dest = true;
    }
    rc = get_pubkey_array(c, "programs", cl->programs, POLICY_MAX_PROGRAMS, &cl->n_programs);
    if (rc) return rc;
    arr = cJSON_GetObjectItemCaseSensitive(c, "token_out");
    if (arr) {
        if (!cJSON_IsArray(arr)) return PROTO_E_FIELD;
        cJSON_ArrayForEach(it, arr) {
            char mint[64];
            if (!cJSON_IsObject(it)) return PROTO_E_FIELD;
            if (cl->n_token_out >= POLICY_MAX_TOKEN_OUT) return PROTO_E_TOOBIG;
            if (!get_str(it, "mint", mint, sizeof mint, true)) return PROTO_E_FIELD;
            if (!pubkey_from_b58(mint, cl->token_out[cl->n_token_out].mint)) return PROTO_E_ENCODING;
            if (!get_u64(it, "amount", &cl->token_out[cl->n_token_out].amount, true)) return PROTO_E_FIELD;
            if (!get_str(it, "dest", mint, sizeof mint, false)) return PROTO_E_FIELD;
            if (mint[0]) {
                if (!pubkey_from_b58(mint, cl->token_out[cl->n_token_out].dest)) return PROTO_E_ENCODING;
                cl->token_out[cl->n_token_out].has_dest = true;
            }
            cl->n_token_out++;
        }
    }
    arr = cJSON_GetObjectItemCaseSensitive(c, "dangerous");
    if (arr) {
        if (!cJSON_IsArray(arr)) return PROTO_E_FIELD;
        cJSON_ArrayForEach(it, arr) {
            uint32_t bit;
            if (!cJSON_IsString(it)) return PROTO_E_FIELD;
            bit = solmsg_danger_from_name(it->valuestring);
            if (!bit) return PROTO_E_FIELD;
            cl->dangerous |= bit;
        }
    }
    return PROTO_OK;
}

static int parse_danger_names(const cJSON *c, uint32_t (*from_name)(const char *), uint32_t *mask)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(c, "dangerous"), *it;
    *mask = 0;
    if (!arr) return PROTO_OK;
    if (!cJSON_IsArray(arr)) return PROTO_E_FIELD;
    cJSON_ArrayForEach(it, arr) {
        uint32_t bit;
        if (!cJSON_IsString(it)) return PROTO_E_FIELD;
        bit = from_name(it->valuestring);
        if (!bit) return PROTO_E_FIELD;
        *mask |= bit;
    }
    return PROTO_OK;
}

/* {"chain_id":1,"to":"0x..","value_wei":"..","token_out":[{"token":"0x..","to":"0x..","amount":".."}],"max_fee_wei":"..",
 *  "dangerous":[..]}. A `routers` array is accepted and ignored: the allowlist lives on the controller (routers.h). */
static int parse_evm_claims(const cJSON *c, policy_evm_claims_t *cl)
{
    const cJSON *arr, *it;
    int rc;
    memset(cl, 0, sizeof *cl);
    if (!c || !cJSON_IsObject(c)) return PROTO_E_FIELD;
    if (!get_u64(c, "chain_id", &cl->chain_id, true) || cl->chain_id == 0) return PROTO_E_FIELD;
    if (!get_evm_addr(c, "to", cl->to, &cl->has_to)) return PROTO_E_ENCODING;
    if (!get_u256(c, "value_wei", &cl->value_wei, false)) return PROTO_E_FIELD;
    if (!get_u256(c, "max_fee_wei", &cl->max_fee_wei, false)) return PROTO_E_FIELD;
    arr = cJSON_GetObjectItemCaseSensitive(c, "token_out");
    if (arr) {
        if (!cJSON_IsArray(arr)) return PROTO_E_FIELD;
        cJSON_ArrayForEach(it, arr) {
            bool present;
            if (!cJSON_IsObject(it)) return PROTO_E_FIELD;
            if (cl->n_token_out >= POLICY_MAX_TOKEN_OUT) return PROTO_E_TOOBIG;
            if (!get_evm_addr(it, "token", cl->token_out[cl->n_token_out].token, &present)) return PROTO_E_ENCODING;
            if (!present) return PROTO_E_FIELD;
            if (!get_u256(it, "amount", &cl->token_out[cl->n_token_out].amount, true)) return PROTO_E_FIELD;
            if (!get_evm_addr(it, "to", cl->token_out[cl->n_token_out].to, &cl->token_out[cl->n_token_out].has_to)) return PROTO_E_ENCODING;
            cl->n_token_out++;
        }
    }
    rc = parse_danger_names(c, evmtx_danger_from_name, &cl->dangerous);
    return rc;
}

/* {"network":"mainnet","sats_out":"..","dest":"bc1q..","fee_sats":"..","inputs":2,"dangerous":[..]} */
static int parse_btc_claims(const cJSON *c, policy_btc_claims_t *cl)
{
    char net[16], dest[BTC_ADDR_MAX];
    uint64_t v;
    memset(cl, 0, sizeof *cl);
    if (!c || !cJSON_IsObject(c)) return PROTO_E_FIELD;
    if (!get_str(c, "network", net, sizeof net, true)) return PROTO_E_FIELD;
    cl->mainnet = strcmp(net, "mainnet") == 0;
    if (!get_u64(c, "sats_out", &cl->sats_out, false)) return PROTO_E_FIELD;
    if (!get_u64(c, "fee_sats", &cl->fee_sats, false)) return PROTO_E_FIELD;
    if (!get_str(c, "dest", dest, sizeof dest, false)) return PROTO_E_FIELD;
    if (dest[0]) {
        size_t n = btc_address_to_script(dest, cl->dest_spk);
        if (!n) return PROTO_E_ENCODING;
        cl->dest_spk_len = (uint8_t)n;
        cl->has_dest = true;
    }
    if (cJSON_GetObjectItemCaseSensitive(c, "inputs")) {
        if (!get_u64(c, "inputs", &v, true) || v > BTCPSBT_MAX_INPUTS) return PROTO_E_FIELD;
        cl->inputs = (uint8_t)v;
        cl->has_inputs = true;
    }
    return parse_danger_names(c, btcpsbt_danger_from_name, &cl->dangerous);
}

/* evm_msg with EIP-712: "typed_data" object + "digest" (0x + 64 hex) */
static int parse_typed_data(const cJSON *root, proto_req_t *req)
{
    const cJSON *td = cJSON_GetObjectItemCaseSensitive(root, "typed_data");
    const cJSON *dg = cJSON_GetObjectItemCaseSensitive(root, "digest");
    u256_t v;
    if (!td) return PROTO_OK;
    if (!cJSON_IsObject(td)) return PROTO_E_FIELD;
    if (!cJSON_IsString(dg) || !dg->valuestring || strlen(dg->valuestring) != 66 || !u256_from_hex(dg->valuestring, &v))
        return PROTO_E_FIELD;
    u256_to_be32(&v, req->sign.typed.claimed);
    req->sign.typed.present = true;
    req->sign.typed.rc = eip712_digest(td, req->sign.typed.digest);
    return PROTO_OK;
}

/* bytes (base64) + ttl_ms + claims (shape by kind) into req->sign. */
static int parse_sign_body(const cJSON *root, proto_req_t *req)
{
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "bytes");
    uint64_t ttl = 0;
    int n = 0, rc;
    if (req->sign.kind == PROTO_KIND_EVM_MSG) {
        rc = parse_typed_data(root, req);
        if (rc) return rc;
    }
    if (b || !req->sign.typed.present) {
        if (!cJSON_IsString(b) || !b->valuestring) return PROTO_E_FIELD;
        n = base64_decode(b->valuestring, strlen(b->valuestring), req->sign.bytes, sizeof req->sign.bytes);
        if (n < 0) return (strlen(b->valuestring) > (PROTO_MAX_BYTES * 4) / 3 + 4) ? PROTO_E_TOOBIG : PROTO_E_ENCODING;
        if (n == 0) return PROTO_E_FIELD;
        if (req->sign.typed.present) return PROTO_E_FIELD; /* either bytes or typed_data, never both */
    }
    req->sign.len = (uint16_t)n;
    if (!get_u64(root, "ttl_ms", &ttl, false)) return PROTO_E_FIELD;
    req->sign.ttl_ms = ttl > CONSENT_MAX_TTL_MS ? CONSENT_MAX_TTL_MS : (uint32_t)ttl;
    switch (req->sign.kind) {
    case PROTO_KIND_TX: return parse_claims(cJSON_GetObjectItemCaseSensitive(root, "claims"), &req->sign.claims);
    case PROTO_KIND_EVM_TX: return parse_evm_claims(cJSON_GetObjectItemCaseSensitive(root, "claims"), &req->sign.evm_claims);
    case PROTO_KIND_BTC_PSBT: return parse_btc_claims(cJSON_GetObjectItemCaseSensitive(root, "claims"), &req->sign.btc_claims);
    default: return PROTO_OK; /* messages carry no claims */
    }
}

/* {"chain":"solana"|"evm","address":"<base58>"|"0x…"} -> router entry */
static int parse_router_change(const cJSON *root, const char *key, bool *present, router_entry_t *e)
{
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, key);
    char chain[12], addr[64];
    *present = false;
    if (!o) return PROTO_OK;
    if (!cJSON_IsObject(o)) return PROTO_E_FIELD;
    if (!get_str(o, "chain", chain, sizeof chain, true) || !routers_chain_from_name(chain, &e->chain)) return PROTO_E_FIELD;
    if (!get_str(o, "address", addr, sizeof addr, true)) return PROTO_E_FIELD;
    memset(e->addr, 0, sizeof e->addr);
    if (e->chain == ROUTER_CHAIN_SOL) {
        if (!pubkey_from_b58(addr, e->addr)) return PROTO_E_ENCODING;
    } else {
        if (!evm_address_from_hex(addr, e->addr)) return PROTO_E_ENCODING;
    }
    *present = true;
    return PROTO_OK;
}

int proto_parse_request(const char *line, proto_req_t *req)
{
    cJSON *root;
    const cJSON *t;
    int rc = PROTO_OK;
    memset(req, 0, sizeof *req);
    root = cJSON_ParseWithLength(line, strlen(line));
    if (!root) return PROTO_E_JSON;
    if (!cJSON_IsObject(root)) { rc = PROTO_E_JSON; goto out; }
    t = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(t)) { rc = PROTO_E_TYPE; goto out; }
    if (!get_str(root, "id", req->id, sizeof req->id, false)) { rc = PROTO_E_FIELD; goto out; }

    if (strcmp(t->valuestring, "hello") == 0) {
        req->type = PROTO_REQ_HELLO;
    } else if (strcmp(t->valuestring, "time") == 0) {
        req->type = PROTO_REQ_TIME;
        if (!get_u64(root, "unix", &req->unix_time, true)) rc = PROTO_E_FIELD;
    } else if (strcmp(t->valuestring, "init") == 0) {
        req->type = PROTO_REQ_INIT;
    } else if (strcmp(t->valuestring, "sign") == 0) {
        char kind[12];
        req->type = PROTO_REQ_SIGN;
        if (!get_str(root, "kind", kind, sizeof kind, true)) { rc = PROTO_E_FIELD; goto out; }
        if (strcmp(kind, "msg") == 0) { req->sign.kind = PROTO_KIND_MSG; req->sign.is_msg = true; }
        else if (strcmp(kind, "tx") == 0) req->sign.kind = PROTO_KIND_TX;
        else if (strcmp(kind, "evm_tx") == 0) req->sign.kind = PROTO_KIND_EVM_TX;
        else if (strcmp(kind, "btc_psbt") == 0) req->sign.kind = PROTO_KIND_BTC_PSBT;
        else if (strcmp(kind, "evm_msg") == 0) req->sign.kind = PROTO_KIND_EVM_MSG;
        else if (strcmp(kind, "btc_msg") == 0) req->sign.kind = PROTO_KIND_BTC_MSG;
        else { rc = PROTO_E_FIELD; goto out; }
        rc = parse_sign_body(root, req);
    } else if (strcmp(t->valuestring, "inheritance_set") == 0) {
        const cJSON *arr, *it;
        uint64_t v;
        char heir[64];
        req->type = PROTO_REQ_INHERITANCE_SET;
        req->sign.kind = PROTO_KIND_TX; /* Solana only for now */
        rc = parse_sign_body(root, req); /* bytes + claims, always a tx */
        if (rc) goto out;
        if (!get_u64(root, "inactivity_days", &v, true) || v == 0 || v > 0xffff) { rc = PROTO_E_FIELD; goto out; }
        req->inheritance.inactivity_days = (uint16_t)v;
        arr = cJSON_GetObjectItemCaseSensitive(root, "warn_days");
        if (arr) {
            if (!cJSON_IsArray(arr)) { rc = PROTO_E_FIELD; goto out; }
            cJSON_ArrayForEach(it, arr) {
                double d;
                if (!cJSON_IsNumber(it)) { rc = PROTO_E_FIELD; goto out; }
                d = it->valuedouble;
                if (d < 1 || d > 0xffff || d != (double)(uint16_t)d) { rc = PROTO_E_FIELD; goto out; }
                if (req->inheritance.n_warn >= CONSENT_INH_MAX_WARN) { rc = PROTO_E_TOOBIG; goto out; }
                req->inheritance.warn_days[req->inheritance.n_warn++] = (uint16_t)d;
            }
        }
        if (!get_str(root, "heir", heir, sizeof heir, true)) { rc = PROTO_E_FIELD; goto out; }
        if (!pubkey_from_b58(heir, req->inheritance.heir)) { rc = PROTO_E_ENCODING; goto out; }
    } else if (strcmp(t->valuestring, "inheritance_status") == 0) {
        req->type = PROTO_REQ_INHERITANCE_STATUS;
    } else if (strcmp(t->valuestring, "inheritance_release") == 0) {
        req->type = PROTO_REQ_INHERITANCE_RELEASE;
    } else if (strcmp(t->valuestring, "inheritance_clear") == 0) {
        req->type = PROTO_REQ_INHERITANCE_CLEAR;
    } else if (strcmp(t->valuestring, "elevate") == 0) {
        /* exactly one of limit_lamports | limit_wei | limit_sats */
        int given = 0;
        req->type = PROTO_REQ_ELEVATE;
        if (cJSON_GetObjectItemCaseSensitive(root, "limit_lamports")) {
            given++;
            req->elevate_chain = CONSENT_CHAIN_SOL;
            if (!get_u64(root, "limit_lamports", &req->limit_lamports, true)) rc = PROTO_E_FIELD;
        }
        if (cJSON_GetObjectItemCaseSensitive(root, "limit_sats")) {
            given++;
            req->elevate_chain = CONSENT_CHAIN_BTC;
            if (!get_u64(root, "limit_sats", &req->limit_lamports, true)) rc = PROTO_E_FIELD;
        }
        if (cJSON_GetObjectItemCaseSensitive(root, "limit_wei")) {
            given++;
            req->elevate_chain = CONSENT_CHAIN_EVM;
            if (!get_u256(root, "limit_wei", &req->limit_wei, true)) rc = PROTO_E_FIELD;
        }
        if (given != 1) rc = PROTO_E_FIELD;
    } else if (strcmp(t->valuestring, "lock") == 0) {
        req->type = PROTO_REQ_LOCK;
    } else if (strcmp(t->valuestring, "unlock") == 0) {
        req->type = PROTO_REQ_UNLOCK;
    } else if (strcmp(t->valuestring, "update_begin") == 0) {
        char hex[65];
        uint64_t size;
        req->type = PROTO_REQ_UPDATE_BEGIN;
        if (!get_u64(root, "size", &size, true) || size == 0 || size > 0xffffffffu) { rc = PROTO_E_FIELD; goto out; }
        req->update.size = (uint32_t)size;
        if (!get_str(root, "sha256", hex, sizeof hex, true) || strlen(hex) != 64 || !hex_decode(hex, req->update.sha256, 32)) { rc = PROTO_E_FIELD; goto out; }
        if (!get_str(root, "version", req->update.version, sizeof req->update.version, true)) rc = PROTO_E_FIELD;
    } else if (strcmp(t->valuestring, "update_chunk") == 0) {
        uint64_t off;
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
        int n;
        req->type = PROTO_REQ_UPDATE_CHUNK;
        if (!get_u64(root, "off", &off, true) || off > 0xffffffffu) { rc = PROTO_E_FIELD; goto out; }
        req->update.off = (uint32_t)off;
        if (!cJSON_IsString(d)) { rc = PROTO_E_FIELD; goto out; }
        n = base64_decode(d->valuestring, strlen(d->valuestring), req->update.data, sizeof req->update.data);
        if (n <= 0) { rc = PROTO_E_FIELD; goto out; }
        req->update.len = (uint16_t)n;
    } else if (strcmp(t->valuestring, "update_end") == 0) {
        req->type = PROTO_REQ_UPDATE_END;
    } else if (strcmp(t->valuestring, "update_apply") == 0) {
        req->type = PROTO_REQ_UPDATE_APPLY;
    } else if (strcmp(t->valuestring, "update_commit") == 0) {
        req->type = PROTO_REQ_UPDATE_COMMIT;
    } else if (strcmp(t->valuestring, "update_abort") == 0) {
        req->type = PROTO_REQ_UPDATE_ABORT;
    } else if (strcmp(t->valuestring, "policy") == 0) {
        /* any subset of the three limits, at least one */
        req->type = PROTO_REQ_POLICY;
        if (cJSON_GetObjectItemCaseSensitive(root, "daily_limit_lamports")) {
            req->policy.has_sol = true;
            if (!get_u64(root, "daily_limit_lamports", &req->policy.sol, true)) rc = PROTO_E_FIELD;
            req->limit_lamports = req->policy.sol;
        }
        if (cJSON_GetObjectItemCaseSensitive(root, "daily_limit_wei")) {
            req->policy.has_wei = true;
            if (!get_u256(root, "daily_limit_wei", &req->policy.wei, true)) rc = PROTO_E_FIELD;
        }
        if (cJSON_GetObjectItemCaseSensitive(root, "daily_limit_sats")) {
            req->policy.has_sats = true;
            if (!get_u64(root, "daily_limit_sats", &req->policy.sats, true)) rc = PROTO_E_FIELD;
        }
        rc = rc ? rc : parse_router_change(root, "add_router", &req->policy.has_router_add, &req->policy.router);
        rc = rc ? rc : parse_router_change(root, "remove_router", &req->policy.has_router_remove, &req->policy.router);
        if (rc == PROTO_OK && req->policy.has_router_add && req->policy.has_router_remove) rc = PROTO_E_FIELD;
        if (rc == PROTO_OK && !req->policy.has_sol && !req->policy.has_wei && !req->policy.has_sats &&
            !req->policy.has_router_add && !req->policy.has_router_remove)
            rc = PROTO_E_FIELD;
    } else if (strcmp(t->valuestring, "reveal_seed") == 0) {
        req->type = PROTO_REQ_REVEAL_SEED;
    } else if (strcmp(t->valuestring, "set_duress_pin") == 0) {
        req->type = PROTO_REQ_SET_DURESS_PIN;
    } else if (strcmp(t->valuestring, "decoy_addresses") == 0) {
        req->type = PROTO_REQ_DECOY_ADDRESSES;
    } else if (strcmp(t->valuestring, "duress") == 0) {
        req->type = PROTO_REQ_DURESS;
    } else if (strcmp(t->valuestring, "set_pin") == 0) {
        req->type = PROTO_REQ_SET_PIN;
    } else if (strcmp(t->valuestring, "allowance") == 0) {
        const cJSON *rv = cJSON_GetObjectItemCaseSensitive(root, "revoke");
        req->type = PROTO_REQ_ALLOWANCE;
        if (rv && cJSON_IsTrue(rv)) {
            req->allowance.revoke = true;
        } else {
            consent_allowance_t *a = &req->allowance.a;
            if (!get_u64(root, "lamports", &a->lamports, true) ||
                !get_u64(root, "max_per_tx_lamports", &a->max_per_tx, true) ||
                !get_u64(root, "until_unix", &a->until_unix, true)) { rc = PROTO_E_FIELD; goto out; }
            rc = get_pubkey_array(root, "programs", a->programs, CONSENT_ALLOWANCE_MAX_PROGRAMS, &a->n_programs);
            if (rc == PROTO_OK && a->n_programs == 0) rc = PROTO_E_FIELD;
        }
    } else {
        rc = PROTO_E_TYPE;
    }
out:
    cJSON_Delete(root);
    return rc;
}

const char *proto_error_str(int err)
{
    switch (err) {
    case PROTO_OK: return "ok";
    case PROTO_E_JSON: return "bad_json";
    case PROTO_E_TYPE: return "unknown_type";
    case PROTO_E_FIELD: return "bad_field";
    case PROTO_E_ENCODING: return "bad_encoding";
    case PROTO_E_TOOBIG: return "too_big";
    default: return "error";
    }
}

/* ---- serialisation ---- */

static int finish(cJSON *o, char *out, size_t cap)
{
    int rc = -1;
    if (o && cJSON_PrintPreallocated(o, out, (int)cap - 1, 0)) {
        size_t n = strlen(out);
        if (n + 2 <= cap) {
            out[n] = '\n';
            out[n + 1] = 0;
            rc = (int)n + 1;
        }
    }
    if (rc < 0 && cap) out[0] = 0;
    cJSON_Delete(o);
    return rc;
}

static void add_u64(cJSON *o, const char *k, uint64_t v)
{
    char s[21];
    u64_to_dec(v, s, sizeof s);
    cJSON_AddStringToObject(o, k, s);
}

static void add_b58(cJSON *o, const char *k, const uint8_t key[32])
{
    char s[48];
    pubkey_to_b58(key, s, sizeof s);
    cJSON_AddStringToObject(o, k, s);
}

static void add_u256(cJSON *o, const char *k, const u256_t *v)
{
    char s[U256_DEC_MAX];
    if (v) u256_to_dec(v, s, sizeof s);
    else { s[0] = '0'; s[1] = 0; }
    cJSON_AddStringToObject(o, k, s);
}

static void add_evm_addr(cJSON *o, const char *k, const uint8_t addr[20])
{
    char s[EVM_ADDR_HEX_LEN];
    evm_address_to_hex(addr, s);
    cJSON_AddStringToObject(o, k, s);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}

/* tenths of a day as an exact decimal number (124 -> 12.4), not a double that prints as 12.400000000000001 */
static void add_tenths(cJSON *o, const char *k, uint32_t tenths)
{
    char s[16];
    snprintf(s, sizeof s, "%lu.%u", (unsigned long)(tenths / 10u), (unsigned)(tenths % 10u));
    cJSON_AddRawToObject(o, k, s);
}

static void add_inheritance_summary(cJSON *o, bool armed, uint16_t inactivity_days, uint32_t since_tenths)
{
    cJSON *h = cJSON_AddObjectToObject(o, "inheritance");
    cJSON_AddBoolToObject(h, "armed", armed);
    cJSON_AddNumberToObject(h, "inactivity_days", (double)inactivity_days);
    add_tenths(h, "days_since_touch", since_tenths);
}

static void add_router_entry(cJSON *arr, const router_entry_t *e)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "chain", routers_chain_name(e->chain));
    if (e->chain == ROUTER_CHAIN_SOL) add_b58(o, "address", e->addr);
    else add_evm_addr(o, "address", e->addr);
    cJSON_AddItemToArray(arr, o);
}

/* "addresses":{"solana","evm","bitcoin"} for hello and init */
static void add_addresses_named(cJSON *o, const char *key, const uint8_t pubkey[32], const uint8_t *evm_addr, const uint8_t *btc_pkh)
{
    cJSON *a = cJSON_AddObjectToObject(o, key);
    add_b58(a, "solana", pubkey);
    if (evm_addr && !all_zero(evm_addr, 20)) add_evm_addr(a, "evm", evm_addr); else cJSON_AddNullToObject(a, "evm");
    if (btc_pkh && !all_zero(btc_pkh, 20)) {
        char s[BTC_ADDR_MAX];
        if (btc_p2wpkh_address(btc_pkh, s)) cJSON_AddStringToObject(a, "bitcoin", s); else cJSON_AddNullToObject(a, "bitcoin");
    } else {
        cJSON_AddNullToObject(a, "bitcoin");
    }
}
static void add_addresses(cJSON *o, const uint8_t pubkey[32], const uint8_t *evm_addr, const uint8_t *btc_pkh)
{
    add_addresses_named(o, "addresses", pubkey, evm_addr, btc_pkh);
}

static cJSON *begin(const char *t, const char *id)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddStringToObject(o, "t", t);
    if (id && id[0]) cJSON_AddStringToObject(o, "id", id);
    return o;
}

int proto_fmt_hello(char *out, size_t cap, const proto_hello_t *h)
{
    cJSON *o = begin("hello", NULL), *pol;
    if (!o) return -1;
    cJSON_AddStringToObject(o, "fw", h->fw);
    cJSON_AddBoolToObject(o, "se050", h->se050);
    cJSON_AddBoolToObject(o, "initialized", h->initialized);
    if (h->pubkey) add_b58(o, "pubkey", h->pubkey); else cJSON_AddNullToObject(o, "pubkey");
    if (h->pubkey) add_addresses(o, h->pubkey, h->evm_addr, h->btc_pkh);
    else cJSON_AddNullToObject(o, "addresses");
    cJSON_AddBoolToObject(o, "locked", h->locked);
    pol = cJSON_AddObjectToObject(o, "policy");
    add_u64(pol, "daily_limit_lamports", h->daily_limit);
    add_u64(pol, "spent_today_lamports", h->spent_today);
    cJSON_AddNumberToObject(pol, "day", (double)h->day);
    add_u256(pol, "daily_limit_wei", h->daily_limit_wei);
    add_u256(pol, "spent_today_wei", h->spent_today_wei);
    add_u64(pol, "daily_limit_sats", h->daily_limit_sats);
    add_u64(pol, "spent_today_sats", h->spent_today_sats);
    if (h->allowance && h->allowance->active) {
        cJSON *a = cJSON_AddObjectToObject(o, "allowance"), *arr;
        uint8_t i;
        add_u64(a, "lamports", h->allowance->lamports);
        add_u64(a, "spent_lamports", h->allowance->spent);
        add_u64(a, "max_per_tx_lamports", h->allowance->max_per_tx);
        cJSON_AddNumberToObject(a, "until_unix", (double)h->allowance->until_unix);
        arr = cJSON_AddArrayToObject(a, "programs");
        for (i = 0; i < h->allowance->n_programs; i++) {
            char s[48];
            pubkey_to_b58(h->allowance->programs[i], s, sizeof s);
            cJSON_AddItemToArray(arr, cJSON_CreateString(s));
        }
    } else {
        cJSON_AddNullToObject(o, "allowance");
    }
    cJSON_AddBoolToObject(o, "pin_set", h->pin_set);
    cJSON_AddBoolToObject(o, "duress_pin_set", h->duress_pin_set);
    cJSON_AddNumberToObject(o, "pin_failures", (double)h->pin_failures);
    if (h->dev_keys) cJSON_AddBoolToObject(o, "dev_keys", true);
    add_inheritance_summary(o, h->inh_armed, h->inh_inactivity_days, h->inh_days_since_touch_tenths);
    cJSON_AddBoolToObject(o, "display", h->display);
    {
        cJSON *r = cJSON_AddObjectToObject(o, "routers"), *arr;
        size_t n, i;
        const router_entry_t *b = routers_builtin(&n);
        arr = cJSON_AddArrayToObject(r, "builtin");
        for (i = 0; i < n; i++) add_router_entry(arr, &b[i]);
        arr = cJSON_AddArrayToObject(r, "owner");
        for (i = 0; h->routers && i < h->routers->n && i < ROUTERS_MAX_OWNER; i++) add_router_entry(arr, &h->routers->e[i]);
    }
        {
        cJSON *up = cJSON_AddObjectToObject(o, "update");
        cJSON_AddNumberToObject(up, "partition", h->partition);
        cJSON_AddBoolToObject(up, "staged", h->update_staged);
        cJSON_AddBoolToObject(up, "trial", h->trial);
    }
    return finish(o, out, cap);
}

int proto_fmt_pending_confirm(char *out, size_t cap, const char *id)
{
    cJSON *o = begin("pending", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "need", "pin");
    cJSON_AddBoolToObject(o, "confirm", true);
    return finish(o, out, cap);
}

int proto_fmt_pending_sol(char *out, size_t cap, const char *id, const char *need, const solmsg_summary_t *parsed, uint32_t dangerous)
{
    cJSON *o = begin("pending", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "need", need);
    if (parsed) {
        cJSON *p = cJSON_AddObjectToObject(o, "parsed");
        add_u64(p, "lamports_out", parsed->lamports_out);
        if (parsed->has_dest) add_b58(p, "dest", parsed->dest);
        if (parsed->n_token_out) {
            cJSON *arr = cJSON_AddArrayToObject(p, "token_out");
            uint8_t i;
            for (i = 0; i < parsed->n_token_out; i++) {
                cJSON *t = cJSON_CreateObject();
                if (parsed->token_out[i].has_mint) add_b58(t, "mint", parsed->token_out[i].mint);
                if (parsed->token_out[i].has_dest) add_b58(t, "dest", parsed->token_out[i].dest);
                add_u64(t, "amount", parsed->token_out[i].amount);
                cJSON_AddItemToArray(arr, t);
            }
        }
        if (dangerous) {
            cJSON *arr = cJSON_AddArrayToObject(p, "dangerous");
            uint32_t bit;
            for (bit = 1; bit <= SOLMSG_DANGER_ALL; bit <<= 1)
                if (dangerous & bit) cJSON_AddItemToArray(arr, cJSON_CreateString(solmsg_danger_name(bit)));
        }
    }
    return finish(o, out, cap);
}

int proto_fmt_pending(char *out, size_t cap, const char *id, const char *need, const solmsg_summary_t *parsed)
{
    return proto_fmt_pending_sol(out, cap, id, need, parsed, parsed ? parsed->dangerous : 0);
}

int proto_fmt_signed(char *out, size_t cap, const char *id, const uint8_t sig[64])
{
    char b64[92];
    cJSON *o = begin("signed", id);
    if (!o) return -1;
    base64_encode(sig, 64, b64, sizeof b64);
    cJSON_AddStringToObject(o, "sig", b64);
    return finish(o, out, cap);
}

int proto_fmt_signed_bytes(char *out, size_t cap, const char *id, const uint8_t *sig, size_t len)
{
    char b64[(BTCPSBT_PARTIAL_SIG_MAX / 3 + 2) * 4 + 1];
    cJSON *o;
    if (base64_encode(sig, len, b64, sizeof b64) < 0) return -1;
    o = begin("signed", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "sig", b64);
    return finish(o, out, cap);
}

int proto_fmt_signed_psbt(char *out, size_t cap, const char *id, const uint8_t *psbt, size_t len)
{
    static char b64[(BTCPSBT_MAX_OUT_BYTES / 3 + 2) * 4 + 1]; /* ~6.7 KB: kept static, not on the stack */
    cJSON *o;
    if (base64_encode(psbt, len, b64, sizeof b64) < 0) return -1;
    o = begin("signed", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "psbt", b64);
    return finish(o, out, cap);
}

static void add_danger_names(cJSON *p, uint32_t dangerous, uint32_t all, const char *(*name)(uint32_t))
{
    cJSON *arr;
    uint32_t bit;
    if (!dangerous) return;
    arr = cJSON_AddArrayToObject(p, "dangerous");
    for (bit = 1; bit <= all; bit <<= 1)
        if (dangerous & bit) cJSON_AddItemToArray(arr, cJSON_CreateString(name(bit)));
}

int proto_fmt_pending_evm(char *out, size_t cap, const char *id, const char *need, const evmtx_t *tx, uint32_t dangerous)
{
    cJSON *o = begin("pending", id), *p;
    static const char *CALLS[] = {"transfer_native", "erc20_transfer", "approve", "erc20_transfer_from", "set_approval_for_all", "contract_call", "contract_create"};
    if (!o) return -1;
    cJSON_AddStringToObject(o, "need", need);
    p = cJSON_AddObjectToObject(o, "parsed");
    cJSON_AddNumberToObject(p, "chain_id", (double)tx->chain_id);
    cJSON_AddNumberToObject(p, "nonce", (double)tx->nonce);
    if (tx->has_to) add_evm_addr(p, "to", tx->to); else cJSON_AddNullToObject(p, "to");
    add_u256(p, "value_wei", &tx->value);
    add_u256(p, "max_fee_wei", &tx->max_fee_total);
    cJSON_AddStringToObject(p, "call", CALLS[tx->call <= EVMTX_CALL_CREATE ? tx->call : EVMTX_CALL_UNKNOWN]);
    if (tx->call == EVMTX_CALL_TRANSFER || tx->call == EVMTX_CALL_TRANSFER_FROM) {
        cJSON *arr = cJSON_AddArrayToObject(p, "token_out"), *t = cJSON_CreateObject();
        add_evm_addr(t, "token", tx->to);
        add_evm_addr(t, "to", tx->call_to);
        if (tx->call == EVMTX_CALL_TRANSFER_FROM) add_evm_addr(t, "from", tx->call_from);
        add_u256(t, "amount", &tx->call_amount);
        cJSON_AddItemToArray(arr, t);
    } else if (tx->call == EVMTX_CALL_APPROVE || tx->call == EVMTX_CALL_SET_APPROVAL) {
        add_evm_addr(p, "spender", tx->call_to);
        if (tx->call == EVMTX_CALL_APPROVE) add_u256(p, "allowance", &tx->call_amount);
    }
    add_danger_names(p, dangerous, EVMTX_DANGER_ALL, evmtx_danger_name);
    return finish(o, out, cap);
}

int proto_fmt_pending_btc(char *out, size_t cap, const char *id, const char *need, const btcpsbt_t *b, uint32_t dangerous)
{
    cJSON *o = begin("pending", id), *p;
    char addr[BTC_ADDR_MAX];
    if (!o) return -1;
    cJSON_AddStringToObject(o, "need", need);
    p = cJSON_AddObjectToObject(o, "parsed");
    add_u64(p, "sats_out", b->sats_out);
    add_u64(p, "fee_sats", b->fee);
    add_u64(p, "change_sats", b->sats_change);
    if (b->has_dest && btc_script_to_address(b->dest_spk, b->dest_spk_len, addr)) cJSON_AddStringToObject(p, "dest", addr);
    else cJSON_AddNullToObject(p, "dest");
    cJSON_AddNumberToObject(p, "inputs", (double)b->n_owner_inputs);
    cJSON_AddNumberToObject(p, "outputs", (double)b->n_out);
    add_danger_names(p, dangerous, BTCPSBT_DANGER_ALL, btcpsbt_danger_name);
    return finish(o, out, cap);
}

int proto_fmt_denied(char *out, size_t cap, const char *id, const char *reason)
{
    cJSON *o = begin("denied", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "reason", reason);
    return finish(o, out, cap);
}

int proto_fmt_init(char *out, size_t cap, const char *id, const proto_addrs_t *real, const proto_addrs_t *decoy)
{
    cJSON *o = begin("init", id);
    if (!o) return -1;
    add_b58(o, "pubkey", real->pubkey);
    add_addresses(o, real->pubkey, real->evm_addr, real->btc_pkh);
    if (decoy && decoy->pubkey) add_addresses_named(o, "decoy_addresses", decoy->pubkey, decoy->evm_addr, decoy->btc_pkh);
    else cJSON_AddNullToObject(o, "decoy_addresses");
    return finish(o, out, cap);
}

int proto_fmt_decoy_addresses(char *out, size_t cap, const char *id, const proto_addrs_t *decoy)
{
    cJSON *o = begin("decoy_addresses", id);
    if (!o) return -1;
    add_addresses(o, decoy->pubkey, decoy->evm_addr, decoy->btc_pkh);
    return finish(o, out, cap);
}

int proto_fmt_elevated(char *out, size_t cap, const char *id, uint32_t until_ms)
{
    cJSON *o = begin("elevated", id);
    if (!o) return -1;
    cJSON_AddNumberToObject(o, "until_ms", (double)until_ms);
    return finish(o, out, cap);
}

int proto_fmt_update_begin(char *out, size_t cap, const char *id, uint32_t chunk, uint32_t partition_size)
{
    cJSON *o = begin("update_begin", id);
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "chunk", chunk);
    cJSON_AddNumberToObject(o, "partition_size", partition_size);
    return finish(o, out, cap);
}

int proto_fmt_update_commit(char *out, size_t cap, const char *id, bool was_trial, bool bought)
{
    cJSON *o = begin("update_commit", id);
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "was_trial", was_trial);
    cJSON_AddBoolToObject(o, "bought", bought);
    return finish(o, out, cap);
}

int proto_fmt_ok(char *out, size_t cap, const char *t, const char *id)
{
    cJSON *o = begin(t, id);
    if (!o) return -1;
    cJSON_AddBoolToObject(o, "ok", true);
    return finish(o, out, cap);
}

int proto_fmt_simple(char *out, size_t cap, const char *t, const char *id)
{
    cJSON *o = begin(t, id);
    if (!o) return -1;
    return finish(o, out, cap);
}

int proto_fmt_event(char *out, size_t cap, const char *ev, const char *what)
{
    cJSON *o = begin("event", NULL);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "ev", ev);
    if (what) cJSON_AddStringToObject(o, "what", what);
    return finish(o, out, cap);
}

int proto_fmt_event_days_left(char *out, size_t cap, uint32_t days_left)
{
    cJSON *o = begin("event", NULL);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "ev", "inheritance_warning");
    cJSON_AddNumberToObject(o, "days_left", (double)days_left);
    return finish(o, out, cap);
}

int proto_fmt_event_display(char *out, size_t cap, uint8_t page, uint8_t pages)
{
    cJSON *o = begin("event", NULL);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "ev", "display");
    cJSON_AddNumberToObject(o, "page", (double)page);
    cJSON_AddNumberToObject(o, "pages", (double)pages);
    return finish(o, out, cap);
}

int proto_fmt_error(char *out, size_t cap, const char *id, const char *error)
{
    cJSON *o = begin("error", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "error", error);
    return finish(o, out, cap);
}

int proto_fmt_inheritance_set(char *out, size_t cap, const char *id, const uint8_t sig[64])
{
    char b64[92];
    cJSON *o = begin("inheritance_set", id);
    if (!o) return -1;
    cJSON_AddBoolToObject(o, "ok", true);
    base64_encode(sig, 64, b64, sizeof b64);
    cJSON_AddStringToObject(o, "sig", b64);
    return finish(o, out, cap);
}

int proto_fmt_inheritance_status(char *out, size_t cap, const char *id, const proto_inh_status_t *st)
{
    const consent_inheritance_t *h = st->inh;
    cJSON *o = begin("inheritance_status", id), *arr;
    uint8_t i;
    if (!o) return -1;
    cJSON_AddBoolToObject(o, "armed", h->armed);
    cJSON_AddNumberToObject(o, "inactivity_days", (double)(h->armed ? h->inactivity_days : 0));
    add_tenths(o, "days_since_touch", st->days_since_touch_tenths);
    add_tenths(o, "powered_days", st->powered_days_tenths);
    if (h->armed) add_b58(o, "heir", h->heir); else cJSON_AddNullToObject(o, "heir");
    arr = cJSON_AddArrayToObject(o, "warn_days");
    for (i = 0; h->armed && i < h->n_warn && i < CONSENT_INH_MAX_WARN; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)h->warn_days[i]));
    arr = cJSON_AddArrayToObject(o, "warned");
    for (i = 0; h->armed && i < h->n_warn && i < CONSENT_INH_MAX_WARN; i++)
        if (h->warned & (1u << i)) cJSON_AddItemToArray(arr, cJSON_CreateNumber((double)h->warn_days[i]));
    return finish(o, out, cap);
}

int proto_fmt_inheritance_release(char *out, size_t cap, const char *id, const uint8_t *tx, size_t len)
{
    char b64[((1u + 64u * CONSENT_INH_MAX_SIGNERS + CONSENT_INH_MSG_MAX) / 3 + 2) * 4 + 1];
    cJSON *o;
    if (base64_encode(tx, len, b64, sizeof b64) < 0) return -1;
    o = begin("inheritance_release", id);
    if (!o) return -1;
    cJSON_AddStringToObject(o, "tx", b64);
    return finish(o, out, cap);
}
