#include "eip712.h"

#include <stdio.h>
#include <string.h>

#include "chainaddr.h"
#include "keccak.h"
#include "u256.h"

typedef enum { T_ADDRESS, T_BOOL, T_UINT, T_INT, T_BYTESN, T_BYTES, T_STRING } ptype_t;

/* "uint256" -> T_UINT bits 256; false when not a supported primitive. */
static bool parse_type(const char *t, ptype_t *kind, unsigned *bits)
{
    size_t n = strlen(t);
    *bits = 0;
    if (strchr(t, '[')) return false; /* arrays */
    if (strcmp(t, "address") == 0) { *kind = T_ADDRESS; return true; }
    if (strcmp(t, "bool") == 0) { *kind = T_BOOL; return true; }
    if (strcmp(t, "bytes") == 0) { *kind = T_BYTES; return true; }
    if (strcmp(t, "string") == 0) { *kind = T_STRING; return true; }
    if (n > 4 && strncmp(t, "uint", 4) == 0) { *kind = T_UINT; t += 4; }
    else if (n > 3 && strncmp(t, "int", 3) == 0) { *kind = T_INT; t += 3; }
    else if (n > 5 && strncmp(t, "bytes", 5) == 0) { *kind = T_BYTESN; t += 5; }
    else return false;
    {
        unsigned v = 0;
        if (!*t || strlen(t) > 3) return false;
        for (; *t; t++) {
            if (*t < '0' || *t > '9') return false;
            v = v * 10 + (unsigned)(*t - '0');
        }
        if (*kind == T_BYTESN) {
            if (v < 1 || v > 32) return false;
        } else if (v < 8 || v > 256 || v % 8) {
            return false;
        }
        *bits = v;
    }
    return true;
}

static bool hex_bytes(const char *s, uint8_t *out, size_t cap, size_t *len)
{
    size_t n, i;
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X')) return false;
    s += 2;
    n = strlen(s);
    if (n % 2 || n / 2 > cap) return false;
    for (i = 0; i < n; i++) {
        int v;
        char c = s[i];
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(v << 4);
        else out[i / 2] |= (uint8_t)v;
    }
    *len = n / 2;
    return true;
}

/* JSON number / decimal string / hex string -> u256 (with `neg` for a leading '-'). */
static bool json_int(const cJSON *v, u256_t *out, bool *neg)
{
    *neg = false;
    if (cJSON_IsNumber(v)) {
        double d = v->valuedouble;
        bool n = d < 0;
        if (n) d = -d;
        if (d > 9007199254740992.0 || d != (double)(uint64_t)d) return false;
        u256_from_u64(out, (uint64_t)d);
        *neg = n && !u256_is_zero(out);
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring) {
        const char *s = v->valuestring;
        if (s[0] == '-') { *neg = true; s++; }
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return !*neg && u256_from_hex(s, out);
        if (!u256_from_dec(s, out)) return false;
        if (*neg && u256_is_zero(out)) *neg = false;
        return true;
    }
    return false;
}

/* fits in N bits as unsigned / as two's complement signed */
static bool fits_bits(const u256_t *v, unsigned bits, bool is_signed, bool neg)
{
    unsigned limit = is_signed ? bits - 1 : bits; /* magnitude bound: 2^limit (signed negative may equal 2^(bits-1)) */
    u256_t bound;
    int i;
    if (limit >= 256) return true;
    u256_zero(&bound);
    bound.w[limit / 64] = 1ull << (limit % 64);
    for (i = 0; i < 4; i++) (void)i;
    if (is_signed && neg) return u256_cmp(v, &bound) <= 0;
    return u256_cmp(v, &bound) < 0;
}

static int encode_value(ptype_t kind, unsigned bits, const cJSON *v, uint8_t word[32])
{
    memset(word, 0, 32);
    switch (kind) {
    case T_ADDRESS:
        if (!cJSON_IsString(v) || !evm_address_from_hex(v->valuestring, word + 12)) return EIP712_E_MALFORMED;
        return EIP712_OK;
    case T_BOOL:
        if (!cJSON_IsBool(v)) return EIP712_E_MALFORMED;
        word[31] = cJSON_IsTrue(v) ? 1 : 0;
        return EIP712_OK;
    case T_UINT:
    case T_INT: {
        u256_t x;
        bool neg;
        if (!json_int(v, &x, &neg)) return EIP712_E_MALFORMED;
        if (kind == T_UINT && neg) return EIP712_E_MALFORMED;
        if (!fits_bits(&x, bits, kind == T_INT, neg)) return EIP712_E_MALFORMED;
        if (neg) u256_neg(&x);
        u256_to_be32(&x, word);
        return EIP712_OK;
    }
    case T_BYTESN: {
        size_t n;
        if (!cJSON_IsString(v) || !hex_bytes(v->valuestring, word, 32, &n) || n != bits) return EIP712_E_MALFORMED;
        return EIP712_OK;
    }
    case T_BYTES: {
        /* dynamic bytes: hash of the content; bounded by a stack buffer */
        uint8_t buf[512];
        size_t n;
        if (!cJSON_IsString(v) || !hex_bytes(v->valuestring, buf, sizeof buf, &n)) return EIP712_E_MALFORMED;
        keccak256(buf, n, word);
        return EIP712_OK;
    }
    case T_STRING:
        if (!cJSON_IsString(v) || !v->valuestring) return EIP712_E_MALFORMED;
        keccak256((const uint8_t *)v->valuestring, strlen(v->valuestring), word);
        return EIP712_OK;
    }
    return EIP712_E_UNSUPPORTED;
}

int eip712_hash_struct(const cJSON *types, const char *type_name, const cJSON *data, uint8_t out[32])
{
    const cJSON *fields, *f;
    char ts[EIP712_MAX_TYPE_STR];
    size_t tl = 0, nl;
    uint8_t words[EIP712_MAX_FIELDS][32];
    int n = 0, i, rc;
    keccak_ctx c;
    if (!cJSON_IsObject(types) || !type_name || !cJSON_IsObject(data)) return EIP712_E_MALFORMED;
    fields = cJSON_GetObjectItemCaseSensitive(types, type_name);
    if (!cJSON_IsArray(fields)) return EIP712_E_MALFORMED;
    nl = strlen(type_name);
    if (nl + 2 > sizeof ts) return EIP712_E_UNSUPPORTED;
    memcpy(ts, type_name, nl);
    tl = nl;
    ts[tl++] = '(';
    cJSON_ArrayForEach(f, fields) {
        const cJSON *fname = cJSON_GetObjectItemCaseSensitive(f, "name");
        const cJSON *ftype = cJSON_GetObjectItemCaseSensitive(f, "type");
        const cJSON *val;
        ptype_t kind;
        unsigned bits;
        size_t l1, l2;
        if (!cJSON_IsObject(f) || !cJSON_IsString(fname) || !cJSON_IsString(ftype) || !fname->valuestring[0]) return EIP712_E_MALFORMED;
        if (n >= EIP712_MAX_FIELDS) return EIP712_E_UNSUPPORTED;
        if (!parse_type(ftype->valuestring, &kind, &bits)) return EIP712_E_UNSUPPORTED;
        l1 = strlen(ftype->valuestring);
        l2 = strlen(fname->valuestring);
        if (tl + l1 + 1 + l2 + 2 > sizeof ts) return EIP712_E_UNSUPPORTED;
        if (n) ts[tl++] = ',';
        memcpy(ts + tl, ftype->valuestring, l1); tl += l1;
        ts[tl++] = ' ';
        memcpy(ts + tl, fname->valuestring, l2); tl += l2;
        val = cJSON_GetObjectItemCaseSensitive(data, fname->valuestring);
        if (!val) return EIP712_E_MALFORMED;
        rc = encode_value(kind, bits, val, words[n]);
        if (rc) return rc;
        n++;
    }
    ts[tl++] = ')';
    keccak256_init(&c);
    {
        uint8_t th[32];
        keccak256((const uint8_t *)ts, tl, th);
        keccak256_update(&c, th, 32);
    }
    for (i = 0; i < n; i++) keccak256_update(&c, words[i], 32);
    keccak256_final(&c, out);
    return EIP712_OK;
}

int eip712_digest(const cJSON *td, uint8_t digest[32])
{
    const cJSON *types, *primary, *domain, *message;
    uint8_t ds[32], hs[32];
    keccak_ctx c;
    int rc;
    static const uint8_t prefix[2] = {0x19, 0x01};
    if (!cJSON_IsObject(td)) return EIP712_E_MALFORMED;
    types = cJSON_GetObjectItemCaseSensitive(td, "types");
    primary = cJSON_GetObjectItemCaseSensitive(td, "primaryType");
    domain = cJSON_GetObjectItemCaseSensitive(td, "domain");
    message = cJSON_GetObjectItemCaseSensitive(td, "message");
    if (!cJSON_IsObject(types) || !cJSON_IsString(primary) || !cJSON_IsObject(domain) || !cJSON_IsObject(message))
        return EIP712_E_MALFORMED;
    if (strcmp(primary->valuestring, "EIP712Domain") == 0) return EIP712_E_MALFORMED;
    rc = eip712_hash_struct(types, "EIP712Domain", domain, ds);
    if (rc) return rc;
    rc = eip712_hash_struct(types, primary->valuestring, message, hs);
    if (rc) return rc;
    keccak256_init(&c);
    keccak256_update(&c, prefix, 2);
    keccak256_update(&c, ds, 32);
    keccak256_update(&c, hs, 32);
    keccak256_final(&c, digest);
    return EIP712_OK;
}
