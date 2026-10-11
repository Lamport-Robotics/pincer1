#ifdef ARDUINO
#include "gps.h"
#include "board.h"
#include "packet.h"
#include <Arduino.h>
#include <TinyGPSPlus.h>

static TinyGPSPlus gps;
static HardwareSerial gps_serial(1);
static bool on = false;
static gps_fix_t last = {false, 0, 0, PKT_HDOP_UNKNOWN, 0, 0};

void gps_begin() {
    if (on) return;
    board_gps_power(true);
    delay(100);
    gps_serial.begin(9600, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    if (PIN_GPS_WAKEUP >= 0) { pinMode(PIN_GPS_WAKEUP, OUTPUT); digitalWrite(PIN_GPS_WAKEUP, HIGH); }
    on = true;
}

void gps_end() {
    if (!on) return;
    gps_serial.end();
    board_gps_power(false);
    on = false;
}

bool gps_is_on() { return on; }

bool gps_poll() {
    if (!on) return false;
    bool fresh = false;
    while (gps_serial.available()) {
        if (gps.encode((char)gps_serial.read()) && gps.location.isValid() && gps.location.isUpdated()) {
            last.valid = true;
            last.lat_e7 = (int32_t)llround(gps.location.lat() * 1e7);
            last.lon_e7 = (int32_t)llround(gps.location.lng() * 1e7);
            last.hdop_x10 = gps.hdop.isValid() ? (uint8_t)min(254.0, gps.hdop.hdop() * 10.0) : PKT_HDOP_UNKNOWN;
            last.sats = gps.satellites.isValid() ? (uint8_t)gps.satellites.value() : 0;
            last.at_ms = board_now_ms();
            fresh = true;
        }
    }
    return fresh;
}

bool gps_wait_fix(uint32_t timeout_ms, void (*keep_alive)(void)) {
    gps_begin();
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        if (gps_poll()) return true;
        if (keep_alive) keep_alive();
        delay(50);
    }
    return false;
}

const gps_fix_t *gps_last_fix() { return &last; }
void gps_restore_fix(const gps_fix_t *fix) { if (fix) last = *fix; }

uint16_t gps_fix_age_s(uint64_t now_ms) {
    if (!last.valid) return PKT_FIX_AGE_NEVER;
    uint64_t age = now_ms > last.at_ms ? (now_ms - last.at_ms) / 1000ull : 0;
    return age >= PKT_FIX_AGE_NEVER ? (uint16_t)(PKT_FIX_AGE_NEVER - 1) : (uint16_t)age;
}
#endif
