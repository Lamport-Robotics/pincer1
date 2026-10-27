// Beacon ⇄ base LoRa packet. Pure, host-tested (test/test_packet).
//
// Wire format, 48 bytes, little-endian integers:
//
//   header, 14 bytes, sent in clear and authenticated (AEAD associated data):
//     0      version        = 2
//     1..8   pseudonym      8 bytes: SHA-256(key ‖ device id ‖ "pincer1-pid" ‖ seq-with-direction)[0..8].
//                           Changes with every packet, so a listener without the key cannot tell this
//                           beacon's packets from any other's; the device id itself never goes on air.
//     9      type           1 heartbeat · 2 alarm · 3 ack · 4 cmd
//     10..13 seq            u32; bit 31 = direction (0 beacon→base, 1 base→beacon), bits 0..30 count
//   body, 18 bytes, encrypted:
//     14..15 battery_mv     u16
//     16..19 lat            i32, 1e-7 degrees (0 = unknown)
//     20..23 lon            i32, 1e-7 degrees
//     24..25 fix_age_s      u16 seconds since the fix (0xFFFF = never had one)
//     26     trigger bits   bit0 ctrl · bit1 robot · bit2 tamper · bit3 motion · bit4 low battery
//     27     hdop_x10       u8 (0xFF = unknown)
//     28..29 findmy_idx     u16, Find My key index currently advertised
//     30     state          0 idle · 1 armed · 2 alarm
//     31     cmd            for type cmd/ack: 1 arm · 2 disarm · 3 ping · 4 ok
//   tag, 16 bytes, Poly1305
//
// ChaCha20-Poly1305 (RFC 8439) under the 32-byte shared key; nonce = device id ‖ seq (12 bytes),
// with the real device id, which both sides know from provisioning. Each direction has its own
// counter (the direction bit keeps the two nonce spaces apart; the same key is used both ways).
// Replay protection: a receiver accepts only a seq strictly greater than the highest it has accepted
// from that direction. Counters must be persisted by both sides. The seq is the one value in clear
// that moves predictably; it is 31 bits of counter, not an identity.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PKT_VERSION 2
#define PKT_ID_LEN  8
#define PKT_HDR_LEN 14
#define PKT_BODY_LEN 18
#define PKT_TAG_LEN 16
#define PKT_LEN     (PKT_HDR_LEN + PKT_BODY_LEN + PKT_TAG_LEN)   // 48

#define PKT_SEQ_DIR_BIT 0x80000000u
#define PKT_SEQ_MAX     0x7fffffffu

#define PKT_FIX_AGE_NEVER 0xffff
#define PKT_HDOP_UNKNOWN  0xff

typedef enum { PKT_HEARTBEAT = 1, PKT_ALARM = 2, PKT_ACK = 3, PKT_CMD = 4 } pkt_type_t;
typedef enum { PKT_CMD_NONE = 0, PKT_CMD_ARM = 1, PKT_CMD_DISARM = 2, PKT_CMD_PING = 3, PKT_CMD_OK = 4 } pkt_cmd_t;
typedef enum { PKT_STATE_IDLE = 0, PKT_STATE_ARMED = 1, PKT_STATE_ALARM = 2 } pkt_state_t;

enum {
    PKT_TRIG_CTRL   = 1 << 0,
    PKT_TRIG_ROBOT  = 1 << 1,
    PKT_TRIG_TAMPER = 1 << 2,
    PKT_TRIG_MOTION = 1 << 3,
    PKT_TRIG_LOWBAT = 1 << 4,
};

typedef enum {
    PKT_OK = 0,
    PKT_ERR_LEN,       // not 48 bytes
    PKT_ERR_VERSION,
    PKT_ERR_ID,        // pseudonym does not match our key + device id
    PKT_ERR_DIR,       // a packet from our own direction (echo)
    PKT_ERR_AUTH,      // tag mismatch: wrong key or corrupted
    PKT_ERR_REPLAY,    // seq not greater than the last accepted
    PKT_ERR_TYPE,
} pkt_err_t;

typedef struct {
    uint8_t  type;        // pkt_type_t
    uint32_t seq;         // filled by encode (counter only, without the direction bit)
    uint16_t battery_mv;
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint16_t fix_age_s;
    uint8_t  trig;
    uint8_t  hdop_x10;
    uint16_t findmy_idx;
    uint8_t  state;       // pkt_state_t
    uint8_t  cmd;         // pkt_cmd_t
} pkt_msg_t;

typedef struct {
    uint8_t  key[32];
    uint8_t  id[PKT_ID_LEN];
    uint8_t  dir;         // 0 = we are the beacon, 1 = we are the base
    uint32_t next_seq;    // our next counter value (0..PKT_SEQ_MAX)
    uint32_t peer_seq;    // highest counter accepted from the peer; 0 = none yet (counters start at 1)
} pkt_ctx_t;

// dir: 0 beacon, 1 base. next_seq is clamped to ≥ 1.
void pkt_init(pkt_ctx_t *c, const uint8_t key[32], const uint8_t id[PKT_ID_LEN], uint8_t dir,
              uint32_t next_seq, uint32_t peer_seq);

// Encrypts msg into out[PKT_LEN], assigning and consuming the next seq (also written to msg->seq).
// Returns 0 when the counter is exhausted (re-provision), 1 otherwise.
int pkt_encode(pkt_ctx_t *c, pkt_msg_t *msg, uint8_t out[PKT_LEN]);

// Verifies, decrypts and replay-checks in[len]. On PKT_OK, peer_seq is advanced.
pkt_err_t pkt_decode(pkt_ctx_t *c, const uint8_t *in, size_t len, pkt_msg_t *out);

// Peek at the clear header without any crypto (for logging rejected packets). id_out is the pseudonym.
int pkt_peek(const uint8_t *in, size_t len, uint8_t id_out[PKT_ID_LEN], uint8_t *type, uint32_t *seq_with_dir);

// The on-air pseudonym for one (key, id, seq-with-direction).
void pkt_pseudonym(const pkt_ctx_t *c, uint32_t seq_with_dir, uint8_t out[PKT_ID_LEN]);

const char *pkt_err_str(pkt_err_t e);
const char *pkt_type_str(uint8_t t);
const char *pkt_cmd_str(uint8_t c);
const char *pkt_state_str(uint8_t s);

#ifdef __cplusplus
}
#endif
