// Pincer1 locator beacon: idle → armed → alarm.
//
//   idle    deep sleep; wakes on the trigger pins (only TRIG_CTRL counts), every 15 min to listen
//           for an `arm` command, and at most every 24 h to refresh. No radio, no GNSS.
//   armed   deep sleep; wakes on the trigger pins, every 20 s to sample the IMU, every 15 min to
//           listen for commands, every hour to send a heartbeat (battery, coarse position).
//   alarm   awake: GNSS on (90 s for a first fix, then last known), Find My advertising with a key
//           rotation every 15 min, a LoRa alarm packet every 2 min for the first hour and every
//           10 min after, radio in receive between packets. Ends only on an authenticated
//           `disarm` / `arm` over LoRa, or when the cell dies.
//
// The logic that decides (debounce, grace, cadence) is in triggers.cpp and host-tested; this file
// is the glue to the hardware and has not run on a board yet.
#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <string.h>
#include "board.h"
#include "config.h"
#include "packet.h"
#include "triggers.h"
#include "findmy.h"
#include "findmy_ble.h"
#include "lora.h"
#include "gps.h"

#define RTC_MAGIC 0x50314231u   // "P1B1"

// ---------------------------------------------------------------- state

static uint8_t DEV_ID[PKT_ID_LEN], LORA_KEY[32], FM_SEED[FINDMY_SEED_LEN];

RTC_DATA_ATTR static struct {
    uint32_t  magic;
    trig_t    trig;
    pkt_ctx_t pkt;
    uint32_t  seq_reserved;      // counter values below this are reserved in flash
    gps_fix_t fix;
    uint32_t  findmy_idx;
    uint64_t  findmy_rotated_at;
    uint64_t  last_listen;
} rtc;

static Preferences prefs;
static findmy_state_t fm;
static bool fm_ready = false;
static uint64_t next_tx_at = 0;
static uint64_t last_batt_at = 0;
static uint16_t batt_mv = 0;
static bool alarm_started = false;
static bool first_fix_wait_done = false;

static uint64_t now_ms() { return board_now_ms(); }

static void unhex(const char *h, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

// ---------------------------------------------------------------- persistence (NVS)

static void persist_state() {
    prefs.putUChar("state", (uint8_t)trig_state(&rtc.trig));
    prefs.putUChar("cause", trig_cause(&rtc.trig));
}
static void persist_fix() { prefs.putBytes("fix", &rtc.fix, sizeof rtc.fix); }
static void persist_fm_idx() { prefs.putUInt("fm_idx", rtc.findmy_idx); }
static void persist_peer_seq() { prefs.putUInt("peer_seq", rtc.pkt.peer_seq); }

// Reserve a block of counter values in flash so a reboot can never reuse a nonce.
static void reserve_seq() {
    if (rtc.pkt.next_seq + 1 >= rtc.seq_reserved) {
        rtc.seq_reserved = rtc.pkt.next_seq + CFG_SEQ_RESERVE;
        prefs.putUInt("seq_hi", rtc.seq_reserved);
    }
}

static void cold_boot_restore() {
    uint8_t state = prefs.getUChar("state", BEACON_IDLE);
    uint8_t cause = prefs.getUChar("cause", 0);
    uint32_t seq_hi = prefs.getUInt("seq_hi", 1);
    uint32_t peer = prefs.getUInt("peer_seq", 0);
    rtc.findmy_idx = prefs.getUInt("fm_idx", 0);
    memset(&rtc.fix, 0, sizeof rtc.fix);
    rtc.fix.hdop_x10 = PKT_HDOP_UNKNOWN;
    prefs.getBytes("fix", &rtc.fix, sizeof rtc.fix);
    rtc.fix.at_ms = 0;                      // the clock restarted: the age is unknown, report it as old

    trig_config_t cfg;
    trig_default_config(&cfg);
    trig_init(&rtc.trig, &cfg, (beacon_state_t)state, now_ms());
    rtc.trig.cause = cause;
    pkt_init(&rtc.pkt, LORA_KEY, DEV_ID, 0, seq_hi, peer);
    rtc.seq_reserved = seq_hi;
    reserve_seq();
    rtc.findmy_rotated_at = 0;
    rtc.last_listen = 0;
    rtc.magic = RTC_MAGIC;
}

// ---------------------------------------------------------------- radio helpers

static void read_battery() {
    batt_mv = board_battery_mv();
    last_batt_at = now_ms();
}

static int32_t coarse(int32_t e7) { return (e7 / 100000) * 100000; }   // 0.01° ≈ 1.1 km

static bool send_packet(uint8_t type, uint8_t cmd) {
    pkt_msg_t m;
    memset(&m, 0, sizeof m);
    m.type = type;
    m.battery_mv = batt_mv;
    const gps_fix_t *fix = gps_last_fix();
    bool with_pos = fix->valid && (type != PKT_HEARTBEAT || BEACON_HEARTBEAT_POSITION);
    if (with_pos) {
        m.lat_e7 = type == PKT_HEARTBEAT ? coarse(fix->lat_e7) : fix->lat_e7;
        m.lon_e7 = type == PKT_HEARTBEAT ? coarse(fix->lon_e7) : fix->lon_e7;
        m.hdop_x10 = type == PKT_HEARTBEAT ? PKT_HDOP_UNKNOWN : fix->hdop_x10;
    } else {
        m.hdop_x10 = PKT_HDOP_UNKNOWN;
    }
    m.fix_age_s = gps_fix_age_s(now_ms());
    m.trig = trig_cause(&rtc.trig) | (batt_mv && batt_mv < CFG_LOW_BATTERY_MV ? PKT_TRIG_LOWBAT : 0);
    m.findmy_idx = (uint16_t)rtc.findmy_idx;
    m.state = (uint8_t)trig_state(&rtc.trig);
    m.cmd = cmd;
    reserve_seq();
    uint8_t wire[PKT_LEN];
    if (!pkt_encode(&rtc.pkt, &m, wire)) { Serial.println("packet: counter exhausted, re-provision"); return false; }
    bool ok = lora_send(wire, PKT_LEN);
    Serial.printf("lora: %s seq=%lu %s\n", pkt_type_str(type), (unsigned long)m.seq, ok ? "sent" : "FAILED");
    return ok;
}

// Handle one received frame. Returns the command it carried (PKT_CMD_NONE when nothing useful).
static uint8_t handle_rx(const uint8_t *buf, size_t len) {
    pkt_msg_t m;
    pkt_err_t e = pkt_decode(&rtc.pkt, buf, len, &m);
    if (e != PKT_OK) {
        Serial.printf("lora: rejected (%s) len=%u\n", pkt_err_str(e), (unsigned)len);
        return PKT_CMD_NONE;
    }
    persist_peer_seq();
    if (m.type != PKT_CMD) return PKT_CMD_NONE;
    Serial.printf("lora: command %s seq=%lu\n", pkt_cmd_str(m.cmd), (unsigned long)m.seq);
    bool changed = trig_command(&rtc.trig, m.cmd, now_ms());
    if (changed) persist_state();
    read_battery();
    send_packet(PKT_ACK, m.cmd);
    return m.cmd;
}

static void listen_window(uint32_t ms) {
    uint8_t buf[64];
    float rssi, snr;
    size_t n = lora_receive(buf, sizeof buf, ms, &rssi, &snr);
    if (n) handle_rx(buf, n);
    rtc.last_listen = now_ms();
}

static void service_radio() {   // used while waiting for a fix
    uint8_t buf[64];
    size_t n = lora_poll(buf, sizeof buf, NULL, NULL);
    if (n) handle_rx(buf, n);
}

// ---------------------------------------------------------------- Find My

static bool findmy_advertise_current() {
    if (!fm_ready) {
        findmy_init_at(&fm, FM_SEED, rtc.findmy_idx);
        fm_ready = true;
    }
    uint8_t d[FINDMY_KEY_LEN], x[FINDMY_KEY_LEN];
    findmy_private_key(&fm, d);
    bool ok = findmy_public_key(d, x);
    memset(d, 0, sizeof d);
    if (!ok) { Serial.println("findmy: public key failed"); return false; }
    ok = findmy_ble_advertise(x, findmy_status_for_mv(batt_mv));
    Serial.printf("findmy: key %lu %s\n", (unsigned long)rtc.findmy_idx, ok ? "advertising" : "FAILED");
    return ok;
}

static void findmy_rotate_if_due() {
    uint64_t now = now_ms();
    if (rtc.findmy_rotated_at && now - rtc.findmy_rotated_at < CFG_FINDMY_ROTATE_MS) return;
    if (rtc.findmy_rotated_at) {      // not the first key of this alarm
        findmy_advance(&fm);
        rtc.findmy_idx = fm.index;
        persist_fm_idx();
    }
    rtc.findmy_rotated_at = now ? now : 1;
    findmy_advertise_current();
}

// ---------------------------------------------------------------- inputs

static bool debounce_pending() {
    const trig_t *t = &rtc.trig;
    return (t->ctrl_since && !t->ctrl_fired) || (t->robot_since && !t->robot_fired) || (t->tamper_since && !t->tamper_fired);
}

static void on_alarm_entered() {
    Serial.printf("ALARM cause=0x%02x\n", trig_cause(&rtc.trig));
    persist_state();
    alarm_started = false;
}

// Sample the inputs, run the debounces to completion (at most a few seconds), handle the button
// and bench commands on the USB serial. Returns true when the state is alarm afterwards.
static bool settle(bool sample_imu) {
    bool moving = sample_imu ? board_imu_moving(CFG_IMU_THRESHOLD_MG) : false;
    uint64_t start = now_ms(), button_since = 0;
    for (;;) {
        uint64_t now = now_ms();
        trig_inputs_t in = {board_trig_ctrl(), board_trig_robot(), board_tamper(), moving};
        uint8_t ev = trig_step(&rtc.trig, now, &in);
        if (ev & TRIG_EV_ALARM) { on_alarm_entered(); return true; }

        // board button: hold 3 s toggles idle ↔ armed (never out of alarm)
        if (board_button()) {
            if (!button_since) button_since = now;
            else if (now - button_since >= CFG_BUTTON_HOLD_MS && trig_state(&rtc.trig) != BEACON_ALARM) {
#ifndef BEACON_UNPROVISIONED
                trig_command(&rtc.trig, trig_state(&rtc.trig) == BEACON_ARMED ? TRIG_CMD_DISARM : TRIG_CMD_ARM, now);
                persist_state();
                Serial.printf("button: now %s\n", pkt_state_str(trig_state(&rtc.trig)));
#else
                Serial.println("button: unprovisioned, cannot arm");
#endif
                for (int i = 0; i < 6; i++) { board_led(i & 1); delay(100); }
                while (board_button()) delay(20);
                button_since = 0;
            }
        } else button_since = 0;

        // bench commands over USB
        if (Serial.available()) {
            String line = Serial.readStringUntil('\n');
            line.trim();
            if (line == "status") {
                board_print_status();
                Serial.printf("state=%s cause=0x%02x seq=%lu peer=%lu fm_idx=%lu fix=%d age=%us\n",
                              pkt_state_str(trig_state(&rtc.trig)), trig_cause(&rtc.trig), (unsigned long)rtc.pkt.next_seq,
                              (unsigned long)rtc.pkt.peer_seq, (unsigned long)rtc.findmy_idx, gps_last_fix()->valid, gps_fix_age_s(now));
            } else if (line == "arm" || line == "disarm" || line == "ok") {
#ifndef BEACON_UNPROVISIONED
                if (trig_state(&rtc.trig) == BEACON_ALARM) {
                    // the shell is open (tamper) or the robot was taken: whoever holds the USB cable is not the owner
                    Serial.println("serial: alarm; only the owner's `ok` over the radio ends it");
                } else
                trig_command(&rtc.trig, line == "arm" ? TRIG_CMD_ARM : line == "disarm" ? TRIG_CMD_DISARM : TRIG_CMD_OK, now);
                persist_state();
                Serial.printf("serial: now %s\n", pkt_state_str(trig_state(&rtc.trig)));
#else
                Serial.println("serial: unprovisioned");
#endif
            } else if (line == "test-alarm" && trig_state(&rtc.trig) != BEACON_ALARM) {
                rtc.trig.cause |= TRIG_CAUSE_ROBOT;
                rtc.trig.state = BEACON_ALARM;
                rtc.trig.alarm_since = now;
                on_alarm_entered();
                return true;
            }
        }

        bool keep = debounce_pending() || button_since || (now - start < 300);
        if (!keep || now - start > 15000) break;
        delay(CFG_DEBOUNCE_POLL_MS);
    }
    return trig_state(&rtc.trig) == BEACON_ALARM;
}

// ---------------------------------------------------------------- sleep

static void go_to_sleep() {
    uint64_t now = now_ms();
    bool armed = trig_state(&rtc.trig) == BEACON_ARMED;
    uint64_t wake = trig_next_wake_ms(&rtc.trig, now, armed && BOARD_HAS_IMU ? CFG_IMU_SAMPLE_MS : 0);
    uint64_t since_listen = now - rtc.last_listen;
    uint64_t listen_in = since_listen >= CFG_LISTEN_PERIOD_MS ? 0 : CFG_LISTEN_PERIOD_MS - since_listen;
    if (listen_in < wake) wake = listen_in;
    if (wake > CFG_IDLE_MAX_SLEEP_MS) wake = CFG_IDLE_MAX_SLEEP_MS;
    if (wake < 1000) wake = 1000;
    findmy_ble_stop();
    gps_end();
    lora_end(true);
    rtc.fix = *gps_last_fix();
    Serial.printf("sleep %s for %llu s\n", pkt_state_str(trig_state(&rtc.trig)), (unsigned long long)(wake / 1000));
    board_deep_sleep(wake);
}

// ---------------------------------------------------------------- alarm

static void alarm_begin() {
    alarm_started = true;
    first_fix_wait_done = false;
    read_battery();
    board_lora_power(true);
    lora_begin();
    findmy_ble_begin();
    rtc.findmy_rotated_at = 0;
    findmy_rotate_if_due();
    gps_begin();
    // first word to the base at once, with whatever position we have
    send_packet(PKT_ALARM, PKT_CMD_NONE);
    lora_listen();
    next_tx_at = now_ms() + CFG_GPS_FIX_TIMEOUT_MS;   // the next one goes out with the fix, or at the timeout
}

static void alarm_tick() {
    if (!alarm_started) alarm_begin();
    uint64_t now = now_ms();

    gps_poll();
    service_radio();
    if (trig_state(&rtc.trig) != BEACON_ALARM) {      // an authenticated command ended it
        Serial.printf("alarm ended: %s\n", pkt_state_str(trig_state(&rtc.trig)));
        alarm_started = false;
        go_to_sleep();                                 // does not return
    }
    findmy_rotate_if_due();
    if (now - last_batt_at > 30000) read_battery();

    // later triggers still feed the cause bits
    trig_inputs_t in = {board_trig_ctrl(), board_trig_robot(), board_tamper(), false};
    trig_step(&rtc.trig, now, &in);

    uint64_t since = now - rtc.trig.alarm_since;
    bool first_hour = since < 3600000ull;
    const gps_fix_t *fix = gps_last_fix();
    bool fix_fresh = fix->valid && now - fix->at_ms < CFG_GPS_HOT_FIX_AGE_MS;

    if (!first_fix_wait_done && fix_fresh) {           // the first fix arrived: send it now
        first_fix_wait_done = true;
        next_tx_at = now;
    }
    if (now >= next_tx_at) {
        first_fix_wait_done = true;
        send_packet(PKT_ALARM, PKT_CMD_NONE);
        rtc.fix = *fix;
        persist_fix();
        uint32_t interval = trig_alarm_interval_ms(since);
        next_tx_at = now + interval;
        lora_listen();
        if (!first_hour) gps_end();                    // after the first hour the receiver only runs before a packet
    }
    if (!first_hour && !gps_is_on() && next_tx_at > now && next_tx_at - now <= CFG_GPS_FIX_TIMEOUT_MS) gps_begin();

    delay(CFG_ALARM_RX_POLL_MS);
}

// ---------------------------------------------------------------- arduino

void setup() {
    Serial.begin(115200);
    unhex(BEACON_DEVICE_ID_HEX, DEV_ID, PKT_ID_LEN);
    unhex(BEACON_LORA_KEY_HEX, LORA_KEY, 32);
    unhex(BEACON_FINDMY_SEED_HEX, FM_SEED, FINDMY_SEED_LEN);
    board_init();
    prefs.begin("beacon", false);

    bool cold = rtc.magic != RTC_MAGIC;
    if (cold) cold_boot_restore();
    gps_restore_fix(&rtc.fix);
    read_battery();

    Serial.printf("pincer1 beacon %s on %s, %s boot, state=%s region=%s\n", BEACON_FW_VERSION, BOARD_NAME,
                  cold ? "cold" : "warm", pkt_state_str(trig_state(&rtc.trig)), lora_region()->name);
#ifdef BEACON_UNPROVISIONED
    Serial.println("UNPROVISIONED build: no keys, the beacon will not arm. Run tools/provision.py and rebuild.");
    if (trig_state(&rtc.trig) != BEACON_IDLE) { rtc.trig.state = BEACON_IDLE; persist_state(); }
#endif
    board_print_status();

    if (trig_state(&rtc.trig) == BEACON_ALARM) return;      // loop() carries on with the alarm

    bool armed = trig_state(&rtc.trig) == BEACON_ARMED;
    bool timer_wake = strcmp(board_wake_reason(), "timer") == 0;
    if (armed && BOARD_HAS_IMU) board_imu_begin();
    if (settle(armed && timer_wake)) return;                 // alarm raised: loop() takes over

    uint64_t now = now_ms();
    if (trig_heartbeat_due(&rtc.trig, now)) {
        lora_begin();
        send_packet(PKT_HEARTBEAT, PKT_CMD_NONE);
        trig_heartbeat_sent(&rtc.trig, now);
        listen_window(CFG_LISTEN_WINDOW_MS);
    } else if (now - rtc.last_listen >= CFG_LISTEN_PERIOD_MS || rtc.last_listen == 0) {
        lora_begin();
        listen_window(CFG_LISTEN_WINDOW_MS);
    }
    if (trig_state(&rtc.trig) == BEACON_ALARM) return;       // a command cannot do that, but be safe
    go_to_sleep();
}

void loop() {
    if (trig_state(&rtc.trig) == BEACON_ALARM) alarm_tick();
    else go_to_sleep();
}
#endif
