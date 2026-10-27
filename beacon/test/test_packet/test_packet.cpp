// Packet + AEAD tests: RFC 8439 vectors, round trip, tamper, replay, direction, counters.
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "aead.h"
#include "packet.h"

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static const char *PT =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
    "sunscreen would be it.";   // 114 bytes

// ---------------------------------------------------------------- RFC 8439

void test_rfc8439_block_2_3_2(void) {
    uint8_t key[32], nonce[12], out[64], want[64];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    unhex("000000090000004a00000000", nonce, 12);
    unhex("10f1e7e4d13b5915500fdd1fa32071c4c7d1f4c733c068030422aa9ac3d46c4e"
          "d2826446079faa0914c2d705d98b02a2b5129cd1de164eb9cbd083e8a2503c4e", want, 64);
    chacha20_block(key, nonce, 1, out);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 64);
}

void test_rfc8439_chacha20_2_4_2(void) {
    uint8_t key[32], nonce[12], out[114], want[114];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
    unhex("000000000000004a00000000", nonce, 12);
    unhex("6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab"
          "8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e"
          "52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d", want, 114);
    chacha20_xor(key, nonce, 1, (const uint8_t *)PT, out, 114);
    TEST_ASSERT_EQUAL_MEMORY(want, out, 114);
    // in-place decrypt
    chacha20_xor(key, nonce, 1, out, out, 114);
    TEST_ASSERT_EQUAL_MEMORY(PT, out, 114);
}

void test_rfc8439_poly1305_2_5_2(void) {
    uint8_t key[32], tag[16], want[16];
    unhex("85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b", key, 32);
    unhex("a8061dc1305136c6c22b8baf0c0127a9", want, 16);
    const char *msg = "Cryptographic Forum Research Group";
    poly1305_mac(key, (const uint8_t *)msg, strlen(msg), tag);
    TEST_ASSERT_EQUAL_MEMORY(want, tag, 16);
}

void test_rfc8439_aead_2_8_2(void) {
    uint8_t key[32], nonce[12], aad[12], ct[114], tag[16], want_ct[114], want_tag[16], pt[114];
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    unhex("070000004041424344454647", nonce, 12);
    unhex("50515253c0c1c2c3c4c5c6c7", aad, 12);
    unhex("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca96712"
          "82fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58"
          "fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116", want_ct, 114);
    unhex("1ae10b594f09e26a7e902ecbd0600691", want_tag, 16);
    aead_encrypt(key, nonce, aad, 12, (const uint8_t *)PT, 114, ct, tag);
    TEST_ASSERT_EQUAL_MEMORY(want_ct, ct, 114);
    TEST_ASSERT_EQUAL_MEMORY(want_tag, tag, 16);
    TEST_ASSERT_EQUAL(1, aead_decrypt(key, nonce, aad, 12, ct, 114, tag, pt));
    TEST_ASSERT_EQUAL_MEMORY(PT, pt, 114);
    // any flipped bit anywhere is rejected
    ct[50] ^= 1;
    TEST_ASSERT_EQUAL(0, aead_decrypt(key, nonce, aad, 12, ct, 114, tag, pt));
    ct[50] ^= 1;
    aad[0] ^= 1;
    TEST_ASSERT_EQUAL(0, aead_decrypt(key, nonce, aad, 12, ct, 114, tag, pt));
    aad[0] ^= 1;
    tag[15] ^= 0x80;
    TEST_ASSERT_EQUAL(0, aead_decrypt(key, nonce, aad, 12, ct, 114, tag, pt));
}

void test_aead_empty_and_multiblock(void) {
    // empty plaintext still authenticates the AAD; 200-byte plaintext spans 4 blocks
    uint8_t key[32] = {1}, nonce[12] = {2}, aad[3] = {3, 4, 5}, tag[16], pt[200], ct[200], back[200];
    for (int i = 0; i < 200; i++) pt[i] = (uint8_t)(i * 7);
    aead_encrypt(key, nonce, aad, 3, pt, 0, ct, tag);
    TEST_ASSERT_EQUAL(1, aead_decrypt(key, nonce, aad, 3, ct, 0, tag, back));
    TEST_ASSERT_EQUAL(0, aead_decrypt(key, nonce, aad, 2, ct, 0, tag, back));
    aead_encrypt(key, nonce, aad, 3, pt, 200, ct, tag);
    TEST_ASSERT_EQUAL(1, aead_decrypt(key, nonce, aad, 3, ct, 200, tag, back));
    TEST_ASSERT_EQUAL_MEMORY(pt, back, 200);
}

// ---------------------------------------------------------------- packet

static uint8_t KEY[32], ID[8];

static void fresh(pkt_ctx_t *beacon, pkt_ctx_t *base) {
    for (int i = 0; i < 32; i++) KEY[i] = (uint8_t)(0xA0 + i);
    for (int i = 0; i < 8; i++) ID[i] = (uint8_t)(0x10 + i);
    pkt_init(beacon, KEY, ID, 0, 1, 0);
    pkt_init(base, KEY, ID, 1, 1, 0);
}

static pkt_msg_t sample_alarm(void) {
    pkt_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = PKT_ALARM;
    m.battery_mv = 3912;
    m.lat_e7 = 137563210;     // 13.7563210 N
    m.lon_e7 = 1005018000;    // 100.5018 E
    m.fix_age_s = 42;
    m.trig = PKT_TRIG_CTRL | PKT_TRIG_TAMPER;
    m.hdop_x10 = 12;
    m.findmy_idx = 777;
    m.state = PKT_STATE_ALARM;
    m.cmd = 0;
    return m;
}

void test_packet_round_trip(void) {
    pkt_ctx_t beacon, base;
    fresh(&beacon, &base);
    pkt_msg_t m = sample_alarm(), out;
    uint8_t wire[PKT_LEN];
    TEST_ASSERT_EQUAL(1, pkt_encode(&beacon, &m, wire));
    TEST_ASSERT_EQUAL(1, m.seq);
    TEST_ASSERT_EQUAL(PKT_VERSION, wire[0]);
    TEST_ASSERT_TRUE(memcmp(ID, wire + 1, 8) != 0);                   // the device id never goes on air
    TEST_ASSERT_EQUAL(PKT_ALARM, wire[9]);
    TEST_ASSERT_EQUAL(0, wire[13] & 0x80);                       // beacon direction
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(PKT_ALARM, out.type);
    TEST_ASSERT_EQUAL(1, out.seq);
    TEST_ASSERT_EQUAL(3912, out.battery_mv);
    TEST_ASSERT_EQUAL(137563210, out.lat_e7);
    TEST_ASSERT_EQUAL(1005018000, out.lon_e7);
    TEST_ASSERT_EQUAL(42, out.fix_age_s);
    TEST_ASSERT_EQUAL(PKT_TRIG_CTRL | PKT_TRIG_TAMPER, out.trig);
    TEST_ASSERT_EQUAL(12, out.hdop_x10);
    TEST_ASSERT_EQUAL(777, out.findmy_idx);
    TEST_ASSERT_EQUAL(PKT_STATE_ALARM, out.state);
    TEST_ASSERT_EQUAL(1, base.peer_seq);
    // negative coordinates survive
    m.lat_e7 = -339249000; m.lon_e7 = -1511400000; m.type = PKT_HEARTBEAT;
    TEST_ASSERT_EQUAL(1, pkt_encode(&beacon, &m, wire));
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(-339249000, out.lat_e7);
    TEST_ASSERT_EQUAL(-1511400000, out.lon_e7);
    TEST_ASSERT_EQUAL(PKT_HEARTBEAT, out.type);
}

void test_packet_pseudonym_changes_every_packet(void) {
    pkt_ctx_t beacon, base;
    fresh(&beacon, &base);
    pkt_msg_t m = sample_alarm();
    uint8_t w1[PKT_LEN], w2[PKT_LEN], want[8];
    pkt_encode(&beacon, &m, w1);
    pkt_encode(&beacon, &m, w2);
    TEST_ASSERT_TRUE(memcmp(w1 + 1, w2 + 1, 8) != 0);
    TEST_ASSERT_TRUE(memcmp(w1 + 1, ID, 8) != 0 && memcmp(w2 + 1, ID, 8) != 0);
    // the base derives the same pseudonym from the clear seq
    pkt_pseudonym(&base, 2, want);
    TEST_ASSERT_EQUAL_MEMORY(want, w2 + 1, 8);
    // a different key gives a different pseudonym for the same seq, so two robots' beacons on the
    // same channel are not linkable to each other either
    pkt_ctx_t other; uint8_t key2[32]; memcpy(key2, KEY, 32); key2[0] ^= 1;
    pkt_init(&other, key2, ID, 0, 1, 0);
    uint8_t w3[PKT_LEN];
    pkt_encode(&other, &m, w3);                                           // seq 1, like w1
    TEST_ASSERT_TRUE(memcmp(w1 + 1, w3 + 1, 8) != 0);
    // and a forged pseudonym on a real seq is refused before any decryption
    uint8_t copy[PKT_LEN]; memcpy(copy, w1, PKT_LEN); copy[3] ^= 1;
    pkt_msg_t out;
    TEST_ASSERT_EQUAL(PKT_ERR_ID, pkt_decode(&base, copy, PKT_LEN, &out));
}

void test_packet_body_is_encrypted(void) {
    pkt_ctx_t beacon, base;
    fresh(&beacon, &base);
    pkt_msg_t m = sample_alarm();
    uint8_t wire[PKT_LEN];
    pkt_encode(&beacon, &m, wire);
    // the raw little-endian latitude must not appear in the wire bytes
    uint8_t raw[4] = {(uint8_t)m.lat_e7, (uint8_t)(m.lat_e7 >> 8), (uint8_t)(m.lat_e7 >> 16), (uint8_t)(m.lat_e7 >> 24)};
    for (int i = 0; i + 4 <= PKT_LEN; i++) TEST_ASSERT_TRUE(memcmp(wire + i, raw, 4) != 0);
}

void test_packet_replay_rejected(void) {
    pkt_ctx_t beacon, base;
    fresh(&beacon, &base);
    pkt_msg_t m = sample_alarm(), out;
    uint8_t w1[PKT_LEN], w2[PKT_LEN], w3[PKT_LEN];
    pkt_encode(&beacon, &m, w1);
    pkt_encode(&beacon, &m, w2);
    pkt_encode(&beacon, &m, w3);
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, w2, PKT_LEN, &out));      // seq 2 (seq 1 lost in the air)
    TEST_ASSERT_EQUAL(PKT_ERR_REPLAY, pkt_decode(&base, w2, PKT_LEN, &out));   // same again
    TEST_ASSERT_EQUAL(PKT_ERR_REPLAY, pkt_decode(&base, w1, PKT_LEN, &out));   // older
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, w3, PKT_LEN, &out));      // newer
    TEST_ASSERT_EQUAL(3, base.peer_seq);
}

void test_packet_wrong_key_id_len_version(void) {
    pkt_ctx_t beacon, base, other;
    fresh(&beacon, &base);
    pkt_msg_t m = sample_alarm(), out;
    uint8_t wire[PKT_LEN], copy[PKT_LEN];
    pkt_encode(&beacon, &m, wire);

    uint8_t key2[32]; memcpy(key2, KEY, 32); key2[5] ^= 1;
    pkt_init(&other, key2, ID, 1, 1, 0);
    TEST_ASSERT_EQUAL(PKT_ERR_ID, pkt_decode(&other, wire, PKT_LEN, &out)); // wrong key: pseudonym already off
    TEST_ASSERT_EQUAL(0, other.peer_seq);                                 // a failed packet never advances
    memcpy(copy, wire, PKT_LEN); pkt_pseudonym(&other, 1, copy + 1);        // right pseudonym, wrong key
    TEST_ASSERT_EQUAL(PKT_ERR_AUTH, pkt_decode(&other, copy, PKT_LEN, &out));

    uint8_t id2[8]; memcpy(id2, ID, 8); id2[0] ^= 1;
    pkt_init(&other, KEY, id2, 1, 1, 0);
    TEST_ASSERT_EQUAL(PKT_ERR_ID, pkt_decode(&other, wire, PKT_LEN, &out));

    TEST_ASSERT_EQUAL(PKT_ERR_LEN, pkt_decode(&base, wire, PKT_LEN - 1, &out));
    memcpy(copy, wire, PKT_LEN); copy[0] = PKT_VERSION + 1;
    TEST_ASSERT_EQUAL(PKT_ERR_VERSION, pkt_decode(&base, copy, PKT_LEN, &out));
    memcpy(copy, wire, PKT_LEN); copy[9] = PKT_HEARTBEAT;                    // header is authenticated
    TEST_ASSERT_EQUAL(PKT_ERR_AUTH, pkt_decode(&base, copy, PKT_LEN, &out));
    memcpy(copy, wire, PKT_LEN); copy[9] = 9;
    TEST_ASSERT_EQUAL(PKT_ERR_TYPE, pkt_decode(&base, copy, PKT_LEN, &out));
    memcpy(copy, wire, PKT_LEN); copy[20] ^= 0x40;                           // body bit flip
    TEST_ASSERT_EQUAL(PKT_ERR_AUTH, pkt_decode(&base, copy, PKT_LEN, &out));
    // the good packet still decodes after all that
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, wire, PKT_LEN, &out));
}

void test_packet_direction_and_commands(void) {
    pkt_ctx_t beacon, base;
    fresh(&beacon, &base);
    pkt_msg_t cmd, out, ack;
    uint8_t wire[PKT_LEN];
    memset(&cmd, 0, sizeof cmd);
    cmd.type = PKT_CMD; cmd.cmd = PKT_CMD_DISARM;
    TEST_ASSERT_EQUAL(1, pkt_encode(&base, &cmd, wire));
    TEST_ASSERT_EQUAL(0x80, wire[13] & 0x80);                             // base direction bit
    // the base must not accept its own packet (echo / reflection)
    TEST_ASSERT_EQUAL(PKT_ERR_DIR, pkt_decode(&base, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&beacon, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(PKT_CMD, out.type);
    TEST_ASSERT_EQUAL(PKT_CMD_DISARM, out.cmd);
    // a beacon packet reflected back to the beacon is refused too
    memset(&ack, 0, sizeof ack); ack.type = PKT_ACK; ack.cmd = PKT_CMD_DISARM; ack.state = PKT_STATE_IDLE;
    pkt_encode(&beacon, &ack, wire);
    TEST_ASSERT_EQUAL(PKT_ERR_DIR, pkt_decode(&beacon, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(PKT_OK, pkt_decode(&base, wire, PKT_LEN, &out));
    TEST_ASSERT_EQUAL(PKT_ACK, out.type);
    // both directions have seq 1 and share the key: the nonces differ by the direction bit,
    // so the ciphertexts of identical bodies differ
    pkt_ctx_t b2, s2;
    fresh(&b2, &s2);
    pkt_msg_t same; memset(&same, 0, sizeof same); same.type = PKT_ACK;
    uint8_t wa[PKT_LEN], wb[PKT_LEN];
    pkt_encode(&b2, &same, wa);
    same.type = PKT_ACK; pkt_encode(&s2, &same, wb);
    TEST_ASSERT_TRUE(memcmp(wa + PKT_HDR_LEN, wb + PKT_HDR_LEN, PKT_BODY_LEN) != 0);
}

void test_packet_counter_exhaustion_and_peek(void) {
    pkt_ctx_t beacon;
    pkt_init(&beacon, KEY, ID, 0, PKT_SEQ_MAX, 0);
    pkt_msg_t m = sample_alarm();
    uint8_t wire[PKT_LEN];
    TEST_ASSERT_EQUAL(1, pkt_encode(&beacon, &m, wire));
    TEST_ASSERT_EQUAL(PKT_SEQ_MAX, m.seq);
    TEST_ASSERT_EQUAL(0, pkt_encode(&beacon, &m, wire));              // exhausted
    uint8_t id[8]; uint8_t type; uint32_t swd;
    TEST_ASSERT_EQUAL(1, pkt_peek(wire, PKT_LEN, id, &type, &swd));
    TEST_ASSERT_EQUAL_MEMORY(wire + 1, id, 8);                             // the pseudonym, not ID
    TEST_ASSERT_EQUAL(PKT_ALARM, type);
    TEST_ASSERT_EQUAL(PKT_SEQ_MAX, swd);
    TEST_ASSERT_EQUAL(0, pkt_peek(wire, 5, id, &type, &swd));
    // pkt_init clamps a zero counter to 1 (0 is the "nothing yet" replay value)
    pkt_init(&beacon, KEY, ID, 0, 0, 0);
    TEST_ASSERT_EQUAL(1, beacon.next_seq);
}

void test_packet_strings(void) {
    TEST_ASSERT_EQUAL_STRING("replay", pkt_err_str(PKT_ERR_REPLAY));
    TEST_ASSERT_EQUAL_STRING("alarm", pkt_type_str(PKT_ALARM));
    TEST_ASSERT_EQUAL_STRING("disarm", pkt_cmd_str(PKT_CMD_DISARM));
    TEST_ASSERT_EQUAL_STRING("armed", pkt_state_str(PKT_STATE_ARMED));
    TEST_ASSERT_EQUAL(48, PKT_LEN);
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_rfc8439_block_2_3_2);
    RUN_TEST(test_rfc8439_chacha20_2_4_2);
    RUN_TEST(test_rfc8439_poly1305_2_5_2);
    RUN_TEST(test_rfc8439_aead_2_8_2);
    RUN_TEST(test_aead_empty_and_multiblock);
    RUN_TEST(test_packet_round_trip);
    RUN_TEST(test_packet_pseudonym_changes_every_packet);
    RUN_TEST(test_packet_body_is_encrypted);
    RUN_TEST(test_packet_replay_rejected);
    RUN_TEST(test_packet_wrong_key_id_len_version);
    RUN_TEST(test_packet_direction_and_commands);
    RUN_TEST(test_packet_counter_exhaustion_and_peek);
    RUN_TEST(test_packet_strings);
    return UNITY_END();
}
