#include "solmsg.h"

#include "ge.h"
#include "sha256.h"

#include <string.h>

#include "util.h"

const uint8_t SOLMSG_SYSTEM_PROGRAM[32] = {0};
const uint8_t SOLMSG_TOKEN_PROGRAM[32] = {0x06, 0xdd, 0xf6, 0xe1, 0xd7, 0x65, 0xa1, 0x93, 0xd9, 0xcb, 0xe1,
                                          0x46, 0xce, 0xeb, 0x79, 0xac, 0x1c, 0xb4, 0x85, 0xed, 0x5f, 0x5b,
                                          0x37, 0x91, 0x3a, 0x8c, 0xf5, 0x85, 0x7e, 0xff, 0x00, 0xa9};
const uint8_t SOLMSG_TOKEN2022_PROGRAM[32] = {0x06, 0xdd, 0xf6, 0xe1, 0xee, 0x75, 0x8f, 0xde, 0x18, 0x42, 0x5d,
                                              0xbc, 0xe4, 0x6c, 0xcd, 0xda, 0xb6, 0x1a, 0xfc, 0x4d, 0x83, 0xb9,
                                              0x0d, 0x27, 0xfe, 0xbd, 0xf9, 0x28, 0xd8, 0xa1, 0x8b, 0xfc};

typedef struct {
    const uint8_t *p;
    size_t len, pos;
} rd_t;

static bool rd_u8(rd_t *r, uint8_t *v)
{
    if (r->pos + 1 > r->len) return false;
    *v = r->p[r->pos++];
    return true;
}

/* Solana "compact-u16" (shortvec): 1-3 bytes, 7 bits each, little-endian. */
static bool rd_cu16(rd_t *r, uint16_t *v)
{
    uint32_t acc = 0;
    int i;
    for (i = 0; i < 3; i++) {
        uint8_t b;
        if (!rd_u8(r, &b)) return false;
        acc |= (uint32_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            if (acc > 0xffff) return false;
            if (i > 0 && (b & 0x7f) == 0) return false; /* non-canonical */
            *v = (uint16_t)acc;
            return true;
        }
    }
    return false;
}

static bool rd_skip(rd_t *r, size_t n, const uint8_t **start)
{
    if (r->pos + n > r->len) return false;
    *start = r->p + r->pos;
    r->pos += n;
    return true;
}

static bool rd_u32le(rd_t *r, uint32_t *v)
{
    if (r->pos + 4 > r->len) return false;
    *v = (uint32_t)r->p[r->pos] | ((uint32_t)r->p[r->pos + 1] << 8) | ((uint32_t)r->p[r->pos + 2] << 16) | ((uint32_t)r->p[r->pos + 3] << 24);
    r->pos += 4;
    return true;
}

/* Version 1 (SIMD-0385), the bytes that are signed:
 *   0x81 | signatures, read-only signed, read-only unsigned | config mask u32 | blockhash[32] |
 *   instruction count | address count | addresses | 4 bytes per set mask bit |
 *   per instruction: program index, account count, data length u16 | per instruction: account indexes, data
 * No lookup tables. Every sanitization rule of the proposal is applied here: a transaction the cluster would
 * reject is not something to put on the display. */
static int parse_v1(const uint8_t *bytes, size_t len, solmsg_t *out)
{
    rd_t r = {bytes, len, 1};
    uint32_t mask, w;
    uint8_t nix, nk, i;
    const uint8_t *headers;
    if (len > SOLMSG_V1_MAX_BYTES) return SOLMSG_E_SHORT;
    out->version = SOLMSG_VERSION_1;
    if (!rd_u8(&r, &out->num_required_signatures) || !rd_u8(&r, &out->num_readonly_signed) || !rd_u8(&r, &out->num_readonly_unsigned)) return SOLMSG_E_SHORT;
    if (out->num_required_signatures == 0 || out->num_required_signatures > 12) return SOLMSG_E_HEADER;
    if (out->num_readonly_signed >= out->num_required_signatures) return SOLMSG_E_HEADER; /* the fee payer is writable */
    if (!rd_u32le(&r, &mask)) return SOLMSG_E_SHORT;
    if (mask & ~0x1fu) return SOLMSG_E_HEADER;                 /* a setting this parser does not know */
    if (((mask & 1u) != 0) != ((mask & 2u) != 0)) return SOLMSG_E_HEADER; /* the fee takes both bits */
    if (!rd_skip(&r, 32, &out->recent_blockhash)) return SOLMSG_E_SHORT;
    if (!rd_u8(&r, &nix) || !rd_u8(&r, &nk)) return SOLMSG_E_SHORT;
    if (nix > 64 || nk > 64) return SOLMSG_E_TOOMANY;
    if (nix > SOLMSG_MAX_IX) return SOLMSG_E_TOOMANY;
    if (nk == 0 || (uint32_t)out->num_required_signatures + out->num_readonly_unsigned > nk) return SOLMSG_E_HEADER;
    out->n_static_keys = nk;
    if (!rd_skip(&r, (size_t)nk * 32, &out->static_keys)) return SOLMSG_E_SHORT;
    for (i = 1; i < nk; i++) {
        uint8_t j;
        for (j = 0; j < i; j++)
            if (memcmp(out->static_keys + 32 * (size_t)i, out->static_keys + 32 * (size_t)j, 32) == 0) return SOLMSG_E_HEADER;
    }
    if (mask & 1u) {
        uint32_t lo, hi;
        if (!rd_u32le(&r, &lo) || !rd_u32le(&r, &hi)) return SOLMSG_E_SHORT;
        out->v1_priority_lamports = ((uint64_t)hi << 32) | lo;
    }
    if (mask & 4u) { if (!rd_u32le(&r, &out->v1_compute_unit_limit)) return SOLMSG_E_SHORT; }
    if (mask & 8u) { if (!rd_u32le(&r, &w)) return SOLMSG_E_SHORT; } /* loaded accounts data size limit */
    if (mask & 16u) {
        if (!rd_u32le(&r, &w)) return SOLMSG_E_SHORT;
        if (w % 1024u != 0 || w < 32u * 1024u || w > 256u * 1024u) return SOLMSG_E_HEADER; /* heap size */
    }
    out->n_ix = nix;
    if (!rd_skip(&r, (size_t)nix * 4, &headers)) return SOLMSG_E_SHORT;
    for (i = 0; i < nix; i++) {
        solmsg_ix_t *ix = &out->ix[i];
        ix->program_index = headers[4 * (size_t)i];
        ix->n_accounts = headers[4 * (size_t)i + 1];
        ix->data_len = (uint16_t)(headers[4 * (size_t)i + 2] | ((uint16_t)headers[4 * (size_t)i + 3] << 8));
        if (!rd_skip(&r, ix->n_accounts, &ix->account_indexes)) return SOLMSG_E_SHORT;
        if (!rd_skip(&r, ix->data_len, &ix->data)) return SOLMSG_E_SHORT;
    }
    if (r.pos != r.len) return SOLMSG_E_TRAILING;
    for (i = 0; i < nix; i++) {
        const solmsg_ix_t *ix = &out->ix[i];
        uint8_t a;
        if (ix->program_index >= nk) return SOLMSG_E_INDEX;
        for (a = 0; a < ix->n_accounts; a++) if (ix->account_indexes[a] >= nk) return SOLMSG_E_INDEX;
    }
    return SOLMSG_OK;
}

int solmsg_parse(const uint8_t *bytes, size_t len, solmsg_t *out)
{
    rd_t r = {bytes, len, 0};
    uint8_t b, nk8;
    uint16_t nk, nix, i;
    uint32_t total_accounts;
    memset(out, 0, sizeof *out);
    if (len == 0) return SOLMSG_E_SHORT;
    if (bytes[0] == 0x81) return parse_v1(bytes, len, out);
    if (len > SOLMSG_MAX_BYTES) return SOLMSG_E_SHORT;
    if (!rd_u8(&r, &b)) return SOLMSG_E_SHORT;
    if (b & 0x80) {
        if ((b & 0x7f) != 0) return SOLMSG_E_VERSION;
        out->version = 0;
        if (!rd_u8(&r, &b)) return SOLMSG_E_SHORT;
    } else {
        out->version = SOLMSG_VERSION_LEGACY;
    }
    out->num_required_signatures = b;
    if (!rd_u8(&r, &out->num_readonly_signed)) return SOLMSG_E_SHORT;
    if (!rd_u8(&r, &out->num_readonly_unsigned)) return SOLMSG_E_SHORT;
    if (!rd_cu16(&r, &nk)) return SOLMSG_E_SHORT;
    if (nk == 0 || nk > 255) return SOLMSG_E_HEADER;
    nk8 = (uint8_t)nk;
    if (out->num_required_signatures == 0 || out->num_required_signatures > nk8) return SOLMSG_E_HEADER;
    if (out->num_readonly_signed > out->num_required_signatures) return SOLMSG_E_HEADER;
    if ((uint32_t)out->num_required_signatures + out->num_readonly_unsigned > nk8) return SOLMSG_E_HEADER;
    out->n_static_keys = nk8;
    if (!rd_skip(&r, (size_t)nk8 * 32, &out->static_keys)) return SOLMSG_E_SHORT;
    if (!rd_skip(&r, 32, &out->recent_blockhash)) return SOLMSG_E_SHORT;
    if (!rd_cu16(&r, &nix)) return SOLMSG_E_SHORT;
    if (nix > SOLMSG_MAX_IX) return SOLMSG_E_TOOMANY;
    out->n_ix = (uint8_t)nix;
    for (i = 0; i < nix; i++) {
        solmsg_ix_t *ix = &out->ix[i];
        uint16_t na, dl;
        if (!rd_u8(&r, &ix->program_index)) return SOLMSG_E_SHORT;
        if (!rd_cu16(&r, &na)) return SOLMSG_E_SHORT;
        if (na > 255) return SOLMSG_E_TOOMANY;
        ix->n_accounts = (uint8_t)na;
        if (!rd_skip(&r, na, &ix->account_indexes)) return SOLMSG_E_SHORT;
        if (!rd_cu16(&r, &dl)) return SOLMSG_E_SHORT;
        ix->data_len = dl;
        if (!rd_skip(&r, dl, &ix->data)) return SOLMSG_E_SHORT;
    }
    if (out->version == 0) {
        uint16_t nl, t;
        if (!rd_cu16(&r, &nl)) return SOLMSG_E_SHORT;
        if (nl > SOLMSG_MAX_LOOKUPS) return SOLMSG_E_TOOMANY;
        out->n_lookup_tables = (uint8_t)nl;
        for (t = 0; t < nl; t++) {
            const uint8_t *dummy;
            uint16_t nw, nr;
            if (!rd_skip(&r, 32, &dummy)) return SOLMSG_E_SHORT;
            if (!rd_cu16(&r, &nw)) return SOLMSG_E_SHORT;
            if (!rd_skip(&r, nw, &dummy)) return SOLMSG_E_SHORT;
            if (!rd_cu16(&r, &nr)) return SOLMSG_E_SHORT;
            if (!rd_skip(&r, nr, &dummy)) return SOLMSG_E_SHORT;
            out->n_lookup_accounts = (uint16_t)(out->n_lookup_accounts + nw + nr);
        }
    }
    if (r.pos != r.len) return SOLMSG_E_TRAILING;
    total_accounts = (uint32_t)out->n_static_keys + out->n_lookup_accounts;
    if (total_accounts > 256) return SOLMSG_E_TOOMANY;
    for (i = 0; i < nix; i++) {
        solmsg_ix_t *ix = &out->ix[i];
        uint8_t a;
        if (ix->program_index >= total_accounts) return SOLMSG_E_INDEX;
        if (ix->program_index >= out->n_static_keys) ix->touches_lookup = true;
        for (a = 0; a < ix->n_accounts; a++) {
            if (ix->account_indexes[a] >= total_accounts) return SOLMSG_E_INDEX;
            if (ix->account_indexes[a] >= out->n_static_keys) ix->touches_lookup = true;
        }
    }
    return SOLMSG_OK;
}

bool solmsg_looks_like_message(const uint8_t *bytes, size_t len)
{
    solmsg_t m;
    return solmsg_parse(bytes, len, &m) == SOLMSG_OK;
}

static bool contains(const uint8_t *bytes, size_t len, const uint8_t *needle, size_t n)
{
    if (!bytes || len < n) return false;
    for (size_t i = 0; i + n <= len; i++)
        if (bytes[i] == needle[0] && memcmp(bytes + i, needle, n) == 0) return true;
    return false;
}

bool solmsg_is_ring_key_derivation(const uint8_t *bytes, size_t len)
{
    static const char tag[] = "TSPP/derive/";
    /* SHA-256("TSPP/derive/v1"): the application domain of the off-chain envelope the keys are derived from */
    static const uint8_t domain_v1[32] = { 0x1d, 0x32, 0xa8, 0x85, 0x33, 0xaf, 0x12, 0xd3, 0x5e, 0x5a, 0xc6, 0xfc, 0xe8, 0x17, 0xa4, 0xcb, 0x81, 0x0b, 0xcc, 0x41, 0x15, 0x38, 0x6b, 0x14, 0xa7, 0x8e, 0x8b, 0x2e, 0xf0, 0x9d, 0x86, 0x4c };
    return contains(bytes, len, (const uint8_t *)tag, sizeof tag - 1) || contains(bytes, len, domain_v1, sizeof domain_v1);
}

/* ---- signMessage ---- */
static const uint8_t OFFCHAIN_DOMAIN[16] = { 0xff, 's', 'o', 'l', 'a', 'n', 'a', ' ', 'o', 'f', 'f', 'c', 'h', 'a', 'i', 'n' };

static bool is_text(const uint8_t *b, size_t len, bool allow_newline)
{
    for (size_t i = 0; i < len; i++) {
        if (b[i] == '\n') { if (!allow_newline) return false; continue; }
        if (b[i] < 0x20 || b[i] > 0x7e) return false;
    }
    return true;
}

static bool has_space(const uint8_t *b, size_t len)
{
    for (size_t i = 0; i < len; i++) if (b[i] == ' ') return true;
    return false;
}

bool solmsg_bare_text_ok(const uint8_t *bytes, size_t len)
{
    if (!bytes || len == 0 || len == 32) return false;
    if (!is_text(bytes, len, true)) return false;
    return len < 32 || has_space(bytes, len);
}

/* "<domain> wants you to sign in with your Solana account:\n<address>...": the address must be the signer's. */
static bool sign_in_names_other_account(const uint8_t *t, size_t len, const uint8_t own_key[32])
{
    static const char tail[] = " wants you to sign in with your Solana account:";
    const size_t tl = sizeof tail - 1;
    char addr[48];
    size_t eol = 0, al, e2;
    int n;
    while (eol < len && t[eol] != '\n') eol++;
    if (eol < tl || memcmp(t + eol - tl, tail, tl) != 0) return false; /* not a sign-in text */
    if (eol == len) return true;                                       /* claims a sign-in, names nobody */
    n = pubkey_to_b58(own_key, addr, sizeof addr);
    if (n <= 0) return true;
    al = (size_t)n;
    e2 = eol + 1;
    while (e2 < len && t[e2] != '\n') e2++;
    return e2 - (eol + 1) != al || memcmp(t + eol + 1, addr, al) != 0;
}

bool solmsg_signmsg_check(const uint8_t *bytes, size_t len, const uint8_t own_key[32], const uint8_t *other_key,
                          solmsg_signmsg_t *out)
{
    static const char ns_tspp[] = "TSPP/", ns_zolana[] = "zolana/";
    const uint8_t *signer = NULL; /* the one place a wallet key may appear */
    solmsg_signmsg_t r;
    memset(&r, 0, sizeof r);
    if (out) memset(out, 0, sizeof *out);
    if (!bytes || !own_key || len == 0) return false;
    if (solmsg_is_ring_key_derivation(bytes, len)) return false;
    if (contains(bytes, len, (const uint8_t *)ns_tspp, sizeof ns_tspp - 1)) return false;
    if (contains(bytes, len, (const uint8_t *)ns_zolana, sizeof ns_zolana - 1)) return false;

    if (len >= sizeof OFFCHAIN_DOMAIN && memcmp(bytes, OFFCHAIN_DOMAIN, sizeof OFFCHAIN_DOMAIN) == 0) {
        if (len < 18) return false;
        if (bytes[16] == 0) {
            /* domain(16) version(1) application domain(32) format(1) signer count(1) signer(32) length(2) body */
            static const uint8_t zero[32] = {0};
            size_t body_len;
            if (len <= 85 || len > SOLMSG_MAX_BYTES) return false;
            if (bytes[49] > 1 || bytes[50] != 1) return false;
            signer = bytes + 51;
            body_len = (size_t)bytes[83] | ((size_t)bytes[84] << 8);
            if (body_len == 0 || body_len != len - 85) return false;
            r.shape = SOLMSG_SIGNMSG_OFFCHAIN_V0;
            r.text = bytes + 85;
            r.text_len = body_len;
            if (memcmp(bytes + 17, zero, 32) != 0) r.app_domain = bytes + 17;
            if (!is_text(r.text, r.text_len, bytes[49] == 1)) return false; /* format 0 has no line breaks */
        } else if (bytes[16] == 1) {
            /* domain(16) version(1) signer count(1) signer(32) content */
            if (len <= 50 || bytes[17] != 1) return false;
            signer = bytes + 18;
            r.shape = SOLMSG_SIGNMSG_OFFCHAIN_V1;
            r.text = bytes + 50;
            r.text_len = len - 50;
            if (!is_text(r.text, r.text_len, true)) return false;
        } else {
            return false; /* another header version, or the old 20-byte header */
        }
        if (memcmp(signer, own_key, 32) != 0) return false;
    } else {
        if (bytes[0] >= 0x80) return false; /* every versioned transaction message starts here */
        if (!solmsg_bare_text_ok(bytes, len)) return false;
        r.shape = SOLMSG_SIGNMSG_TEXT;
        r.text = bytes;
        r.text_len = len;
    }
    /* a wallet key anywhere else means the bytes are shaped like something a program would read */
    for (size_t i = 0; i + 32 <= len; i++) {
        if (signer && bytes + i == signer) continue;
        if (memcmp(bytes + i, own_key, 32) == 0) return false;
        if (other_key && memcmp(bytes + i, other_key, 32) == 0) return false;
    }
    if (sign_in_names_other_account(r.text, r.text_len, own_key)) return false;
    if (out) *out = r;
    return true;
}

static const uint8_t *key_at(const solmsg_t *m, uint8_t idx)
{
    return (idx < m->n_static_keys) ? m->static_keys + 32 * (size_t)idx : NULL;
}

bool solmsg_touches_privacy_rings(const solmsg_t *m)
{
    /* ring1GaLCuJnkUMVkX2JeZLTTX17wKsV4fZmPVzXANW (pool) and regy3WAeRXMYuMYxr5iK3Bf9Arr3PcxGxyzAddbiqDQ (registry) */
    static const uint8_t pool[32] = { 0x0c, 0xbc, 0xb5, 0x78, 0xae, 0x61, 0x25, 0x81, 0x32, 0xee, 0xaf, 0x6a, 0xc1, 0x21, 0xc0, 0xa4, 0x2b, 0x88, 0xa2, 0xe8, 0x4f, 0xb1, 0x1e, 0x7e, 0x2d, 0x5b, 0xa0, 0xaf, 0xd1, 0x27, 0x31, 0x63 };
    static const uint8_t registry[32] = { 0x0c, 0xb8, 0x13, 0x27, 0x22, 0xa9, 0xc5, 0x50, 0xad, 0xe6, 0xd5, 0xeb, 0xee, 0x48, 0xa5, 0x69, 0x93, 0x08, 0xf1, 0x8a, 0x25, 0x56, 0xc5, 0xdc, 0xae, 0x44, 0x77, 0x61, 0x0e, 0xbd, 0x5d, 0x17 };
    for (uint16_t i = 0; i < m->n_static_keys; i++) {
        const uint8_t *k = m->static_keys + 32 * (size_t)i;
        if (memcmp(k, pool, 32) == 0 || memcmp(k, registry, 32) == 0) return true;
    }
    return false;
}

bool solmsg_starts_with_advance_nonce(const solmsg_t *m, uint8_t nonce_account[32])
{
    const solmsg_ix_t *ix;
    const uint8_t *pid, *nonce;
    if (m->n_ix == 0) return false;
    ix = &m->ix[0];
    if (ix->touches_lookup) return false;
    pid = key_at(m, ix->program_index);
    if (!pid || memcmp(pid, SOLMSG_SYSTEM_PROGRAM, 32) != 0) return false;
    /* AdvanceNonceAccount: [nonce (writable), RecentBlockhashes sysvar, nonce authority (signer)], u32 tag 4 */
    if (ix->data_len != 4 || rd_le32(ix->data) != 4) return false;
    if (ix->n_accounts < 3) return false;
    nonce = key_at(m, ix->account_indexes[0]);
    if (!nonce) return false;
    if (nonce_account) memcpy(nonce_account, nonce, 32);
    return true;
}

static bool ix_key_is(const solmsg_t *m, const solmsg_ix_t *ix, uint8_t pos, const uint8_t key[32])
{
    const uint8_t *k;
    if (pos >= ix->n_accounts) return false;
    k = key_at(m, ix->account_indexes[pos]);
    return k && memcmp(k, key, 32) == 0;
}

/* True if `key` appears at account position `from` or any later position (covers multisig signers). */
static bool ix_key_from(const solmsg_t *m, const solmsg_ix_t *ix, uint8_t from, const uint8_t key[32])
{
    uint8_t i;
    for (i = from; i < ix->n_accounts; i++)
        if (ix_key_is(m, ix, i, key)) return true;
    return false;
}

static void add_program(solmsg_summary_t *s, const uint8_t *pid)
{
    uint8_t i;
    for (i = 0; i < s->n_programs; i++)
        if (memcmp(s->programs[i], pid, 32) == 0) return;
    if (s->n_programs >= SOLMSG_MAX_PROGRAMS) {
        s->programs_overflow = true;
        return;
    }
    memcpy(s->programs[s->n_programs++], pid, 32);
}

static void add_lamports(solmsg_summary_t *s, uint64_t v) { s->lamports_out = u64_sat_add(s->lamports_out, v); }

static void set_dest(solmsg_summary_t *s, const solmsg_t *m, const solmsg_ix_t *ix, uint8_t pos)
{
    const uint8_t *k;
    if (pos >= ix->n_accounts) { s->multi_dest = true; return; } /* a recipient we cannot even name */
    k = key_at(m, ix->account_indexes[pos]);
    if (!k) { s->multi_dest = true; return; }
    if (s->has_dest) {
        if (memcmp(s->dest, k, 32) != 0) s->multi_dest = true; /* a second recipient: never shown as one */
        return;
    }
    memcpy(s->dest, k, 32);
    s->has_dest = true;
}

static void add_token_out(solmsg_summary_t *s, const solmsg_t *m, const solmsg_ix_t *ix, uint8_t src_pos,
                          int mint_pos, int dest_pos, uint64_t amount)
{
    solmsg_token_out_t *t;
    const uint8_t *k;
    if (s->n_token_out >= SOLMSG_MAX_TOKEN_OUT) {
        s->token_out_overflow = true;
        return;
    }
    t = &s->token_out[s->n_token_out];
    memset(t, 0, sizeof *t);
    k = key_at(m, ix->account_indexes[src_pos]);
    if (k) memcpy(t->source, k, 32);
    if (mint_pos >= 0 && (uint8_t)mint_pos < ix->n_accounts) {
        k = key_at(m, ix->account_indexes[mint_pos]);
        if (k) {
            memcpy(t->mint, k, 32);
            t->has_mint = true;
        }
    }
    if (dest_pos >= 0 && (uint8_t)dest_pos < ix->n_accounts) {
        k = key_at(m, ix->account_indexes[dest_pos]);
        if (k) {
            memcpy(t->dest, k, 32);
            t->has_dest = true;
        }
    }
    t->amount = amount;
    s->n_token_out++;
}

static void analyze_system(const solmsg_t *m, const solmsg_ix_t *ix, const uint8_t owner[32], solmsg_summary_t *s)
{
    uint32_t tag;
    if (ix->data_len < 4) return;
    tag = rd_le32(ix->data);
    switch (tag) {
    case 0: /* CreateAccount: [from, new], u32 tag, u64 lamports, u64 space, pubkey owner */
        if (ix->data_len >= 4 + 8 && ix_key_is(m, ix, 0, owner)) {
            add_lamports(s, rd_le64(ix->data + 4));
            set_dest(s, m, ix, 1);
        }
        break;
    case 1: /* Assign: [account], u32 tag, pubkey owner */
        if (ix_key_is(m, ix, 0, owner)) s->dangerous |= SOLMSG_DANGER_ASSIGN;
        break;
    case 2: /* Transfer: [from, to], u32 tag, u64 lamports */
        if (ix->data_len >= 4 + 8 && ix_key_is(m, ix, 0, owner)) {
            add_lamports(s, rd_le64(ix->data + 4));
            set_dest(s, m, ix, 1);
        }
        break;
    case 3: { /* CreateAccountWithSeed: [from, new, (base)], tag, base pubkey, seed(u64 len + bytes), lamports, space, owner */
        if (ix->data_len >= 4 + 32 + 8) {
            uint64_t seed_len = rd_le64(ix->data + 36);
            size_t off = 44;
            if (seed_len <= 64 && off + seed_len + 8 <= ix->data_len && ix_key_is(m, ix, 0, owner)) {
                add_lamports(s, rd_le64(ix->data + off + seed_len));
                set_dest(s, m, ix, 1);
            }
        }
        break;
    }
    case 4: /* AdvanceNonceAccount: [nonce, recent_blockhashes, authority]: nothing leaves */
        break;
    case 5: /* WithdrawNonceAccount: [nonce, to, recent_blockhashes, rent, authority], tag, u64 lamports */
        if (ix->data_len >= 4 + 8 && ix_key_is(m, ix, 4, owner)) {
            add_lamports(s, rd_le64(ix->data + 4));
            set_dest(s, m, ix, 1);
        }
        break;
    case 6: /* InitializeNonceAccount: [nonce, recent_blockhashes, rent], tag, pubkey authority: nothing leaves */
        break;
    case 7: /* AuthorizeNonceAccount: [nonce, current authority], tag, pubkey new authority */
        if (ix_key_is(m, ix, 1, owner)) s->dangerous |= SOLMSG_DANGER_SET_AUTHORITY; /* the owner hands the nonce over */
        break;
    case 8: /* Allocate: no lamport movement, not dangerous by itself */
        break;
    case 9:  /* AllocateWithSeed: [account, base], ... owner program at the end: the base signs the new owner */
    case 10: /* AssignWithSeed: [account, base], tag, base, seed, owner */
        if (ix_key_is(m, ix, 1, owner)) s->dangerous |= SOLMSG_DANGER_ASSIGN;
        break;
    case 11: /* TransferWithSeed: [from(derived), base(signer), to], tag, u64 lamports, seed, owner */
        if (ix->data_len >= 4 + 8 && (ix_key_is(m, ix, 1, owner) || ix_key_is(m, ix, 0, owner))) {
            add_lamports(s, rd_le64(ix->data + 4));
            set_dest(s, m, ix, 2);
        }
        break;
    case 12: /* UpgradeNonceAccount: [nonce]: nothing leaves */
        break;
    default:
        /* a System instruction this parser does not know: it may move lamports or authority in a way the
         * display cannot show, so it is dangerous until the daemon names it and the owner touches */
        s->dangerous |= SOLMSG_DANGER_UNKNOWN_IX;
        break;
    }
}

static void analyze_token(const solmsg_t *m, const solmsg_ix_t *ix, const uint8_t owner[32], solmsg_summary_t *s)
{
    uint8_t tag;
    if (ix->data_len < 1) return;
    tag = ix->data[0];
    switch (tag) {
    case 3: /* Transfer: [source, dest, authority, (signers)], u8 tag, u64 amount */
        if (ix->data_len >= 9 && ix->n_accounts >= 3 && (ix_key_from(m, ix, 2, owner) || ix_key_is(m, ix, 0, owner))) {
            add_token_out(s, m, ix, 0, -1, 1, rd_le64(ix->data + 1));
            set_dest(s, m, ix, 1);
        }
        break;
    case 12: /* TransferChecked: [source, mint, dest, authority, (signers)], u8 tag, u64 amount, u8 decimals */
        if (ix->data_len >= 10 && ix->n_accounts >= 4 && (ix_key_from(m, ix, 3, owner) || ix_key_is(m, ix, 0, owner))) {
            add_token_out(s, m, ix, 0, 1, 2, rd_le64(ix->data + 1));
            set_dest(s, m, ix, 2);
        }
        break;
    case 8: /* Burn: [account, mint, authority, (signers)], u8 tag, u64 amount */
        if (ix->data_len >= 9 && ix->n_accounts >= 3 && (ix_key_from(m, ix, 2, owner) || ix_key_is(m, ix, 0, owner)))
            add_token_out(s, m, ix, 0, 1, -1, rd_le64(ix->data + 1));
        break;
    case 4: /* Approve: [source, delegate, owner, (signers)] */
        if (ix->n_accounts >= 3 && (ix_key_from(m, ix, 2, owner) || ix_key_is(m, ix, 0, owner)))
            s->dangerous |= SOLMSG_DANGER_APPROVE;
        break;
    case 13: /* ApproveChecked: [source, mint, delegate, owner, (signers)] */
        if (ix->n_accounts >= 4 && (ix_key_from(m, ix, 3, owner) || ix_key_is(m, ix, 0, owner)))
            s->dangerous |= SOLMSG_DANGER_APPROVE;
        break;
    case 6: /* SetAuthority: [account, current authority, (signers)] */
        if (ix->n_accounts >= 2 && (ix_key_from(m, ix, 1, owner) || ix_key_is(m, ix, 0, owner)))
            s->dangerous |= SOLMSG_DANGER_SET_AUTHORITY;
        break;
    case 9: /* CloseAccount: [account, dest, owner, (signers)] */
        if (ix->n_accounts >= 3 && (ix_key_from(m, ix, 2, owner) || ix_key_is(m, ix, 0, owner)))
            s->dangerous |= SOLMSG_DANGER_CLOSE_ACCOUNT;
        break;
    case 15: /* BurnChecked: [account, mint, authority, (signers)], u8 tag, u64 amount, u8 decimals */
        if (ix->data_len >= 10 && ix->n_accounts >= 3 && (ix_key_from(m, ix, 2, owner) || ix_key_is(m, ix, 0, owner)))
            add_token_out(s, m, ix, 0, 1, -1, rd_le64(ix->data + 1));
        break;
    case 26: /* Token-2022 TransferFeeExtension: sub-tag 1 = TransferCheckedWithFee
              * [source, mint, dest, authority, (signers)], u8 26, u8 1, u64 amount, u8 decimals, u64 fee */
        if (ix->data_len >= 2 && ix->data[1] == 1) {
            if (ix->data_len >= 19 && ix->n_accounts >= 4 && (ix_key_from(m, ix, 3, owner) || ix_key_is(m, ix, 0, owner))) {
                add_token_out(s, m, ix, 0, 1, 2, rd_le64(ix->data + 2));
                set_dest(s, m, ix, 2);
            }
        } else {
            s->dangerous |= SOLMSG_DANGER_UNKNOWN_IX; /* fee config / harvest / withdraw: not something the display can show */
        }
        break;
    /* instructions that move nothing out of the owner: mint / account / multisig initialisation, revoke,
     * mint-to (the owner would be the mint authority, not a payer), freeze / thaw, sync, size and ui-amount
     * helpers, immutable-owner, close-authority / pointer / interest / memo-transfer / cpi-guard extension setup */
    case 0: case 1: case 2: case 5: case 7: case 10: case 11: case 14: case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23: case 24: case 25: case 28: case 29: case 30: case 31: case 32: case 33:
    case 34: case 36: case 39: case 40: case 41:
        break;
    default:
        /* confidential transfers (27), withdraw excess lamports (38), permanent delegate (35), anything newer:
         * assets can leave in ways the display cannot show, so the daemon must name it and the owner touch */
        s->dangerous |= SOLMSG_DANGER_UNKNOWN_IX;
        break;
    }
}

/* ComputeBudget: u8 tag; 2 = SetComputeUnitLimit(u32), 3 = SetComputeUnitPrice(u64 micro-lamports per unit). */
static void analyze_compute_budget(const solmsg_ix_t *ix, bool *has_limit, uint32_t *limit, uint64_t *price)
{
    if (ix->data_len < 1) return;
    if (ix->data[0] == 2 && ix->data_len >= 5) { *has_limit = true; *limit = rd_le32(ix->data + 1); }
    else if (ix->data[0] == 3 && ix->data_len >= 9) *price = rd_le64(ix->data + 1);
}

#define SOLMSG_BASE_FEE_PER_SIG 5000u
#define SOLMSG_MAX_CU 1400000u
#define SOLMSG_DEFAULT_CU_PER_IX 200000u

/* Core programs (base58 decoded once). */
static const char *const CORE_B58[] = {
    "ATokenGPvbdGVxr1b2hvZbsiqW5xWH25efTNsLJA8knL", /* Associated Token Account */
    "ComputeBudget111111111111111111111111111111", /* Compute Budget */
    "MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr", /* Memo v2 */
};
static uint8_t g_core[3][32];
static bool g_core_ready;

bool solmsg_is_core_program(const uint8_t pid[32])
{
    size_t i;
    if (memcmp(pid, SOLMSG_SYSTEM_PROGRAM, 32) == 0 || memcmp(pid, SOLMSG_TOKEN_PROGRAM, 32) == 0 ||
        memcmp(pid, SOLMSG_TOKEN2022_PROGRAM, 32) == 0)
        return true;
    if (!g_core_ready) {
        for (i = 0; i < 3; i++)
            if (!pubkey_from_b58(CORE_B58[i], g_core[i])) memset(g_core[i], 0xff, 32); /* never matches a real key */
        g_core_ready = true;
    }
    for (i = 0; i < 3; i++)
        if (memcmp(pid, g_core[i], 32) == 0) return true;
    return false;
}


/* ---- Jupiter v6: the one non-core program the controller reads into, so the overnight allowance can cover
 * a swap. Four route shapes; in each the owner must be the transfer authority and the output must land in
 * the owner's own associated token account of the destination mint. Anything else is SOLMSG_DANGER_ROUTE. */
static const char JUPITER_V6_B58[] = "JUP6LkbZbjS1jKKwapdHNy74zcZ3tLUZoi5QNyVTaV4";
static uint8_t g_jup[32];
static uint8_t g_jup_disc[4][8]; /* route, shared_accounts_route, exact_out_route, shared_accounts_exact_out_route */
static bool g_jup_ready;
static void jup_init(void)
{
    static const char *const names[4] = {"global:route", "global:shared_accounts_route", "global:exact_out_route", "global:shared_accounts_exact_out_route"};
    uint8_t h[32];
    size_t i;
    if (g_jup_ready) return;
    (void)pubkey_from_b58(JUPITER_V6_B58, g_jup);
    for (i = 0; i < 4; i++) { sha256((const uint8_t *)names[i], strlen(names[i]), h); memcpy(g_jup_disc[i], h, 8); }
    g_jup_ready = true;
}

bool solmsg_ata_derive(const uint8_t owner[32], const uint8_t token_program[32], const uint8_t mint[32], uint8_t out[32])
{
    static const char PDA_MARKER[] = "ProgramDerivedAddress";
    uint8_t buf[32 * 3 + 1 + 32 + sizeof PDA_MARKER - 1];
    int bump;
    solmsg_is_core_program(owner); /* makes sure g_core (the ATA program id) is decoded */
    memcpy(buf, owner, 32); memcpy(buf + 32, token_program, 32); memcpy(buf + 64, mint, 32);
    memcpy(buf + 97, g_core[0], 32);
    memcpy(buf + 129, PDA_MARKER, sizeof PDA_MARKER - 1);
    for (bump = 255; bump >= 0; bump--) {
        ge_p3 pt;
        buf[96] = (uint8_t)bump;
        sha256(buf, sizeof buf, out);
        /* a program address must not be a point on the curve: the first bump whose hash fails to decode wins */
        if (ge_frombytes_negate_vartime(&pt, out) != 0) return true;
    }
    return false;
}

/* True when `ix` is a Jupiter route this parser understands and its output goes to the owner's own ATA. */
static bool jupiter_route_ok(const solmsg_t *m, const solmsg_ix_t *ix, const uint8_t owner[32], uint8_t swap_mint[32])
{
    int shape = -1, i;
    uint8_t auth_pos, dest_pos, mint_pos, opt_dest_pos;
    const uint8_t *tp, *mint, *dest, *opt;
    uint8_t ata[32];
    jup_init();
    if (ix->data_len < 8) return false;
    for (i = 0; i < 4; i++) if (memcmp(ix->data, g_jup_disc[i], 8) == 0) { shape = i; break; }
    switch (shape) {
    case 0: /* route: [token_program, user_transfer_authority, user_source, user_destination, destination(opt), destination_mint, ...] */
        if (ix->n_accounts < 6) return false;
        auth_pos = 1; dest_pos = 3; opt_dest_pos = 4; mint_pos = 5; break;
    case 1: /* shared_accounts_route: [token_program, program_authority, user_transfer_authority, source, prog_source, prog_dest, destination, source_mint, destination_mint, ...] */
        if (ix->n_accounts < 9) return false;
        auth_pos = 2; dest_pos = 6; opt_dest_pos = 6; mint_pos = 8; break;
    case 2: /* exact_out_route: [token_program, user_transfer_authority, user_source, user_destination, destination(opt), source_mint, destination_mint, ...] */
        if (ix->n_accounts < 7) return false;
        auth_pos = 1; dest_pos = 3; opt_dest_pos = 4; mint_pos = 6; break;
    case 3: /* shared_accounts_exact_out_route: same layout as shared_accounts_route */
        if (ix->n_accounts < 9) return false;
        auth_pos = 2; dest_pos = 6; opt_dest_pos = 6; mint_pos = 8; break;
    default:
        return false;
    }
    if (!ix_key_is(m, ix, auth_pos, owner)) return false;
    tp = key_at(m, ix->account_indexes[0]);
    mint = key_at(m, ix->account_indexes[mint_pos]);
    dest = key_at(m, ix->account_indexes[dest_pos]);
    opt = key_at(m, ix->account_indexes[opt_dest_pos]);
    if (!tp || !mint || !dest || !opt) return false;
    if (memcmp(tp, SOLMSG_TOKEN_PROGRAM, 32) != 0 && memcmp(tp, SOLMSG_TOKEN2022_PROGRAM, 32) != 0) return false;
    if (!solmsg_ata_derive(owner, tp, mint, ata)) return false;
    if (memcmp(dest, ata, 32) != 0) return false;
    /* the optional third-party destination is either absent (the program id stands in) or the same ATA */
    if (memcmp(opt, g_jup, 32) != 0 && memcmp(opt, ata, 32) != 0) return false;
    memcpy(swap_mint, mint, 32);
    return true;
}

void solmsg_analyze(const solmsg_t *m, const uint8_t owner[32], solmsg_summary_t *s)
{
    bool has_limit = false;
    uint32_t cu_limit = 0;
    uint64_t cu_price = 0;
    uint8_t n_cb = 0;
    const uint8_t *unverified[SOLMSG_MAX_IX];
    uint8_t n_unverified_ix = 0;
    uint8_t i;
    memset(s, 0, sizeof *s);
    s->fee_payer_is_owner = m->n_static_keys > 0 && m->num_required_signatures > 0 &&
                            memcmp(m->static_keys, owner, 32) == 0;
    for (i = 0; i < m->n_ix; i++) {
        const solmsg_ix_t *ix = &m->ix[i];
        const uint8_t *pid;
        if (ix->touches_lookup) {
            /* Cannot know what this instruction does without the table: refuse unless explicitly claimed. */
            s->dangerous |= SOLMSG_DANGER_LOOKUP_TABLE;
            pid = key_at(m, ix->program_index);
            if (pid) add_program(s, pid);
            continue;
        }
        pid = key_at(m, ix->program_index);
        if (!pid) continue;
        add_program(s, pid);
        if (memcmp(pid, SOLMSG_SYSTEM_PROGRAM, 32) == 0) {
            analyze_system(m, ix, owner, s);
        } else if (memcmp(pid, SOLMSG_TOKEN_PROGRAM, 32) == 0 || memcmp(pid, SOLMSG_TOKEN2022_PROGRAM, 32) == 0) {
            analyze_token(m, ix, owner, s);
        } else if (solmsg_is_core_program(pid) && memcmp(pid, g_core[1], 32) == 0) {
            n_cb++;
            analyze_compute_budget(ix, &has_limit, &cu_limit, &cu_price);
        } else if (!solmsg_is_core_program(pid)) {
            uint8_t mint[32];
            jup_init();
            if (memcmp(pid, g_jup, 32) == 0 && jupiter_route_ok(m, ix, owner, mint)) {
                if (!s->has_swap) { s->has_swap = true; memcpy(s->swap_mint, mint, 32); }
            } else {
                if (memcmp(pid, g_jup, 32) == 0) s->dangerous |= SOLMSG_DANGER_ROUTE;
                unverified[n_unverified_ix < SOLMSG_MAX_IX ? n_unverified_ix++ : SOLMSG_MAX_IX - 1] = pid;
            }
        }
    }
    /* distinct non-core programs that were not read end to end: the allowance never covers them */
    {
        uint8_t i, j, n = 0;
        for (i = 0; i < n_unverified_ix; i++) {
            bool dup = false;
            for (j = 0; j < i; j++) if (memcmp(unverified[j], unverified[i], 32) == 0) { dup = true; break; }
            if (!dup && n < 255) n++;
        }
        s->n_unverified_programs = n;
    }
    /* the fee: what the leader is paid even when no transfer is in the message */
    {
        uint64_t limit = has_limit ? cu_limit : (uint64_t)SOLMSG_DEFAULT_CU_PER_IX * (m->n_ix > n_cb ? m->n_ix - n_cb : 0);
        uint64_t priority;
        if (limit > SOLMSG_MAX_CU) limit = SOLMSG_MAX_CU;
        if (limit && cu_price > UINT64_MAX / limit) priority = UINT64_MAX;
        else priority = limit ? (limit * cu_price + 999999u) / 1000000u : 0;
        /* version 1: a flat amount in the header; ComputeBudget instructions configure nothing there */
        if (m->version == SOLMSG_VERSION_1) priority = m->v1_priority_lamports;
        s->priority_lamports = priority;
        s->fee_lamports = u64_sat_add((uint64_t)SOLMSG_BASE_FEE_PER_SIG * m->num_required_signatures, priority);
    }
}

static const struct {
    uint32_t bit;
    const char *name;
} DANGER_NAMES[] = {
    {SOLMSG_DANGER_SET_AUTHORITY, "set_authority"},
    {SOLMSG_DANGER_APPROVE, "approve"},
    {SOLMSG_DANGER_CLOSE_ACCOUNT, "close_account"},
    {SOLMSG_DANGER_ASSIGN, "assign"},
    {SOLMSG_DANGER_LOOKUP_TABLE, "lookup_table"},
    {SOLMSG_DANGER_UNKNOWN_PROGRAM, "unknown_program"},
    {SOLMSG_DANGER_UNKNOWN_IX, "unknown_instruction"},
    {SOLMSG_DANGER_ROUTE, "route_dest"},
};

const char *solmsg_danger_name(uint32_t bit)
{
    size_t i;
    for (i = 0; i < sizeof DANGER_NAMES / sizeof DANGER_NAMES[0]; i++)
        if (DANGER_NAMES[i].bit == bit) return DANGER_NAMES[i].name;
    return NULL;
}

uint32_t solmsg_danger_from_name(const char *name)
{
    size_t i;
    if (!name) return 0;
    for (i = 0; i < sizeof DANGER_NAMES / sizeof DANGER_NAMES[0]; i++)
        if (strcmp(DANGER_NAMES[i].name, name) == 0) return DANGER_NAMES[i].bit;
    /* Accept the CamelCase spellings from PROTOCOL.md prose too. */
    if (strcmp(name, "SetAuthority") == 0) return SOLMSG_DANGER_SET_AUTHORITY;
    if (strcmp(name, "Approve") == 0) return SOLMSG_DANGER_APPROVE;
    if (strcmp(name, "CloseAccount") == 0) return SOLMSG_DANGER_CLOSE_ACCOUNT;
    if (strcmp(name, "Assign") == 0) return SOLMSG_DANGER_ASSIGN;
    return 0;
}
