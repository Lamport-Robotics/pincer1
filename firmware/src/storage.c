#include "storage.h"

#include <string.h>

#include "sha256.h"
#include "util.h"

/* Record: magic(4) version(2) len(2) seq(4) payload(len) crc32(4) mac(32); the crc covers everything before
 * it (cheap corruption check), the HMAC-SHA256 covers header, payload and crc under the storage key. */
#define HDR 12u
#define TAIL (4u + STORAGE_MAC_LEN)

static void seal(uint8_t *rec, size_t n, const uint8_t key[32])
{
    wr_le32(rec + n, crc32_buf(rec, n));
    hmac_sha256(key, 32, rec, n + 4, rec + n + 4);
}

/* crc then mac over the first `n` bytes; the mac compare is constant time. */
static bool unseal(const uint8_t *rec, size_t n, const uint8_t key[32])
{
    uint8_t mac[32];
    uint8_t diff = 0;
    size_t i;
    if (crc32_buf(rec, n) != rd_le32(rec + n)) return false;
    hmac_sha256(key, 32, rec, n + 4, mac);
    for (i = 0; i < 32; i++) diff |= mac[i] ^ rec[n + 4 + i];
    secure_zero(mac, sizeof mac);
    return diff == 0;
}

static size_t put_allowance(uint8_t *p, const consent_allowance_t *a)
{
    size_t o = 0;
    p[o++] = a->active ? 1 : 0;
    wr_le64(p + o, a->lamports); o += 8;
    wr_le64(p + o, a->spent); o += 8;
    wr_le64(p + o, a->max_per_tx); o += 8;
    wr_le64(p + o, a->until_unix); o += 8;
    p[o++] = a->n_programs;
    memcpy(p + o, a->programs, sizeof a->programs); o += sizeof a->programs;
    return o;
}

static size_t get_allowance(const uint8_t *p, consent_allowance_t *a)
{
    size_t o = 0;
    a->active = p[o++] != 0;
    a->lamports = rd_le64(p + o); o += 8;
    a->spent = rd_le64(p + o); o += 8;
    a->max_per_tx = rd_le64(p + o); o += 8;
    a->until_unix = rd_le64(p + o); o += 8;
    a->n_programs = p[o++];
    if (a->n_programs > CONSENT_ALLOWANCE_MAX_PROGRAMS) a->n_programs = 0;
    memcpy(a->programs, p + o, sizeof a->programs); o += sizeof a->programs;
    return o;
}

#define PAYLOAD_V3_LEN (1u + 1u + 8u + 8u + 4u + 64u + 64u + (1u + 32u + 1u + 8u * 32u) + 1u + 1u + 32u)
/* v4 appends: daily_limit_wei(32) spent_today_wei(32) daily_limit_sats(8) spent_today_sats(8) evm_addr(40)
 * btc_pkh(40) secp_blob(128); v8 dropped the account xpubs (change is the receive script only) */
#define PAYLOAD_V4_LEN (PAYLOAD_V3_LEN + 32u + 32u + 8u + 8u + 40u + 40u + 128u)
/* v5 appends: rollover_powered_seconds(8) n_routers(1) routers(8 x (chain 1 + addr 32)) */
#define PAYLOAD_V5_LEN (PAYLOAD_V4_LEN + 8u + 1u + ROUTERS_MAX_OWNER * 33u)
/* v6 appends: duress_pin_set(1) duress_hash(32) duress_salt(16); v7: shadow_valid(1) + shadow(96); v8: last_unix(8) */
#define PAYLOAD_LEN (PAYLOAD_V5_LEN + 1u + 32u + 16u + 1u + 96u + 8u)

size_t storage_encode(const storage_data_t *d, uint32_t seq, const uint8_t key[32], uint8_t *out, size_t cap)
{
    size_t o = 0, i;
    if (cap < HDR + PAYLOAD_LEN + TAIL) return 0;
    memset(out, 0xff, cap);
    wr_le32(out + o, STORAGE_MAGIC); o += 4;
    wr_le16(out + o, STORAGE_VERSION); o += 2;
    wr_le16(out + o, (uint16_t)PAYLOAD_LEN); o += 2;
    wr_le32(out + o, seq); o += 4;
    out[o++] = d->consent.initialized ? 1 : 0;
    out[o++] = d->consent.duress ? 1 : 0;
    wr_le64(out + o, d->consent.daily_limit); o += 8;
    wr_le64(out + o, d->consent.spent_today); o += 8;
    wr_le32(out + o, d->consent.day); o += 4;
    memcpy(out + o, d->pubkey, 64); o += 64;
    memcpy(out + o, d->signer_blob, 64); o += 64;
    o += put_allowance(out + o, &d->consent.allowance);
    out[o++] = d->consent.pin_set ? 1 : 0;
    out[o++] = d->consent.pin_failures;
    memcpy(out + o, d->pin_hash, 32); o += 32;
    u256_to_be32(&d->consent.daily_limit_wei, out + o); o += 32;
    u256_to_be32(&d->consent.spent_today_wei, out + o); o += 32;
    wr_le64(out + o, d->consent.daily_limit_sats); o += 8;
    wr_le64(out + o, d->consent.spent_today_sats); o += 8;
    memcpy(out + o, d->evm_addr, 40); o += 40;
    memcpy(out + o, d->btc_pkh, 40); o += 40;
    memcpy(out + o, d->secp_blob, 128); o += 128;
    wr_le64(out + o, d->consent.rollover_powered_seconds); o += 8;
    out[o++] = d->consent.routers.n;
    for (i = 0; i < ROUTERS_MAX_OWNER; i++) {
        out[o++] = d->consent.routers.e[i].chain;
        memcpy(out + o, d->consent.routers.e[i].addr, 32); o += 32;
    }
    out[o++] = d->consent.duress_pin_set ? 1 : 0;
    memcpy(out + o, d->duress_hash, 32); o += 32;
    memcpy(out + o, d->duress_salt, 16); o += 16;
    out[o++] = d->consent.shadow_valid ? 1 : 0;
    wr_le64(out + o, d->consent.shadow.daily_limit); o += 8;
    wr_le64(out + o, d->consent.shadow.spent_today); o += 8;
    u256_to_be32(&d->consent.shadow.daily_limit_wei, out + o); o += 32;
    u256_to_be32(&d->consent.shadow.spent_today_wei, out + o); o += 32;
    wr_le64(out + o, d->consent.shadow.daily_limit_sats); o += 8;
    wr_le64(out + o, d->consent.shadow.spent_today_sats); o += 8;
    wr_le64(out + o, d->consent.last_unix); o += 8;
    seal(out, o, key);
    return o + TAIL;
}

bool storage_decode(const uint8_t *in, size_t len, const uint8_t key[32], storage_data_t *d, uint32_t *seq)
{
    size_t o = 0, i;
    uint16_t ver, plen;
    memset(d, 0, sizeof *d);
    if (len < HDR + TAIL) return false;
    if (rd_le32(in) != STORAGE_MAGIC) return false;
    ver = rd_le16(in + 4);
    plen = rd_le16(in + 6);
    if (ver != STORAGE_VERSION || plen != PAYLOAD_LEN) return false;
    if (HDR + plen + TAIL > len) return false;
    if (!unseal(in, HDR + plen, key)) return false;
    *seq = rd_le32(in + 8);
    o = HDR;
    d->consent.initialized = in[o++] != 0;
    d->consent.duress = in[o++] != 0;
    d->consent.daily_limit = rd_le64(in + o); o += 8;
    d->consent.spent_today = rd_le64(in + o); o += 8;
    d->consent.day = rd_le32(in + o); o += 4;
    memcpy(d->pubkey, in + o, 64); o += 64;
    memcpy(d->signer_blob, in + o, 64); o += 64;
    o += get_allowance(in + o, &d->consent.allowance);
    d->consent.pin_set = in[o++] != 0;
    d->consent.pin_failures = in[o++];
    memcpy(d->pin_hash, in + o, 32); o += 32;
    u256_from_be(&d->consent.daily_limit_wei, in + o, 32); o += 32;
    u256_from_be(&d->consent.spent_today_wei, in + o, 32); o += 32;
    d->consent.daily_limit_sats = rd_le64(in + o); o += 8;
    d->consent.spent_today_sats = rd_le64(in + o); o += 8;
    memcpy(d->evm_addr, in + o, 40); o += 40;
    memcpy(d->btc_pkh, in + o, 40); o += 40;
    memcpy(d->secp_blob, in + o, 128); o += 128;
    d->consent.rollover_powered_seconds = rd_le64(in + o); o += 8;
    d->consent.routers.n = in[o++];
    for (i = 0; i < ROUTERS_MAX_OWNER; i++) {
        d->consent.routers.e[i].chain = in[o++];
        memcpy(d->consent.routers.e[i].addr, in + o, 32); o += 32;
    }
    if (d->consent.routers.n > ROUTERS_MAX_OWNER) d->consent.routers.n = 0;
    d->consent.duress_pin_set = in[o++] != 0;
    memcpy(d->duress_hash, in + o, 32); o += 32;
    memcpy(d->duress_salt, in + o, 16); o += 16;
    d->consent.shadow_valid = in[o++] != 0;
    d->consent.shadow.daily_limit = rd_le64(in + o); o += 8;
    d->consent.shadow.spent_today = rd_le64(in + o); o += 8;
    u256_from_be(&d->consent.shadow.daily_limit_wei, in + o, 32); o += 32;
    u256_from_be(&d->consent.shadow.spent_today_wei, in + o, 32); o += 32;
    d->consent.shadow.daily_limit_sats = rd_le64(in + o); o += 8;
    d->consent.shadow.spent_today_sats = rd_le64(in + o); o += 8;
    d->consent.last_unix = rd_le64(in + o); o += 8;
    return true;
}

/* ---- double-slot engine shared by both regions ---- */

typedef bool (*decode_fn)(const uint8_t *in, size_t len, const uint8_t key[32], void *d, uint32_t *seq);

static void slots_init(storage_t *s, const storage_flash_t *fl)
{
    s->fl = fl;
    s->seq = 0;
    s->newest_slot = -1;
    memset(s->key, 0, sizeof s->key);
}

void storage_set_key(storage_t *s, const uint8_t key[32]) { memcpy(s->key, key, 32); }

static bool slots_load(storage_t *s, uint8_t *buf, size_t rec, void *d, void *cand, size_t dsize, decode_fn dec)
{
    uint32_t seq;
    int slot;
    bool found = false;
    memset(d, 0, dsize);
    s->newest_slot = -1;
    s->seq = 0;
    for (slot = 0; slot < 2; slot++) {
        if (s->fl->read((uint32_t)slot * STORAGE_SECTOR_SIZE, buf, rec) != 0) continue;
        if (!dec(buf, rec, s->key, cand, &seq)) continue;
        if (!found || (int32_t)(seq - s->seq) > 0) {
            memcpy(d, cand, dsize);
            s->seq = seq;
            s->newest_slot = slot;
            found = true;
        }
    }
    secure_zero(buf, rec);
    secure_zero(cand, dsize);
    return found;
}

/* `buf` already holds the encoded record (rec bytes). */
static int slots_save(storage_t *s, uint8_t *buf, uint8_t *check, size_t rec, uint32_t seq)
{
    int target = (s->newest_slot == 0) ? 1 : 0;
    uint32_t off = (uint32_t)target * STORAGE_SECTOR_SIZE;
    int rc = -1;
    if (s->fl->erase_sector(off) != 0) goto out;
    if (s->fl->program(off, buf, rec) != 0) goto out;
    if (s->fl->read(off, check, rec) != 0) goto out;
    if (memcmp(buf, check, rec) != 0) goto out;
    s->seq = seq;
    s->newest_slot = target;
    rc = 0;
out:
    secure_zero(buf, rec);
    secure_zero(check, rec);
    return rc;
}

static bool decode_main(const uint8_t *in, size_t len, const uint8_t key[32], void *d, uint32_t *seq) { return storage_decode(in, len, key, d, seq); }
static bool decode_inh(const uint8_t *in, size_t len, const uint8_t key[32], void *d, uint32_t *seq) { return storage_inh_decode(in, len, key, d, seq); }

void storage_init(storage_t *s, const storage_flash_t *fl) { slots_init(s, fl); }

bool storage_load(storage_t *s, storage_data_t *d)
{
    uint8_t buf[STORAGE_RECORD_SIZE];
    storage_data_t cand;
    return slots_load(s, buf, sizeof buf, d, &cand, sizeof cand, decode_main);
}

int storage_save(storage_t *s, const storage_data_t *d)
{
    uint8_t buf[STORAGE_RECORD_SIZE];
    uint8_t check[STORAGE_RECORD_SIZE];
    uint32_t seq = s->seq + 1;
    if (storage_encode(d, seq, s->key, buf, sizeof buf) == 0) return -1;
    return slots_save(s, buf, check, sizeof buf, seq);
}

/* ---- inheritance region ----
 * Record: magic(4) version(2) len(2) seq(4) | powered_seconds(8) last_touch(8) armed(1) inactivity_days(2)
 * n_warn(1) warn_days(4x2) warned(1) heir(32) signer(32) msg_len(2) msg(860) sig(64) | crc32(4) mac(32) = 1067 bytes */
#define INH_PAYLOAD_LEN (8u + 8u + 1u + 2u + 1u + 2u * CONSENT_INH_MAX_WARN + 1u + 32u + 32u + 2u + CONSENT_INH_MSG_MAX + 64u)

size_t storage_inh_encode(const storage_inh_data_t *d, uint32_t seq, const uint8_t key[32], uint8_t *out, size_t cap)
{
    size_t o = 0;
    uint8_t i;
    if (cap < HDR + INH_PAYLOAD_LEN + TAIL) return 0;
    if (d->msg_len > CONSENT_INH_MSG_MAX || d->ih.inh.n_warn > CONSENT_INH_MAX_WARN) return 0;
    memset(out, 0xff, cap);
    wr_le32(out + o, STORAGE_INH_MAGIC); o += 4;
    wr_le16(out + o, STORAGE_INH_VERSION); o += 2;
    wr_le16(out + o, (uint16_t)INH_PAYLOAD_LEN); o += 2;
    wr_le32(out + o, seq); o += 4;
    wr_le64(out + o, d->ih.powered_seconds); o += 8;
    wr_le64(out + o, d->ih.last_touch_powered_seconds); o += 8;
    out[o++] = d->ih.inh.armed ? 1 : 0;
    wr_le16(out + o, d->ih.inh.inactivity_days); o += 2;
    out[o++] = d->ih.inh.n_warn;
    for (i = 0; i < CONSENT_INH_MAX_WARN; i++) { wr_le16(out + o, d->ih.inh.warn_days[i]); o += 2; }
    out[o++] = d->ih.inh.warned;
    memcpy(out + o, d->ih.inh.heir, 32); o += 32;
    memcpy(out + o, d->ih.inh.signer, 32); o += 32;
    wr_le16(out + o, d->msg_len); o += 2;
    memset(out + o, 0, CONSENT_INH_MSG_MAX);
    memcpy(out + o, d->msg, d->msg_len); o += CONSENT_INH_MSG_MAX;
    memcpy(out + o, d->sig, 64); o += 64;
    seal(out, o, key);
    return o + TAIL;
}

bool storage_inh_decode(const uint8_t *in, size_t len, const uint8_t key[32], storage_inh_data_t *d, uint32_t *seq)
{
    size_t o;
    uint16_t ver, plen;
    uint8_t i;
    memset(d, 0, sizeof *d);
    if (len < HDR + TAIL) return false;
    if (rd_le32(in) != STORAGE_INH_MAGIC) return false;
    ver = rd_le16(in + 4);
    plen = rd_le16(in + 6);
    if (ver != STORAGE_INH_VERSION || plen != INH_PAYLOAD_LEN) return false;
    if (HDR + plen + TAIL > len) return false;
    if (!unseal(in, HDR + plen, key)) return false;
    *seq = rd_le32(in + 8);
    o = HDR;
    d->ih.powered_seconds = rd_le64(in + o); o += 8;
    d->ih.last_touch_powered_seconds = rd_le64(in + o); o += 8;
    d->ih.inh.armed = in[o++] != 0;
    d->ih.inh.inactivity_days = rd_le16(in + o); o += 2;
    d->ih.inh.n_warn = in[o++];
    for (i = 0; i < CONSENT_INH_MAX_WARN; i++) { d->ih.inh.warn_days[i] = rd_le16(in + o); o += 2; }
    d->ih.inh.warned = in[o++];
    memcpy(d->ih.inh.heir, in + o, 32); o += 32;
    memcpy(d->ih.inh.signer, in + o, 32); o += 32;
    d->msg_len = rd_le16(in + o); o += 2;
    memcpy(d->msg, in + o, CONSENT_INH_MSG_MAX); o += CONSENT_INH_MSG_MAX;
    memcpy(d->sig, in + o, 64); o += 64;
    if (d->msg_len > CONSENT_INH_MSG_MAX || d->ih.inh.n_warn > CONSENT_INH_MAX_WARN) {
        memset(d, 0, sizeof *d);
        return false;
    }
    if (d->ih.inh.armed && d->msg_len == 0) d->ih.inh.armed = false; /* armed without a tx is meaningless */
    return true;
}

void storage_inh_init(storage_t *s, const storage_flash_t *fl) { slots_init(s, fl); }

bool storage_inh_load(storage_t *s, storage_inh_data_t *d)
{
    uint8_t buf[STORAGE_INH_RECORD_SIZE];
    storage_inh_data_t cand;
    return slots_load(s, buf, sizeof buf, d, &cand, sizeof cand, decode_inh);
}

int storage_inh_save(storage_t *s, const storage_inh_data_t *d)
{
    uint8_t buf[STORAGE_INH_RECORD_SIZE];
    uint8_t check[STORAGE_INH_RECORD_SIZE];
    uint32_t seq = s->seq + 1;
    if (storage_inh_encode(d, seq, s->key, buf, sizeof buf) == 0) return -1;
    return slots_save(s, buf, check, sizeof buf, seq);
}
