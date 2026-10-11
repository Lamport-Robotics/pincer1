// Pincer1 beacon base station: Heltec WiFi LoRa 32 V3 (SX1262) on USB.
//
// Receives beacon packets, verifies and decrypts them with the shared key, prints one JSON object
// per line on the USB serial, and sends authenticated commands typed on the same serial:
//
//   {"cmd":"disarm"}                   send until the beacon acknowledges (default 20 min, every 2 s)
//   {"cmd":"ping","repeat_s":60}       give up after 60 s
//   {"cmd":"stop"}                     cancel the pending command
//   status                             print the base state
//
// Output lines:
//   {"t":"base", ...}                                  at boot and on `status`
//   {"t":"alarm"|"heartbeat"|"ack", "id":"…", "seq":n, "battery_mv":n, "lat":f|null, "lon":f|null,
//    "fix_age_s":n|null, "trig":["ctrl","tamper",…], "hdop":f|null, "findmy_idx":n, "state":"armed",
//    "cmd":"disarm" (ack only), "rssi":f, "snr":f, "ms":n}
//   {"t":"reject","reason":"auth"|"replay"|…, "len":n, "rssi":f}
//   {"t":"sending","cmd":"disarm","tries":n}           every 30 s while a command is pending
//   {"t":"sent","cmd":"disarm","seq":n} / {"t":"gave_up","cmd":"disarm"}
//
// The beacon listens for 3 s every 15 min when idle/armed and continuously in alarm, so a command
// may take up to 15 min to land; the base repeats it every 2 s until the ack arrives. The region
// table below must match src/lora.cpp.
#include <Arduino.h>
#include <Preferences.h>
#include <RadioLib.h>
#include "board.h"
#include "config.h"
#include "packet.h"

struct Region { const char *name; float freq; float bw; uint8_t sf; uint8_t cr; int8_t power; uint16_t preamble; };
static const Region REGIONS[] = {
    {"EU868", 869.525f, 125.0f, 11, 8, 20, 12},
    {"US915", 915.000f, 500.0f, 11, 8, 20, 12},
    {"AS923", 923.200f, 125.0f, 11, 8, 16, 12},
};
static const Region *region() {
#if defined(BEACON_REGION_US915)
    return &REGIONS[1];
#elif defined(BEACON_REGION_AS923)
    return &REGIONS[2];
#else
    return &REGIONS[0];
#endif
}

static SPIClass radio_spi(HSPI);
static SX1262 radio = new Module(PIN_RADIO_CS, PIN_RADIO_DIO1, PIN_RADIO_RST, PIN_RADIO_BUSY, radio_spi);
static volatile bool rx_flag = false;
static void IRAM_ATTR on_dio1() { rx_flag = true; }

static Preferences prefs;
static pkt_ctx_t ctx;
static uint8_t DEV_ID[8], KEY[32];
static uint32_t seq_reserved = 0;

// pending command
static uint8_t pending_cmd = PKT_CMD_NONE;
static uint32_t pending_until = 0, pending_last_tx = 0, pending_tries = 0, pending_last_report = 0;

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) { unsigned v = 0; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v; }
}
static void print_hex(const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) Serial.printf("%02x", p[i]); }

static void reserve_seq() {
    if (ctx.next_seq + 1 >= seq_reserved) {
        seq_reserved = ctx.next_seq + CFG_SEQ_RESERVE;
        prefs.putUInt("seq_hi", seq_reserved);
    }
}

static void print_base() {
    Serial.printf("{\"t\":\"base\",\"fw\":\"%s\",\"board\":\"%s\",\"region\":\"%s\",\"device\":\"", BEACON_FW_VERSION, BOARD_NAME, region()->name);
    print_hex(DEV_ID, 8);
    Serial.printf("\",\"seq\":%lu,\"peer_seq\":%lu,\"pending\":\"%s\"", (unsigned long)ctx.next_seq, (unsigned long)ctx.peer_seq, pkt_cmd_str(pending_cmd));
#ifdef BEACON_UNPROVISIONED
    Serial.print(",\"unprovisioned\":true");
#endif
    Serial.println("}");
}

static void send_cmd(uint8_t cmd) {
    pkt_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = PKT_CMD;
    m.cmd = cmd;
    reserve_seq();
    uint8_t wire[PKT_LEN];
    if (!pkt_encode(&ctx, &m, wire)) { Serial.println("{\"t\":\"error\",\"reason\":\"counter_exhausted\"}"); return; }
    radio.transmit(wire, PKT_LEN);
    radio.startReceive();
    pending_last_tx = millis();
    pending_tries++;
    if (pending_tries == 1) Serial.printf("{\"t\":\"sent\",\"cmd\":\"%s\",\"seq\":%lu}\n", pkt_cmd_str(cmd), (unsigned long)m.seq);
}

static void print_packet(const pkt_msg_t &m, float rssi, float snr) {
    Serial.printf("{\"t\":\"%s\",\"id\":\"", pkt_type_str(m.type));
    print_hex(DEV_ID, 8);
    Serial.printf("\",\"seq\":%lu,\"battery_mv\":%u,", (unsigned long)m.seq, m.battery_mv);
    if (m.lat_e7 == 0 && m.lon_e7 == 0) Serial.print("\"lat\":null,\"lon\":null,");
    else Serial.printf("\"lat\":%.7f,\"lon\":%.7f,", m.lat_e7 / 1e7, m.lon_e7 / 1e7);
    if (m.fix_age_s == PKT_FIX_AGE_NEVER) Serial.print("\"fix_age_s\":null,");
    else Serial.printf("\"fix_age_s\":%u,", m.fix_age_s);
    Serial.print("\"trig\":[");
    const char *names[] = {"ctrl", "robot", "tamper", "motion", "low_battery"};
    bool first = true;
    for (int i = 0; i < 5; i++) if (m.trig & (1 << i)) { Serial.printf("%s\"%s\"", first ? "" : ",", names[i]); first = false; }
    Serial.print("],");
    if (m.hdop_x10 == PKT_HDOP_UNKNOWN) Serial.print("\"hdop\":null,");
    else Serial.printf("\"hdop\":%.1f,", m.hdop_x10 / 10.0);
    Serial.printf("\"findmy_idx\":%u,\"state\":\"%s\",", m.findmy_idx, pkt_state_str(m.state));
    if (m.type == PKT_ACK) Serial.printf("\"cmd\":\"%s\",", pkt_cmd_str(m.cmd));
    Serial.printf("\"rssi\":%.1f,\"snr\":%.1f,\"ms\":%lu}\n", rssi, snr, (unsigned long)millis());
}

static void handle_rx() {
    rx_flag = false;
    uint8_t buf[64];
    size_t len = radio.getPacketLength();
    float rssi = radio.getRSSI(), snr = radio.getSNR();
    int16_t st = (len > 0 && len <= sizeof buf) ? radio.readData(buf, len) : RADIOLIB_ERR_UNKNOWN;
    radio.startReceive();
    if (st != RADIOLIB_ERR_NONE) { Serial.printf("{\"t\":\"reject\",\"reason\":\"radio\",\"len\":%u,\"rssi\":%.1f}\n", (unsigned)len, rssi); return; }
    pkt_msg_t m;
    pkt_err_t e = pkt_decode(&ctx, buf, len, &m);
    if (e != PKT_OK) {
        Serial.printf("{\"t\":\"reject\",\"reason\":\"%s\",\"len\":%u,\"rssi\":%.1f}\n", pkt_err_str(e), (unsigned)len, rssi);
        return;
    }
    prefs.putUInt("peer_seq", ctx.peer_seq);
    digitalWrite(PIN_LED, HIGH);
    print_packet(m, rssi, snr);
    if (m.type == PKT_ACK && pending_cmd != PKT_CMD_NONE && m.cmd == pending_cmd) {
        pending_cmd = PKT_CMD_NONE;
    } else if (pending_cmd != PKT_CMD_NONE) {
        send_cmd(pending_cmd);     // the beacon listens right after it transmits
    }
    digitalWrite(PIN_LED, LOW);
}

static void handle_line(String line) {
    line.trim();
    if (line.length() == 0) return;
    if (line == "status") { print_base(); return; }
    int c = line.indexOf("\"cmd\"");
    if (c < 0) { Serial.println("{\"t\":\"error\",\"reason\":\"bad_line\"}"); return; }
    int q1 = line.indexOf('"', line.indexOf(':', c) + 1);
    int q2 = line.indexOf('"', q1 + 1);
    String cmd = (q1 > 0 && q2 > q1) ? line.substring(q1 + 1, q2) : "";
    uint32_t repeat_s = 20 * 60;
    int r = line.indexOf("\"repeat_s\"");
    if (r >= 0) repeat_s = (uint32_t)line.substring(line.indexOf(':', r) + 1).toInt();
    uint8_t code = PKT_CMD_NONE;
    if (cmd == "arm") code = PKT_CMD_ARM;
    else if (cmd == "disarm") code = PKT_CMD_DISARM;
    else if (cmd == "ping") code = PKT_CMD_PING;
    else if (cmd == "ok") code = PKT_CMD_OK;
    else if (cmd == "stop") { pending_cmd = PKT_CMD_NONE; Serial.println("{\"t\":\"stopped\"}"); return; }
    else { Serial.println("{\"t\":\"error\",\"reason\":\"unknown_cmd\"}"); return; }
#ifdef BEACON_UNPROVISIONED
    Serial.println("{\"t\":\"error\",\"reason\":\"unprovisioned\"}");
    return;
#endif
    pending_cmd = code;
    pending_tries = 0;
    pending_until = millis() + repeat_s * 1000;
    pending_last_report = millis();
    send_cmd(code);
}

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    pinMode(PIN_VEXT, OUTPUT);
    digitalWrite(PIN_VEXT, HIGH);   // OLED off
    unhex(BEACON_DEVICE_ID_HEX, DEV_ID, 8);
    unhex(BEACON_LORA_KEY_HEX, KEY, 32);
    prefs.begin("base", false);
    uint32_t seq_hi = prefs.getUInt("seq_hi", 1);
    pkt_init(&ctx, KEY, DEV_ID, 1, seq_hi, prefs.getUInt("peer_seq", 0));
    seq_reserved = seq_hi;
    reserve_seq();

    radio_spi.begin(PIN_RADIO_SCLK, PIN_RADIO_MISO, PIN_RADIO_MOSI, PIN_RADIO_CS);
    const Region *r = region();
    int16_t st = radio.begin(r->freq, r->bw, r->sf, r->cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, r->power, r->preamble, 1.6f, false);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("{\"t\":\"error\",\"reason\":\"radio_begin\",\"code\":%d}\n", st);
        while (true) { digitalWrite(PIN_LED, HIGH); delay(100); digitalWrite(PIN_LED, LOW); delay(900); }
    }
    radio.setDio2AsRfSwitch(true);
    radio.setCurrentLimit(140);
    radio.setCRC(true);
    radio.setDio1Action(on_dio1);
    radio.startReceive();
    print_base();
}

void loop() {
    if (rx_flag) handle_rx();
    if (Serial.available()) handle_line(Serial.readStringUntil('\n'));
    if (pending_cmd != PKT_CMD_NONE) {
        uint32_t now = millis();
        if ((int32_t)(now - pending_until) >= 0) {
            Serial.printf("{\"t\":\"gave_up\",\"cmd\":\"%s\",\"tries\":%lu}\n", pkt_cmd_str(pending_cmd), (unsigned long)pending_tries);
            pending_cmd = PKT_CMD_NONE;
        } else if (now - pending_last_tx >= 2000) {
            send_cmd(pending_cmd);
            if (now - pending_last_report >= 30000) {
                Serial.printf("{\"t\":\"sending\",\"cmd\":\"%s\",\"tries\":%lu}\n", pkt_cmd_str(pending_cmd), (unsigned long)pending_tries);
                pending_last_report = now;
            }
        }
    }
    delay(5);
}
