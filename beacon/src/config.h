// Build-time configuration: the provisioned secrets (from beacon.env through tools/pio_keys.py)
// and the tunables. Everything here can be overridden with -D flags.
#pragma once
#include <stdint.h>

// --- provisioning (tools/provision.py). Without them the firmware builds UNPROVISIONED.
#ifndef BEACON_UNPROVISIONED
#ifndef BEACON_DEVICE_ID_HEX
#error "BEACON_DEVICE_ID_HEX missing: run tools/provision.py or define BEACON_UNPROVISIONED"
#endif
#else
#define BEACON_DEVICE_ID_HEX   "0000000000000000"
#define BEACON_LORA_KEY_HEX    "0000000000000000000000000000000000000000000000000000000000000000"
#define BEACON_FINDMY_SEED_HEX "0000000000000000000000000000000000000000000000000000000000000000"
#endif

#ifndef BEACON_HEARTBEAT_POSITION
#define BEACON_HEARTBEAT_POSITION 1       // 1 = heartbeats carry a 0.01° (≈1 km) position, 0 = none
#endif

// --- timing
#define CFG_GPS_FIX_TIMEOUT_MS      90000u   // alarm: wait this long for a fix, then use the last known
#define CFG_GPS_REFRESH_TIMEOUT_MS  30000u   // alarm, later fixes: shorter wait before each packet
#define CFG_GPS_HOT_FIX_AGE_MS      300000u  // a fix younger than this is "fresh": no new wait
#define CFG_FINDMY_ROTATE_MS        900000u  // 15 min
#define CFG_LISTEN_WINDOW_MS        3000u    // RX window after a heartbeat and on each listen wake
#define CFG_LISTEN_PERIOD_MS        900000u  // idle/armed: wake every 15 min and listen for commands
#define CFG_ALARM_RX_POLL_MS        50u
#define CFG_IMU_SAMPLE_MS           20000u   // armed: wake every 20 s to sample the IMU
#define CFG_IMU_THRESHOLD_MG        120      // change of the gravity vector between samples that counts as moving
#define CFG_DEBOUNCE_POLL_MS        50u
#define CFG_IDLE_MAX_SLEEP_MS       (24u * 3600u * 1000u)
#define CFG_LOW_BATTERY_MV          3350
#define CFG_SEQ_RESERVE             64u      // LoRa counter values reserved in flash per write
#define CFG_BUTTON_HOLD_MS          3000u    // hold the board button this long to arm/disarm by hand
