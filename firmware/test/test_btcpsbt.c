#include "btcpsbt.h"

#include "bip32.h"
#include "chainaddr.h"
#include "harness.h"
#include "keygen.h"
#include "ripemd160.h"
#include "secp256k1.h"
#include "sha256.h"
#include "util.h"

/* Vector sources: BIP-143 "Native P2WPKH" example (sighash), BIP-322 basic-test-vectors.json (message
 * hash, to_spend / to_sign txids, deterministic "simple" signature for "Hello World"), BIP-84 keys for the
 * standard "abandon ... about" mnemonic. PSBT fixtures are built here byte by byte per BIP-174. */

/* ---- owner keys from the standard mnemonic ---- */
static uint8_t SEED[64];
static bip32_node_t LEAF;         /* m/84'/0'/0'/0/0 */
static uint8_t OWNER_PUB33[33], OWNER_PKH[20], OWNER_SPK[22];
static bip32_xpub_t ACCT;         /* m/84'/0'/0' */
static uint8_t CHANGE0_PUB33[33]; /* m/84'/0'/0'/1/0 */
static uint8_t CHANGE0_SPK[22];
static uint8_t DEST_SPK[22];      /* bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4 */

static void setup_keys(void)
{
    uint8_t pub[64], h[20];
    bip32_node_t acct;
    bip32_xpub_t br, leaf;
    keygen_seed_from_mnemonic("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", "", SEED);
    CHECK(bip32_derive(SEED, BIP32_PATH_BTC, 5, &LEAF));
    CHECK(secp256k1_pubkey(LEAF.key, pub));
    secp256k1_compress(pub, OWNER_PUB33);
    btc_pkh_from_pubkey(pub, OWNER_PKH);
    btc_p2wpkh_script(OWNER_PKH, OWNER_SPK);
    CHECK(bip32_derive(SEED, BIP32_PATH_BTC_ACCT, 3, &acct));
    CHECK(bip32_xpub(&acct, &ACCT));
    CHECK(bip32_ckd_pub(&ACCT, 1, &br));
    CHECK(bip32_ckd_pub(&br, 0, &leaf));
    memcpy(CHANGE0_PUB33, leaf.pub, 33);
    hash160(CHANGE0_PUB33, 33, h);
    btc_p2wpkh_script(h, CHANGE0_SPK);
    hex2bin("0014751e76e8199196d454941c45d1b3a323f1433bd6", DEST_SPK, 22);
}

/* ---- PSBT builder ---- */
typedef struct {
    uint8_t b[BTCPSBT_MAX_BYTES + 256];
    size_t n;
} pb_t;
static void pb_u8(pb_t *p, uint8_t v) { p->b[p->n++] = v; }
static void pb_bytes(pb_t *p, const void *d, size_t n) { memcpy(p->b + p->n, d, n); p->n += n; }
static void pb_u32(pb_t *p, uint32_t v) { wr_le32(p->b + p->n, v); p->n += 4; }
static void pb_u64(pb_t *p, uint64_t v) { wr_le64(p->b + p->n, v); p->n += 8; }
static void pb_varint(pb_t *p, uint64_t v)
{
    if (v < 0xfd) pb_u8(p, (uint8_t)v);
    else if (v <= 0xffff) { pb_u8(p, 0xfd); wr_le16(p->b + p->n, (uint16_t)v); p->n += 2; }
    else { pb_u8(p, 0xfe); pb_u32(p, (uint32_t)v); }
}
/* key-value entry */
static void pb_kv(pb_t *p, const uint8_t *key, size_t kl, const uint8_t *val, size_t vl)
{
    pb_varint(p, kl); pb_bytes(p, key, kl); pb_varint(p, vl); pb_bytes(p, val, vl);
}
static void pb_sep(pb_t *p) { pb_u8(p, 0); }

typedef struct { uint8_t txid[32]; uint32_t vout, seq; uint64_t amount; const uint8_t *spk; uint8_t spk_len;
                 const uint8_t *pub; const uint32_t *path; uint8_t path_len; uint32_t sighash; bool no_utxo; } fin_t;
typedef struct { uint64_t amount; const uint8_t *spk; uint8_t spk_len; const uint8_t *pub; const uint32_t *path; uint8_t path_len; } fout_t;

static void derivation_kv(pb_t *p, uint8_t type, const uint8_t *pub, const uint32_t *path, uint8_t path_len)
{
    uint8_t key[34], val[4 + 5 * 4];
    size_t i;
    key[0] = type;
    memcpy(key + 1, pub, 33);
    memset(val, 0xd3, 4); /* fingerprint: not checked */
    for (i = 0; i < path_len; i++) wr_le32(val + 4 + 4 * i, path[i]);
    pb_kv(p, key, 34, val, 4 + 4 * path_len);
}

static void build_psbt(pb_t *p, int32_t version, uint32_t locktime, const fin_t *in, uint8_t nin, const fout_t *out, uint8_t nout)
{
    pb_t tx;
    uint8_t i;
    static const uint8_t magic[5] = {0x70, 0x73, 0x62, 0x74, 0xff};
    static const uint8_t k_tx = 0x00;
    tx.n = 0;
    pb_u32(&tx, (uint32_t)version);
    pb_varint(&tx, nin);
    for (i = 0; i < nin; i++) { pb_bytes(&tx, in[i].txid, 32); pb_u32(&tx, in[i].vout); pb_varint(&tx, 0); pb_u32(&tx, in[i].seq); }
    pb_varint(&tx, nout);
    for (i = 0; i < nout; i++) { pb_u64(&tx, out[i].amount); pb_varint(&tx, out[i].spk_len); pb_bytes(&tx, out[i].spk, out[i].spk_len); }
    pb_u32(&tx, locktime);
    p->n = 0;
    pb_bytes(p, magic, 5);
    pb_kv(p, &k_tx, 1, tx.b, tx.n);
    pb_sep(p);
    for (i = 0; i < nin; i++) {
        if (!in[i].no_utxo) {
            uint8_t val[8 + 1 + BTCPSBT_MAX_SCRIPT];
            static const uint8_t k = 0x01;
            wr_le64(val, in[i].amount); val[8] = in[i].spk_len; memcpy(val + 9, in[i].spk, in[i].spk_len);
            pb_kv(p, &k, 1, val, 9 + in[i].spk_len);
        }
        if (in[i].sighash) {
            static const uint8_t k = 0x03;
            uint8_t v[4];
            wr_le32(v, in[i].sighash);
            pb_kv(p, &k, 1, v, 4);
        }
        if (in[i].pub) derivation_kv(p, 0x06, in[i].pub, in[i].path, in[i].path_len);
        pb_sep(p);
    }
    for (i = 0; i < nout; i++) {
        if (out[i].pub) derivation_kv(p, 0x02, out[i].pub, out[i].path, out[i].path_len);
        pb_sep(p);
    }
}

static const uint32_t PATH_RECV0[5] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0, 0};
static const uint32_t PATH_CHANGE0[5] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED, 1, 0};

/* the standard fixture: two owner inputs (60000 + 40000), dest 50000, change 48000, fee 2000 */
static void std_fixture(fin_t in[2], fout_t out[2])
{
    memset(in, 0, 2 * sizeof *in);
    memset(out, 0, 2 * sizeof *out);
    memset(in[0].txid, 0xa1, 32); in[0].vout = 1; in[0].seq = 0xfffffffd; in[0].amount = 60000; in[0].spk = OWNER_SPK; in[0].spk_len = 22;
    in[0].pub = OWNER_PUB33; in[0].path = PATH_RECV0; in[0].path_len = 5;
    memset(in[1].txid, 0xb2, 32); in[1].vout = 0; in[1].seq = 0xfffffffd; in[1].amount = 40000; in[1].spk = OWNER_SPK; in[1].spk_len = 22;
    in[1].pub = OWNER_PUB33; in[1].path = PATH_RECV0; in[1].path_len = 5;
    out[0].amount = 50000; out[0].spk = DEST_SPK; out[0].spk_len = 22;
    out[1].amount = 48000; out[1].spk = CHANGE0_SPK; out[1].spk_len = 22; out[1].pub = CHANGE0_PUB33; out[1].path = PATH_CHANGE0; out[1].path_len = 5;
}

static void test_parse_and_analyze(void)
{
    pb_t p;
    btcpsbt_t t;
    fin_t in[2];
    fout_t out[2];
    char addr[BTC_ADDR_MAX];
    std_fixture(in, out);
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(t.version, 2);
    CHECK_EQ(t.n_in, 2); CHECK_EQ(t.n_out, 2);
    CHECK_U64(t.in[0].amount, 60000); CHECK_U64(t.in[1].amount, 40000);
    CHECK_EQ(t.in[0].vout, 1); CHECK_EQ(t.in[0].sequence, 0xfffffffd);
    CHECK(t.in[0].has_witness_utxo && t.in[1].has_witness_utxo);
    CHECK(t.in[0].has_derivation && t.in[0].path_len == 5 && t.in[0].path[4] == 0 && t.in[0].path[3] == 0);
    CHECK_MEM(t.in[0].pubkey, OWNER_PUB33, 33);
    CHECK_U64(t.out[0].amount, 50000);
    CHECK_MEM(t.out[0].spk, DEST_SPK, 22);
    CHECK(t.out[1].has_derivation && t.out[1].path[3] == 1);
    CHECK(t.in[0].map_sep > t.in[0].map_off && t.in[1].map_off > t.in[0].map_sep);
    CHECK(btcpsbt_looks_like_psbt(p.b, p.n));
    CHECK(!btcpsbt_looks_like_psbt((const uint8_t *)"psbt", 4));
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_EQ(t.n_owner_inputs, 2);
    CHECK_U64(t.sats_in, 100000);
    CHECK_U64(t.sats_out, 50000);
    CHECK_U64(t.sats_change, 48000);
    CHECK_U64(t.fee, 2000);
    CHECK(t.in[0].is_owner && t.in[1].is_owner);
    CHECK(!t.out[0].is_change && t.out[1].is_change);
    CHECK(t.has_dest);
    CHECK_EQ(t.n_non_change, 1);
    CHECK(btc_script_to_address(t.dest_spk, t.dest_spk_len, addr));
    CHECK_STR(addr, "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4");
    CHECK_EQ(t.dangerous, 0);
    CHECK_EQ(t.in[0].sighash, BTCPSBT_SIGHASH_ALL);
    /* without the account xpub, change at 1/0 is not recognised -> it counts as leaving */
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, NULL), BTCPSBT_OK);
    CHECK(!t.out[1].is_change);
    CHECK_U64(t.sats_out, 98000);
    CHECK_EQ(t.n_non_change, 2);
    /* change claimed at 1/0 but with a pubkey that is not the derived one -> not change */
    out[1].pub = OWNER_PUB33;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK(!t.out[1].is_change);
    /* right pubkey, wrong script -> not change */
    out[1].pub = CHANGE0_PUB33; out[1].spk = DEST_SPK;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK(!t.out[1].is_change);
    /* paying our own receive address is change too, even without derivation */
    out[1].spk = OWNER_SPK; out[1].pub = NULL;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK(t.out[1].is_change);
    /* change index beyond the bound is not derived */
    {
        static const uint32_t far[5] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED, 1, BTCPSBT_MAX_CHANGE_INDEX};
        out[1].spk = CHANGE0_SPK; out[1].pub = CHANGE0_PUB33; out[1].path = far;
        build_psbt(&p, 2, 0, in, 2, out, 2);
        CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
        CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
        CHECK(!t.out[1].is_change);
    }
}

static void test_analysis_refusals_and_flags(void)
{
    pb_t p;
    btcpsbt_t t;
    fin_t in[2];
    fout_t out[2];
    uint8_t other_pub[33];
    std_fixture(in, out);
    /* foreign input: derivation names another key */
    memcpy(other_pub, CHANGE0_PUB33, 33);
    in[1].pub = other_pub;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_FOREIGN_INPUT);
    /* owner key but the utxo pays someone else */
    std_fixture(in, out);
    in[0].spk = DEST_SPK;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_FOREIGN_INPUT);
    /* owner key at a path we do not hold (0/1) */
    {
        static const uint32_t recv1[5] = {84u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0u | BIP32_HARDENED, 0, 1};
        std_fixture(in, out);
        in[0].path = recv1;
        build_psbt(&p, 2, 0, in, 2, out, 2);
        CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
        CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_FOREIGN_INPUT);
    }
    /* no derivation at all */
    std_fixture(in, out);
    in[0].pub = NULL;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_FOREIGN_INPUT);
    /* missing witness_utxo */
    std_fixture(in, out);
    in[1].no_utxo = true;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_NO_UTXO);
    /* outputs exceed inputs */
    std_fixture(in, out);
    out[0].amount = 60001;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_AMOUNTS);
    /* SIGHASH_NONE is never signed; SINGLE beyond the outputs neither */
    std_fixture(in, out);
    in[0].sighash = BTCPSBT_SIGHASH_NONE;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_SIGHASH);
    std_fixture(in, out);
    in[1].sighash = BTCPSBT_SIGHASH_SINGLE;
    build_psbt(&p, 2, 0, in, 2, out, 1); /* input 1, only 1 output */
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_E_SIGHASH);
    /* SINGLE / ANYONECANPAY are flagged */
    std_fixture(in, out);
    in[1].sighash = BTCPSBT_SIGHASH_SINGLE;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_EQ(t.dangerous, BTCPSBT_DANGER_SIGHASH_NOT_ALL);
    std_fixture(in, out);
    in[0].sighash = BTCPSBT_SIGHASH_ALL | BTCPSBT_SIGHASH_ANYONECANPAY;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_EQ(t.dangerous, BTCPSBT_DANGER_SIGHASH_NOT_ALL);
    /* high fee: 90000 of 100000 leaves as fee */
    std_fixture(in, out);
    out[0].amount = 10000;
    build_psbt(&p, 2, 0, in, 2, out, 1);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_U64(t.fee, 90000);
    CHECK_EQ(t.dangerous, BTCPSBT_DANGER_HIGH_FEE);
    /* exactly under 10 %: 50000 out, fee 4999 -> fine; 5000 -> high */
    std_fixture(in, out);
    out[1].amount = 45001;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_U64(t.fee, 4999);
    CHECK_EQ(t.dangerous, 0);
    out[1].amount = 45000;
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_EQ(t.dangerous, BTCPSBT_DANGER_HIGH_FEE);
    /* consolidation: everything is change, any fee is "high" (nothing leaves) */
    std_fixture(in, out);
    out[0] = out[1]; out[0].amount = 99000;
    build_psbt(&p, 2, 0, in, 2, out, 1);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_U64(t.sats_out, 0);
    CHECK(!t.has_dest);
    CHECK_EQ(t.dangerous, BTCPSBT_DANGER_HIGH_FEE);
    /* zero fee consolidation is fine */
    out[0].amount = 100000;
    build_psbt(&p, 2, 0, in, 2, out, 1);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK_EQ(t.dangerous, 0);
}

static void test_parse_rejections(void)
{
    pb_t p;
    btcpsbt_t t;
    fin_t in[9];
    fout_t out[2];
    size_t i;
    std_fixture(in, out);
    build_psbt(&p, 2, 0, in, 2, out, 2);
    /* magic */
    p.b[4] = 0xfe;
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_E_MAGIC);
    p.b[4] = 0xff;
    /* truncated / trailing */
    CHECK_EQ(btcpsbt_parse(p.b, p.n - 1, &t), BTCPSBT_E_FORMAT);
    p.b[p.n] = 0;
    CHECK_EQ(btcpsbt_parse(p.b, p.n + 1, &t), BTCPSBT_E_FORMAT);
    CHECK_EQ(btcpsbt_parse(p.b, 3, &t), BTCPSBT_E_MAGIC);
    CHECK_EQ(btcpsbt_parse(p.b, BTCPSBT_MAX_BYTES + 1, &t), BTCPSBT_E_SIZE);
    /* PSBT v2 marker */
    {
        pb_t q;
        static const uint8_t magic[5] = {0x70, 0x73, 0x62, 0x74, 0xff};
        static const uint8_t k = 0xfb;
        uint8_t v[4] = {2, 0, 0, 0};
        q.n = 0;
        pb_bytes(&q, magic, 5);
        pb_kv(&q, &k, 1, v, 4);
        pb_bytes(&q, p.b + 5, p.n - 5);
        CHECK_EQ(btcpsbt_parse(q.b, q.n, &t), BTCPSBT_E_VERSION);
        v[0] = 0;
        q.n = 0;
        pb_bytes(&q, magic, 5);
        pb_kv(&q, &k, 1, v, 4);
        pb_bytes(&q, p.b + 5, p.n - 5);
        CHECK_EQ(btcpsbt_parse(q.b, q.n, &t), BTCPSBT_OK);
    }
    /* no unsigned tx */
    {
        pb_t q;
        static const uint8_t magic[5] = {0x70, 0x73, 0x62, 0x74, 0xff};
        q.n = 0;
        pb_bytes(&q, magic, 5);
        pb_sep(&q);
        CHECK_EQ(btcpsbt_parse(q.b, q.n, &t), BTCPSBT_E_FORMAT);
    }
    /* unsigned tx with a scriptSig (byte after vout count) */
    {
        /* tx layout: version(4) nin(1) txid(32) vout(4) scriptSig len(1) ... ; the tx starts at offset 5+1+1+varint */
        size_t txo = 5 + 1 + 1 + (p.b[7] == 0xfd ? 3 : 1);
        uint8_t save = p.b[txo + 4 + 1 + 32 + 4];
        p.b[txo + 4 + 1 + 32 + 4] = 1; /* claims a 1-byte scriptSig */
        CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_E_VERSION);
        p.b[txo + 4 + 1 + 32 + 4] = save;
        CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    }
    /* 9 inputs */
    for (i = 0; i < 9; i++) { in[i] = in[0]; memset(in[i].txid, (int)i, 32); }
    build_psbt(&p, 2, 0, in, 9, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_E_TOOMANY);
    build_psbt(&p, 2, 0, in, 8, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(t.n_in, 8);
    /* an already-finalised input is refused */
    {
        pb_t q;
        static const uint8_t k = 0x08;
        std_fixture(in, out);
        build_psbt(&q, 2, 0, in, 1, out, 2);
        /* append a final_scriptwitness entry to input 0: rebuild by inserting before its separator */
        CHECK_EQ(btcpsbt_parse(q.b, q.n, &t), BTCPSBT_OK);
        {
            size_t sep = t.in[0].map_sep;
            uint8_t v[2] = {0x00, 0x00};
            memmove(q.b + sep + 1 + 1 + 1 + 2, q.b + sep, q.n - sep);
            q.b[sep] = 1; q.b[sep + 1] = k; q.b[sep + 2] = 2; q.b[sep + 3] = v[0]; q.b[sep + 4] = v[1];
            q.n += 5;
            CHECK_EQ(btcpsbt_parse(q.b, q.n, &t), BTCPSBT_E_FORMAT);
        }
    }
}

static void test_bip143_sighash_vector(void)
{
    /* BIP-143 Native P2WPKH example, built as a parsed structure: input 1 is the P2WPKH one (6 BTC). */
    btcpsbt_t t;
    uint8_t exp[32], digest[32], priv[32], pub[64], sig[64], recid;
    memset(&t, 0, sizeof t);
    t.version = 1;
    t.locktime = 0x11;
    t.n_in = 2;
    hex2bin("fff7f7881a8099afa6940d42d1e7f6362bec38171ea3edf433541db4e4ad969f", t.in[0].txid, 32);
    t.in[0].vout = 0; t.in[0].sequence = 0xffffffee;
    hex2bin("ef51e1b804cc89d182d279655c3aa89e815b1b309fe287d9b2b55d57b90ec68a", t.in[1].txid, 32);
    t.in[1].vout = 1; t.in[1].sequence = 0xffffffff;
    t.in[1].amount = 600000000;
    hex2bin("00141d0f172a0ecb48aee1be1f2687d2963ae33f71a1", t.in[1].spk, 22);
    t.in[1].spk_len = 22; t.in[1].has_witness_utxo = true; t.in[1].is_owner = true; t.in[1].sighash = BTCPSBT_SIGHASH_ALL;
    t.n_out = 2;
    t.out[0].amount = 0x06b22c20; hex2bin("76a9148280b37df378db99f66f85c95a783a76ac7a6d5988ac", t.out[0].spk, 25); t.out[0].spk_len = 25;
    t.out[1].amount = 0x0d519390; hex2bin("76a9143bde42dbee7e4dbe6a21b2d50ce2f0167faa815988ac", t.out[1].spk, 25); t.out[1].spk_len = 25;
    CHECK(btcpsbt_sighash(&t, NULL, 1, digest));
    hex2bin("c37af31116d1b27caf68aae9e3ac82f1477929014d5b917657d0eb49478cb670", exp, 32);
    CHECK_MEM(digest, exp, 32);
    CHECK(!btcpsbt_sighash(&t, NULL, 0, digest)); /* not an owner input */
    CHECK(!btcpsbt_sighash(&t, NULL, 2, digest));
    /* sign it with the example key: our deterministic signature verifies against the example pubkey */
    hex2bin("619c335025c7f4012e556c2a58b2506e30b8511b53ade95ea316fd8c3286feb9", priv, 32);
    CHECK(secp256k1_pubkey(priv, pub));
    CHECK(secp256k1_sign(priv, digest, sig, &recid));
    CHECK(secp256k1_verify(pub, digest, sig));
    /* the published signature verifies over our digest too */
    {
        uint8_t der[70], ps[64];
        hex2bin("304402203609e17b84f6a7d30c80bfa610b5b4542f32a8a0d5447a12fb1366d7f01cc44a0220573a954c4518331561406f90300e8f3358f51928d43c212a8caed02de67eebee", der, 70);
        CHECK(secp256k1_der_decode(der, 70, ps));
        CHECK(secp256k1_verify(pub, digest, ps));
    }
    /* SINGLE and ANYONECANPAY change the preimage */
    {
        uint8_t d2[32], d3[32];
        t.in[1].sighash = BTCPSBT_SIGHASH_SINGLE;
        CHECK(btcpsbt_sighash(&t, NULL, 1, d2));
        CHECK(memcmp(d2, digest, 32) != 0);
        t.in[1].sighash = BTCPSBT_SIGHASH_ALL | BTCPSBT_SIGHASH_ANYONECANPAY;
        CHECK(btcpsbt_sighash(&t, NULL, 1, d3));
        CHECK(memcmp(d3, digest, 32) != 0 && memcmp(d3, d2, 32) != 0);
        t.in[1].sighash = BTCPSBT_SIGHASH_NONE;
        CHECK(!btcpsbt_sighash(&t, NULL, 1, d3));
    }
}

static void test_sign_and_insert(void)
{
    pb_t p;
    btcpsbt_t t, back;
    fin_t in[2];
    fout_t out[2];
    uint8_t sigs[2][64], digest[32], pub[64], recid;
    static uint8_t o[BTCPSBT_MAX_OUT_BYTES];
    size_t n, i, expect;
    std_fixture(in, out);
    build_psbt(&p, 2, 0, in, 2, out, 2);
    CHECK_EQ(btcpsbt_parse(p.b, p.n, &t), BTCPSBT_OK);
    CHECK_EQ(btcpsbt_analyze(&t, OWNER_PUB33, &ACCT), BTCPSBT_OK);
    CHECK(secp256k1_pubkey(LEAF.key, pub));
    for (i = 0; i < 2; i++) {
        CHECK(btcpsbt_sighash(&t, p.b, (uint8_t)i, digest));
        CHECK(secp256k1_sign(LEAF.key, digest, sigs[i], &recid));
        CHECK(secp256k1_verify(pub, digest, sigs[i]));
    }
    CHECK(memcmp(sigs[0], sigs[1], 64) != 0); /* different outpoints -> different digests */
    n = btcpsbt_insert_sigs(&t, p.b, p.n, (const uint8_t (*)[64])sigs, o, sizeof o);
    CHECK(n > p.n);
    expect = p.n;
    for (i = 0; i < 2; i++) {
        uint8_t der[SECP256K1_DER_MAX];
        expect += 1 + 34 + 1 + secp256k1_der_encode(sigs[i], der) + 1;
    }
    CHECK_EQ(n, expect);
    /* the result parses, keeps everything else, and carries our partial signatures right before each separator */
    CHECK_EQ(btcpsbt_parse(o, n, &back), BTCPSBT_OK);
    CHECK_EQ(back.n_in, 2);
    CHECK_MEM(back.in[0].pubkey, OWNER_PUB33, 33);
    CHECK_U64(back.in[1].amount, 40000);
    {
        /* locate the entry: 0x22 0x02 <pub33> <len> <der> 0x01 then 0x00 */
        size_t sep = back.in[0].map_sep;
        uint8_t der[SECP256K1_DER_MAX];
        size_t dl = secp256k1_der_encode(sigs[0], der);
        size_t at = sep - (1 + 34 + 1 + dl + 1);
        CHECK_EQ(o[at], 34);
        CHECK_EQ(o[at + 1], 0x02);
        CHECK_MEM(o + at + 2, OWNER_PUB33, 33);
        CHECK_EQ(o[at + 35], dl + 1);
        CHECK_MEM(o + at + 36, der, dl);
        CHECK_EQ(o[at + 36 + dl], 0x01);
        CHECK_EQ(o[sep], 0x00);
    }
    CHECK_EQ(btcpsbt_insert_sigs(&t, p.b, p.n, (const uint8_t (*)[64])sigs, o, p.n + 10), 0); /* too small */
    /* output section is byte-identical */
    CHECK_MEM(o + n - (p.n - t.outputs_off), p.b + t.outputs_off, p.n - t.outputs_off);
}

static void reverse32(uint8_t *p)
{
    int i;
    for (i = 0; i < 16; i++) { uint8_t x = p[i]; p[i] = p[31 - i]; p[31 - i] = x; }
}

static void test_bip322(void)
{
    uint8_t pkh_script[42], mh[32], spend[32], sign[32], exp[32], priv[32], pub[64], pub33[33], digest[32], sig[64], recid;
    uint8_t wit[128];
    char b64[200];
    size_t n;
    CHECK_EQ(btc_address_to_script("bc1q9vza2e8x573nczrlzms0wvx3gsqjx7vavgkx0l", pkh_script), 22);
    /* message "" */
    btcpsbt_bip322_hashes((const uint8_t *)"", 0, pkh_script + 2, mh, spend, sign);
    hex2bin("c90c269c4f8fcbe6880f72a721ddfbf1914268a794cbb21cfafee13770ae19f1", exp, 32);
    CHECK_MEM(mh, exp, 32);
    hex2bin("c5680aa69bb8d860bf82d4e9cd3504b55dde018de765a91bb566283c545a99a7", exp, 32); reverse32(exp); /* txids are displayed reversed */
    CHECK_MEM(spend, exp, 32);
    hex2bin("1e9654e951a5ba44c8604c4de6c67fd78a27e81dcadcfe1edf638ba3aaebaed6", exp, 32); reverse32(exp);
    CHECK_MEM(sign, exp, 32);
    /* message "Hello World" */
    btcpsbt_bip322_hashes((const uint8_t *)"Hello World", 11, pkh_script + 2, mh, spend, sign);
    hex2bin("f0eb03b1a75ac6d9847f55c624a99169b5dccba2a31f5b23bea77ba270de0a7a", exp, 32);
    CHECK_MEM(mh, exp, 32);
    hex2bin("b79d196740ad5217771c1098fc4a4b51e0535c32236c71f1ea4d61a2d603352b", exp, 32); reverse32(exp);
    CHECK_MEM(spend, exp, 32);
    hex2bin("88737ae86f2077145f93cc4b153ae9a1cb8d56afa511988c149c5c8c9d93bddf", exp, 32); reverse32(exp);
    CHECK_MEM(sign, exp, 32);
    /* the published "simple" signatures for L3VFeEuj...: each vector lists two, the second is the RFC 6979
     * one (the first carries Bitcoin Core's extra nonce entropy and is not reproducible) */
    hex2bin("bb051cd0dda0246f33c5a9e133ebd8e7bc02a92af6c41adc131ccd7826c5b004", priv, 32);
    CHECK(secp256k1_pubkey(priv, pub));
    secp256k1_compress(pub, pub33);
    {
        uint8_t h[20];
        hash160(pub33, 33, h);
        CHECK_MEM(h, pkh_script + 2, 20); /* the key really is that address */
    }
    btcpsbt_bip322_digest((const uint8_t *)"Hello World", 11, pkh_script + 2, digest);
    CHECK(secp256k1_sign(priv, digest, sig, &recid));
    n = btcpsbt_bip322_witness(sig, pub33, wit, sizeof wit);
    CHECK(n > 0);
    CHECK(base64_encode(wit, n, b64, sizeof b64) > 0);
    CHECK_STR(b64, "AkgwRQIhAOzyynlqt93lOKJr+wmmxIens//zPzl9tqIOua93wO6MAiBi5n5EyAcPScOjf1lAqIUIQtr3zKNeavYabHyR8eGhowEhAsfxIAMZZEKUPYWI4BruhAQjzFT8FSFSajuFwrDL1Yhy");
    /* "" */
    btcpsbt_bip322_digest((const uint8_t *)"", 0, pkh_script + 2, digest);
    CHECK(secp256k1_sign(priv, digest, sig, &recid));
    n = btcpsbt_bip322_witness(sig, pub33, wit, sizeof wit);
    CHECK(base64_encode(wit, n, b64, sizeof b64) > 0);
    CHECK_STR(b64, "AkgwRQIhAPkJ1Q4oYS0htvyuSFHLxRQpFAY56b70UvE7Dxazen0ZAiAtZfFz1S6T6I23MWI2lK/pcNTWncuyL8UL+oMdydVgzAEhAsfxIAMZZEKUPYWI4BruhAQjzFT8FSFSajuFwrDL1Yhy");
    CHECK_EQ(btcpsbt_bip322_witness(sig, pub33, wit, 10), 0);
}

static void test_danger_names(void)
{
    uint32_t bit;
    for (bit = 1; bit <= BTCPSBT_DANGER_ALL; bit <<= 1) {
        const char *n = btcpsbt_danger_name(bit);
        CHECK(n != NULL);
        CHECK_EQ(btcpsbt_danger_from_name(n), bit);
    }
    CHECK_EQ(btcpsbt_danger_from_name("nope"), 0);
}

int main(void)
{
    setup_keys();
    RUN(test_parse_and_analyze);
    RUN(test_analysis_refusals_and_flags);
    RUN(test_parse_rejections);
    RUN(test_bip143_sighash_vector);
    RUN(test_sign_and_insert);
    RUN(test_bip322);
    RUN(test_danger_names);
    TEST_END();
}
