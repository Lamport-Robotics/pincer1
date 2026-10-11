#include "packet.h"
#include "aead.h"
#include <string.h>

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint16_t r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t r32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

void pkt_init(pkt_ctx_t *c, const uint8_t key[32], const uint8_t id[PKT_ID_LEN], uint8_t dir,
              uint32_t next_seq, uint32_t peer_seq) {
    memset(c, 0, sizeof *c);
    memcpy(c->key, key, 32);
    memcpy(c->id, id, PKT_ID_LEN);
    c->dir = dir ? 1 : 0;
    c->next_seq = next_seq ? next_seq : 1;
    c->peer_seq = peer_seq;
}

static void nonce_for(const pkt_ctx_t *c, uint32_t seq_with_dir, uint8_t nonce[12]) {
    memcpy(nonce, c->id, 8);
    w32(nonce + 8, seq_with_dir);
}

int pkt_encode(pkt_ctx_t *c, pkt_msg_t *m, uint8_t out[PKT_LEN]) {
    if (c->next_seq > PKT_SEQ_MAX) return 0;
    uint32_t seq = c->next_seq++;
    m->seq = seq;
    uint32_t swd = seq | (c->dir ? PKT_SEQ_DIR_BIT : 0);

    out[0] = PKT_VERSION;
    memcpy(out + 1, c->id, 8);
    out[9] = m->type;
    w32(out + 10, swd);

    uint8_t body[PKT_BODY_LEN];
    w16(body + 0, m->battery_mv);
    w32(body + 2, (uint32_t)m->lat_e7);
    w32(body + 6, (uint32_t)m->lon_e7);
    w16(body + 10, m->fix_age_s);
    body[12] = m->trig;
    body[13] = m->hdop_x10;
    w16(body + 14, m->findmy_idx);
    body[16] = m->state;
    body[17] = m->cmd;

    uint8_t nonce[12];
    nonce_for(c, swd, nonce);
    aead_encrypt(c->key, nonce, out, PKT_HDR_LEN, body, PKT_BODY_LEN, out + PKT_HDR_LEN, out + PKT_HDR_LEN + PKT_BODY_LEN);
    memset(body, 0, sizeof body);
    return 1;
}

int pkt_peek(const uint8_t *in, size_t len, uint8_t id_out[PKT_ID_LEN], uint8_t *type, uint32_t *seq_with_dir) {
    if (len < PKT_HDR_LEN) return 0;
    if (id_out) memcpy(id_out, in + 1, 8);
    if (type) *type = in[9];
    if (seq_with_dir) *seq_with_dir = r32(in + 10);
    return 1;
}

pkt_err_t pkt_decode(pkt_ctx_t *c, const uint8_t *in, size_t len, pkt_msg_t *out) {
    if (len != PKT_LEN) return PKT_ERR_LEN;
    if (in[0] != PKT_VERSION) return PKT_ERR_VERSION;
    if (memcmp(in + 1, c->id, 8) != 0) return PKT_ERR_ID;
    uint32_t swd = r32(in + 10);
    uint8_t from_base = (swd & PKT_SEQ_DIR_BIT) ? 1 : 0;
    if (from_base == c->dir) return PKT_ERR_DIR;
    uint8_t type = in[9];
    if (type < PKT_HEARTBEAT || type > PKT_CMD) return PKT_ERR_TYPE;

    uint8_t nonce[12];
    nonce_for(c, swd, nonce);
    uint8_t body[PKT_BODY_LEN];
    if (!aead_decrypt(c->key, nonce, in, PKT_HDR_LEN, in + PKT_HDR_LEN, PKT_BODY_LEN, in + PKT_HDR_LEN + PKT_BODY_LEN, body))
        return PKT_ERR_AUTH;

    uint32_t seq = swd & PKT_SEQ_MAX;
    if (seq == 0 || seq <= c->peer_seq) { memset(body, 0, sizeof body); return PKT_ERR_REPLAY; }
    c->peer_seq = seq;

    memset(out, 0, sizeof *out);
    out->type = type;
    out->seq = seq;
    out->battery_mv = r16(body + 0);
    out->lat_e7 = (int32_t)r32(body + 2);
    out->lon_e7 = (int32_t)r32(body + 6);
    out->fix_age_s = r16(body + 10);
    out->trig = body[12];
    out->hdop_x10 = body[13];
    out->findmy_idx = r16(body + 14);
    out->state = body[16];
    out->cmd = body[17];
    memset(body, 0, sizeof body);
    return PKT_OK;
}

const char *pkt_err_str(pkt_err_t e) {
    switch (e) {
        case PKT_OK: return "ok";
        case PKT_ERR_LEN: return "len";
        case PKT_ERR_VERSION: return "version";
        case PKT_ERR_ID: return "id";
        case PKT_ERR_DIR: return "dir";
        case PKT_ERR_AUTH: return "auth";
        case PKT_ERR_REPLAY: return "replay";
        case PKT_ERR_TYPE: return "type";
    }
    return "?";
}
const char *pkt_type_str(uint8_t t) {
    switch (t) { case PKT_HEARTBEAT: return "heartbeat"; case PKT_ALARM: return "alarm"; case PKT_ACK: return "ack"; case PKT_CMD: return "cmd"; }
    return "?";
}
const char *pkt_cmd_str(uint8_t c) {
    switch (c) { case PKT_CMD_NONE: return "none"; case PKT_CMD_ARM: return "arm"; case PKT_CMD_DISARM: return "disarm"; case PKT_CMD_PING: return "ping"; case PKT_CMD_OK: return "ok"; }
    return "?";
}
const char *pkt_state_str(uint8_t s) {
    switch (s) { case PKT_STATE_IDLE: return "idle"; case PKT_STATE_ARMED: return "armed"; case PKT_STATE_ALARM: return "alarm"; }
    return "?";
}
