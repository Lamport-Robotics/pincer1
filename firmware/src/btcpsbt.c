#include "btcpsbt.h"

#include <string.h>

#include "chainaddr.h"
#include "ripemd160.h"
#include "secp256k1.h"
#include "sha256.h"
#include "util.h"

static const uint8_t MAGIC[5] = {0x70, 0x73, 0x62, 0x74, 0xff};

/* ---- readers ---- */
typedef struct {
    const uint8_t *p;
    size_t len, off;
} rd_t;

static bool rd_u8(rd_t *r, uint8_t *v) { if (r->off + 1 > r->len) return false; *v = r->p[r->off++]; return true; }
static bool rd_u32(rd_t *r, uint32_t *v) { if (r->off + 4 > r->len) return false; *v = rd_le32(r->p + r->off); r->off += 4; return true; }
static bool rd_u64(rd_t *r, uint64_t *v) { if (r->off + 8 > r->len) return false; *v = rd_le64(r->p + r->off); r->off += 8; return true; }
static bool rd_bytes(rd_t *r, size_t n) { if (r->off + n > r->len) return false; r->off += n; return true; }

static bool rd_varint(rd_t *r, uint64_t *v)
{
    uint8_t b;
    if (!rd_u8(r, &b)) return false;
    if (b < 0xfd) { *v = b; return true; }
    if (b == 0xfd) { uint32_t x; if (r->off + 2 > r->len) return false; x = rd_le16(r->p + r->off); r->off += 2; if (x < 0xfd) return false; *v = x; return true; }
    if (b == 0xfe) { uint32_t x; if (!rd_u32(r, &x)) return false; if (x < 0x10000) return false; *v = x; return true; }
    { uint64_t x; if (!rd_u64(r, &x)) return false; if (x < 0x100000000ull) return false; *v = x; return true; }
}

/* ---- unsigned transaction ---- */
static int parse_tx(const uint8_t *tx, size_t len, btcpsbt_t *p)
{
    rd_t r = {tx, len, 0};
    uint32_t v32;
    uint64_t n, i;
    if (!rd_u32(&r, &v32)) return BTCPSBT_E_FORMAT;
    p->version = (int32_t)v32;
    if (!rd_varint(&r, &n)) return BTCPSBT_E_FORMAT;
    if (n == 0) return BTCPSBT_E_VERSION; /* 0x00 marker = segwit serialisation: not allowed in a PSBT */
    if (n > BTCPSBT_MAX_INPUTS) return BTCPSBT_E_TOOMANY;
    p->n_in = (uint8_t)n;
    for (i = 0; i < n; i++) {
        btcpsbt_input_t *in = &p->in[i];
        uint64_t sl;
        if (r.off + 32 > r.len) return BTCPSBT_E_FORMAT;
        memcpy(in->txid, r.p + r.off, 32);
        r.off += 32;
        if (!rd_u32(&r, &in->vout)) return BTCPSBT_E_FORMAT;
        if (!rd_varint(&r, &sl)) return BTCPSBT_E_FORMAT;
        if (sl != 0) return BTCPSBT_E_VERSION; /* scriptSig must be empty in a PSBT */
        if (!rd_u32(&r, &in->sequence)) return BTCPSBT_E_FORMAT;
    }
    if (!rd_varint(&r, &n)) return BTCPSBT_E_FORMAT;
    if (n == 0) return BTCPSBT_E_FORMAT;
    if (n > BTCPSBT_MAX_OUTPUTS) return BTCPSBT_E_TOOMANY;
    p->n_out = (uint8_t)n;
    for (i = 0; i < n; i++) {
        btcpsbt_output_t *o = &p->out[i];
        uint64_t sl;
        if (!rd_u64(&r, &o->amount)) return BTCPSBT_E_FORMAT;
        if (o->amount > 2100000000000000ull) return BTCPSBT_E_AMOUNTS;
        if (!rd_varint(&r, &sl)) return BTCPSBT_E_FORMAT;
        if (sl == 0 || sl > BTCPSBT_MAX_SCRIPT || r.off + sl > r.len) return BTCPSBT_E_FORMAT;
        memcpy(o->spk, r.p + r.off, sl);
        o->spk_len = (uint8_t)sl;
        r.off += sl;
    }
    if (!rd_u32(&r, &p->locktime)) return BTCPSBT_E_FORMAT;
    if (r.off != r.len) return BTCPSBT_E_FORMAT;
    return BTCPSBT_OK;
}

/* One key-value map: calls `kv` for each entry, stops at the 0x00 separator (its offset -> *sep). */
typedef int (*kv_fn)(void *ctx, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen);

static int parse_map(rd_t *r, kv_fn kv, void *ctx, size_t *sep)
{
    for (;;) {
        uint64_t klen, vlen;
        const uint8_t *key, *val;
        size_t key_at = r->off;
        if (!rd_varint(r, &klen)) return BTCPSBT_E_FORMAT;
        if (klen == 0) { *sep = key_at; return BTCPSBT_OK; }
        if (klen > 1000) return BTCPSBT_E_FORMAT;
        key = r->p + r->off;
        if (!rd_bytes(r, (size_t)klen)) return BTCPSBT_E_FORMAT;
        if (!rd_varint(r, &vlen)) return BTCPSBT_E_FORMAT;
        if (vlen > BTCPSBT_MAX_BYTES) return BTCPSBT_E_FORMAT;
        val = r->p + r->off;
        if (!rd_bytes(r, (size_t)vlen)) return BTCPSBT_E_FORMAT;
        if (kv) {
            int rc = kv(ctx, key, (size_t)klen, val, (size_t)vlen);
            if (rc) return rc;
        }
    }
}

typedef struct {
    btcpsbt_t *p;
    const uint8_t *base;
    bool have_tx;
} gctx_t;

static int global_kv(void *c, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen)
{
    gctx_t *g = c;
    if (key[0] == 0x00) { /* PSBT_GLOBAL_UNSIGNED_TX */
        int rc;
        if (klen != 1 || g->have_tx) return BTCPSBT_E_FORMAT;
        rc = parse_tx(val, vlen, g->p);
        if (rc) return rc;
        g->p->tx_off = (size_t)(val - g->base);
        g->p->tx_len = vlen;
        g->have_tx = true;
    } else if (key[0] == 0xfb) { /* PSBT_GLOBAL_VERSION */
        if (klen != 1 || vlen != 4) return BTCPSBT_E_FORMAT;
        if (rd_le32(val) != 0) return BTCPSBT_E_VERSION;
    }
    return BTCPSBT_OK;
}

static bool read_derivation(const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen, uint8_t pubkey[33],
                            uint32_t path[5], uint8_t *path_len)
{
    size_t n, i;
    if (klen != 34 || (key[1] != 0x02 && key[1] != 0x03)) return false; /* compressed keys only */
    if (vlen < 4 || (vlen - 4) % 4) return false;
    n = (vlen - 4) / 4;
    if (n > 5) return false; /* deeper paths are not ours */
    memcpy(pubkey, key + 1, 33);
    for (i = 0; i < n; i++) path[i] = rd_le32(val + 4 + 4 * i);
    *path_len = (uint8_t)n;
    return true;
}

static int input_kv(void *c, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen)
{
    btcpsbt_input_t *in = c;
    switch (key[0]) {
    case 0x01: { /* witness_utxo: amount(8) || scriptPubKey varlen */
        rd_t r = {val, vlen, 0};
        uint64_t sl;
        if (klen != 1 || in->has_witness_utxo) return BTCPSBT_E_FORMAT;
        if (!rd_u64(&r, &in->amount) || !rd_varint(&r, &sl) || sl == 0 || sl > BTCPSBT_MAX_SCRIPT || r.off + sl != vlen)
            return BTCPSBT_E_FORMAT;
        if (in->amount > 2100000000000000ull) return BTCPSBT_E_AMOUNTS;
        memcpy(in->spk, val + r.off, sl);
        in->spk_len = (uint8_t)sl;
        in->has_witness_utxo = true;
        break;
    }
    case 0x03: /* sighash_type */
        if (klen != 1 || vlen != 4) return BTCPSBT_E_FORMAT;
        in->sighash = rd_le32(val);
        if (in->sighash == 0) in->sighash = BTCPSBT_SIGHASH_ALL;
        break;
    case 0x06: /* bip32_derivation */
        if (in->has_derivation) break; /* first one wins; others would belong to co-signers */
        if (!read_derivation(key, klen, val, vlen, in->pubkey, in->path, &in->path_len)) return BTCPSBT_E_FORMAT;
        in->has_derivation = true;
        break;
    case 0x07: case 0x08: /* final scripts: already finalised input */
        return BTCPSBT_E_FORMAT;
    default:
        break;
    }
    return BTCPSBT_OK;
}

static int output_kv(void *c, const uint8_t *key, size_t klen, const uint8_t *val, size_t vlen)
{
    btcpsbt_output_t *o = c;
    if (key[0] == 0x02) {
        if (o->has_derivation) return BTCPSBT_OK;
        if (!read_derivation(key, klen, val, vlen, o->pubkey, o->path, &o->path_len)) return BTCPSBT_E_FORMAT;
        o->has_derivation = true;
    }
    return BTCPSBT_OK;
}

int btcpsbt_parse(const uint8_t *bytes, size_t len, btcpsbt_t *p)
{
    rd_t r = {bytes, len, 0};
    gctx_t g;
    size_t sep;
    int rc;
    uint8_t i;
    memset(p, 0, sizeof *p);
    if (len > BTCPSBT_MAX_BYTES) return BTCPSBT_E_SIZE;
    if (len < 5 || memcmp(bytes, MAGIC, 5) != 0) return BTCPSBT_E_MAGIC;
    r.off = 5;
    g.p = p; g.base = bytes; g.have_tx = false;
    rc = parse_map(&r, global_kv, &g, &sep);
    if (rc) return rc;
    if (!g.have_tx) return BTCPSBT_E_FORMAT;
    p->inputs_off = r.off;
    for (i = 0; i < p->n_in; i++) {
        p->in[i].map_off = r.off;
        rc = parse_map(&r, input_kv, &p->in[i], &p->in[i].map_sep);
        if (rc) return rc;
    }
    p->outputs_off = r.off;
    for (i = 0; i < p->n_out; i++) {
        rc = parse_map(&r, output_kv, &p->out[i], &sep);
        if (rc) return rc;
    }
    if (r.off != len) return BTCPSBT_E_FORMAT;
    return BTCPSBT_OK;
}

static bool path_is(const uint32_t *path, uint8_t len, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return len == 5 && path[0] == a && path[1] == b && path[2] == c && path[3] == d;
}

int btcpsbt_analyze(btcpsbt_t *p, const uint8_t owner_pub33[33])
{
    uint8_t owner_spk[22], pkh[20];
    uint8_t i;
    hash160(owner_pub33, 33, pkh);
    btc_p2wpkh_script(pkh, owner_spk);
    p->n_owner_inputs = 0;
    p->sats_in = p->sats_out = p->sats_change = p->fee = 0;
    p->has_dest = false;
    p->dest_spk_len = 0;
    p->dangerous = 0;
    p->n_non_change = 0;
    for (i = 0; i < p->n_in; i++) {
        btcpsbt_input_t *in = &p->in[i];
        in->is_owner = in->has_derivation && memcmp(in->pubkey, owner_pub33, 33) == 0 &&
                       path_is(in->path, in->path_len, BIP32_PATH_BTC[0], BIP32_PATH_BTC[1], BIP32_PATH_BTC[2], 0) &&
                       in->path[4] == 0;
        if (!in->is_owner) return BTCPSBT_E_FOREIGN_INPUT;
        if (!in->has_witness_utxo) return BTCPSBT_E_NO_UTXO;
        if (in->spk_len != 22 || memcmp(in->spk, owner_spk, 22) != 0) return BTCPSBT_E_FOREIGN_INPUT;
        if (in->sighash == 0) in->sighash = BTCPSBT_SIGHASH_ALL;
        switch (in->sighash & 0x7f) {
        case BTCPSBT_SIGHASH_ALL: break;
        case BTCPSBT_SIGHASH_SINGLE:
            if (i >= p->n_out) return BTCPSBT_E_SIGHASH; /* the SIGHASH_SINGLE bug: never sign that */
            break;
        default: return BTCPSBT_E_SIGHASH; /* NONE: anyone could redirect the funds */
        }
        if (in->sighash != BTCPSBT_SIGHASH_ALL) p->dangerous |= BTCPSBT_DANGER_SIGHASH_NOT_ALL;
        p->n_owner_inputs++;
        if (p->sats_in > 2100000000000000ull - in->amount) return BTCPSBT_E_AMOUNTS;
        p->sats_in += in->amount;
    }
    /* Change is only ever our own receive script (0/0): the chip holds that one key and nothing on a 1/i
     * path, so coins sent there would be unspendable. Such outputs count as leaving and are shown. */
    for (i = 0; i < p->n_out; i++) {
        btcpsbt_output_t *o = &p->out[i];
        o->is_change = o->spk_len == 22 && memcmp(o->spk, owner_spk, 22) == 0;
        if (o->is_change) {
            p->sats_change += o->amount;
        } else {
            p->sats_out += o->amount;
            p->n_non_change++;
            if (!p->has_dest) {
                p->has_dest = true;
                memcpy(p->dest_spk, o->spk, o->spk_len);
                p->dest_spk_len = o->spk_len;
            }
        }
    }
    if (p->sats_out + p->sats_change > p->sats_in) return BTCPSBT_E_AMOUNTS;
    p->fee = p->sats_in - p->sats_out - p->sats_change;
    if (p->fee > 0 && (p->sats_out == 0 || p->fee >= p->sats_out / 10)) p->dangerous |= BTCPSBT_DANGER_HIGH_FEE;
    return BTCPSBT_OK;
}

/* ---- BIP-143 ---- */
static void put_varint(uint8_t *b, size_t *o, uint64_t v)
{
    if (v < 0xfd) { b[(*o)++] = (uint8_t)v; }
    else if (v <= 0xffff) { b[(*o)++] = 0xfd; wr_le16(b + *o, (uint16_t)v); *o += 2; }
    else { b[(*o)++] = 0xfe; wr_le32(b + *o, (uint32_t)v); *o += 4; }
}

bool btcpsbt_sighash(const btcpsbt_t *p, const uint8_t *bytes, uint8_t idx, uint8_t digest[32])
{
    sha256_ctx c;
    uint8_t h_prevouts[32] = {0}, h_sequence[32] = {0}, h_outputs[32] = {0}, tmp[32], buf[8 + 1 + BTCPSBT_MAX_SCRIPT];
    uint8_t pre[4 + 32 + 32 + 36 + 26 + 8 + 4 + 32 + 4 + 4];
    size_t o = 0;
    uint8_t i;
    const btcpsbt_input_t *in;
    uint32_t sh;
    bool anyone, single;
    (void)bytes;
    if (idx >= p->n_in || !p->in[idx].is_owner || !p->in[idx].has_witness_utxo) return false;
    in = &p->in[idx];
    sh = in->sighash ? in->sighash : BTCPSBT_SIGHASH_ALL;
    anyone = (sh & BTCPSBT_SIGHASH_ANYONECANPAY) != 0;
    single = (sh & 0x7f) == BTCPSBT_SIGHASH_SINGLE;
    if ((sh & 0x7f) == BTCPSBT_SIGHASH_NONE) return false;
    if (!anyone) {
        sha256_init(&c);
        for (i = 0; i < p->n_in; i++) {
            uint8_t le[4];
            sha256_update(&c, p->in[i].txid, 32);
            wr_le32(le, p->in[i].vout);
            sha256_update(&c, le, 4);
        }
        sha256_final(&c, tmp); sha256(tmp, 32, h_prevouts);
        if (!single) {
            sha256_init(&c);
            for (i = 0; i < p->n_in; i++) { uint8_t le[4]; wr_le32(le, p->in[i].sequence); sha256_update(&c, le, 4); }
            sha256_final(&c, tmp); sha256(tmp, 32, h_sequence);
        }
    }
    if (!single) {
        sha256_init(&c);
        for (i = 0; i < p->n_out; i++) {
            size_t bo = 0;
            wr_le64(buf, p->out[i].amount); bo = 8;
            put_varint(buf, &bo, p->out[i].spk_len);
            memcpy(buf + bo, p->out[i].spk, p->out[i].spk_len); bo += p->out[i].spk_len;
            sha256_update(&c, buf, bo);
        }
        sha256_final(&c, tmp); sha256(tmp, 32, h_outputs);
    } else {
        size_t bo = 0;
        if (idx >= p->n_out) return false;
        wr_le64(buf, p->out[idx].amount); bo = 8;
        put_varint(buf, &bo, p->out[idx].spk_len);
        memcpy(buf + bo, p->out[idx].spk, p->out[idx].spk_len); bo += p->out[idx].spk_len;
        sha256d(buf, bo, h_outputs);
    }
    wr_le32(pre + o, (uint32_t)p->version); o += 4;
    memcpy(pre + o, h_prevouts, 32); o += 32;
    memcpy(pre + o, h_sequence, 32); o += 32;
    memcpy(pre + o, in->txid, 32); o += 32;
    wr_le32(pre + o, in->vout); o += 4;
    /* scriptCode for P2WPKH: 0x1976a914 <pkh> 88ac */
    pre[o++] = 0x19; pre[o++] = 0x76; pre[o++] = 0xa9; pre[o++] = 0x14;
    memcpy(pre + o, in->spk + 2, 20); o += 20;
    pre[o++] = 0x88; pre[o++] = 0xac;
    wr_le64(pre + o, in->amount); o += 8;
    wr_le32(pre + o, in->sequence); o += 4;
    memcpy(pre + o, h_outputs, 32); o += 32;
    wr_le32(pre + o, p->locktime); o += 4;
    wr_le32(pre + o, sh); o += 4;
    sha256d(pre, o, digest);
    return true;
}

size_t btcpsbt_insert_sigs(const btcpsbt_t *p, const uint8_t *bytes, size_t len, const uint8_t (*sigs)[64], uint8_t *out,
                           size_t cap)
{
    size_t o = 0, src = 0;
    uint8_t i;
    for (i = 0; i < p->n_in; i++) {
        const btcpsbt_input_t *in = &p->in[i];
        size_t chunk = in->map_sep - src;
        uint8_t der[SECP256K1_DER_MAX];
        size_t dl;
        if (in->map_sep < src || in->map_sep >= len) return 0;
        if (o + chunk > cap) return 0;
        memcpy(out + o, bytes + src, chunk); o += chunk;
        src = in->map_sep;
        if (!in->is_owner) continue;
        dl = secp256k1_der_encode(sigs[i], der);
        if (dl == 0 || dl > 72) return 0;
        if (o + 1 + 34 + 1 + dl + 1 > cap) return 0;
        out[o++] = 34;                 /* key length */
        out[o++] = 0x02;               /* PSBT_IN_PARTIAL_SIG */
        memcpy(out + o, in->pubkey, 33); o += 33;
        out[o++] = (uint8_t)(dl + 1);  /* value length */
        memcpy(out + o, der, dl); o += dl;
        out[o++] = (uint8_t)(in->sighash ? in->sighash : BTCPSBT_SIGHASH_ALL);
    }
    if (o + (len - src) > cap) return 0;
    memcpy(out + o, bytes + src, len - src); o += len - src;
    return o;
}

bool btcpsbt_looks_like_psbt(const uint8_t *bytes, size_t len) { return len >= 5 && memcmp(bytes, MAGIC, 5) == 0; }

/* ---- BIP-322 simple (P2WPKH) ---- */
static void tagged_hash(const char *tag, const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint8_t th[32];
    sha256_ctx c;
    sha256((const uint8_t *)tag, strlen(tag), th);
    sha256_init(&c);
    sha256_update(&c, th, 32);
    sha256_update(&c, th, 32);
    sha256_update(&c, msg, len);
    sha256_final(&c, out);
}

void btcpsbt_bip322_hashes(const uint8_t *msg, size_t len, const uint8_t pkh[20], uint8_t message_hash[32],
                           uint8_t to_spend_txid[32], uint8_t to_sign_txid[32])
{
    uint8_t tx[4 + 1 + 32 + 4 + 1 + 34 + 4 + 1 + 8 + 1 + 22 + 4];
    size_t o = 0;
    tagged_hash("BIP0322-signed-message", msg, len, message_hash);
    /* to_spend: version 0, 1 input (0000..:ffffffff, scriptSig = OP_0 PUSH32 message_hash, seq 0),
     * 1 output (0 sats, message_challenge = P2WPKH script), locktime 0 */
    wr_le32(tx + o, 0); o += 4;
    tx[o++] = 1;
    memset(tx + o, 0, 32); o += 32;
    wr_le32(tx + o, 0xffffffffu); o += 4;
    tx[o++] = 34; tx[o++] = 0x00; tx[o++] = 0x20;
    memcpy(tx + o, message_hash, 32); o += 32;
    wr_le32(tx + o, 0); o += 4;
    tx[o++] = 1;
    wr_le64(tx + o, 0); o += 8;
    tx[o++] = 22;
    btc_p2wpkh_script(pkh, tx + o); o += 22;
    wr_le32(tx + o, 0); o += 4;
    sha256d(tx, o, to_spend_txid);
    /* to_sign (unsigned): version 0, 1 input (to_spend:0, empty scriptSig, seq 0), 1 output (0, OP_RETURN), locktime 0 */
    o = 0;
    wr_le32(tx + o, 0); o += 4;
    tx[o++] = 1;
    memcpy(tx + o, to_spend_txid, 32); o += 32;
    wr_le32(tx + o, 0); o += 4;
    tx[o++] = 0;
    wr_le32(tx + o, 0); o += 4;
    tx[o++] = 1;
    wr_le64(tx + o, 0); o += 8;
    tx[o++] = 1; tx[o++] = 0x6a;
    wr_le32(tx + o, 0); o += 4;
    sha256d(tx, o, to_sign_txid);
}

void btcpsbt_bip322_digest(const uint8_t *msg, size_t len, const uint8_t pkh[20], uint8_t digest[32])
{
    uint8_t mh[32], spend_txid[32], sign_txid[32];
    btcpsbt_t p;
    btcpsbt_bip322_hashes(msg, len, pkh, mh, spend_txid, sign_txid);
    /* to_sign as a one-input, one-output P2WPKH spend of to_spend:0 with amount 0 */
    memset(&p, 0, sizeof p);
    p.version = 0;
    p.locktime = 0;
    p.n_in = 1;
    memcpy(p.in[0].txid, spend_txid, 32);
    p.in[0].vout = 0;
    p.in[0].sequence = 0;
    p.in[0].amount = 0;
    btc_p2wpkh_script(pkh, p.in[0].spk);
    p.in[0].spk_len = 22;
    p.in[0].has_witness_utxo = true;
    p.in[0].is_owner = true;
    p.in[0].sighash = BTCPSBT_SIGHASH_ALL;
    p.n_out = 1;
    p.out[0].amount = 0;
    p.out[0].spk[0] = 0x6a;
    p.out[0].spk_len = 1;
    btcpsbt_sighash(&p, NULL, 0, digest);
}

size_t btcpsbt_bip322_witness(const uint8_t sig[64], const uint8_t pub33[33], uint8_t *out, size_t cap)
{
    uint8_t der[SECP256K1_DER_MAX];
    size_t dl = secp256k1_der_encode(sig, der), o = 0;
    if (dl == 0 || cap < 1 + 1 + dl + 1 + 1 + 33) return 0;
    out[o++] = 0x02;              /* two stack items */
    out[o++] = (uint8_t)(dl + 1);
    memcpy(out + o, der, dl); o += dl;
    out[o++] = BTCPSBT_SIGHASH_ALL;
    out[o++] = 33;
    memcpy(out + o, pub33, 33); o += 33;
    return o;
}

const char *btcpsbt_danger_name(uint32_t bit)
{
    switch (bit) {
    case BTCPSBT_DANGER_HIGH_FEE: return "high_fee";
    case BTCPSBT_DANGER_UNKNOWN_CHANGE: return "unknown_change";
    case BTCPSBT_DANGER_SIGHASH_NOT_ALL: return "sighash_not_all";
    default: return NULL;
    }
}

uint32_t btcpsbt_danger_from_name(const char *name)
{
    uint32_t bit;
    if (!name) return 0;
    for (bit = 1; bit <= BTCPSBT_DANGER_ALL; bit <<= 1) {
        const char *n = btcpsbt_danger_name(bit);
        if (n && strcmp(n, name) == 0) return bit;
    }
    return 0;
}
