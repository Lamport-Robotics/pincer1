// u-blox GNSS on the UART through TinyGPSPlus, with the PMU rail gated by the board layer.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     valid;        // ever had a fix
    int32_t  lat_e7;
    int32_t  lon_e7;
    uint8_t  hdop_x10;     // PKT_HDOP_UNKNOWN when unknown
    uint8_t  sats;
    uint64_t at_ms;        // board_now_ms() when the fix was taken
} gps_fix_t;

// Power the receiver and open the UART. Idempotent.
void gps_begin();
// Cut the rail and close the UART.
void gps_end();
bool gps_is_on();
// Feed NMEA bytes; call often while the receiver is on. Returns true when a new valid fix arrived.
bool gps_poll();
// Block up to timeout_ms for a valid fix, polling; keep_alive is called every ~50 ms so the caller
// can service the radio meanwhile. Returns true on a fix.
bool gps_wait_fix(uint32_t timeout_ms, void (*keep_alive)(void));
// Most recent fix (valid=false before the first one). Persisted by main in flash across sleeps.
const gps_fix_t *gps_last_fix();
void gps_restore_fix(const gps_fix_t *fix);
// Seconds since the fix, saturated to 0xFFFF; 0xFFFF when there has never been one.
uint16_t gps_fix_age_s(uint64_t now_ms);
