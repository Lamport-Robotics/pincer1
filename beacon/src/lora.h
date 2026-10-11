// SX1262 through RadioLib: region table, send, timed receive. Used by the beacon; the base station
// has its own minimal copy of the same settings in base/src/main.cpp so both ends agree.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    float   freq_mhz;
    float   bw_khz;
    uint8_t sf;
    uint8_t cr;          // 5..8 = 4/5 .. 4/8
    int8_t  power_dbm;
    uint16_t preamble;
} lora_region_t;

// The region compiled in (BEACON_REGION_EU868 / US915 / AS923).
const lora_region_t *lora_region();

// Power the radio, initialise. Returns false when the chip does not answer.
bool lora_begin();
// Put the radio to sleep (keeps config) and, with cut_power, drop its rail.
void lora_end(bool cut_power);
// Blocking transmit. Returns true on success.
bool lora_send(const uint8_t *data, size_t len);
// Enter continuous receive. lora_poll() then returns a packet when one has arrived.
void lora_listen();
// Non-blocking: copies a received packet into buf (returns its length, 0 when none) and re-arms RX.
size_t lora_poll(uint8_t *buf, size_t cap, float *rssi, float *snr);
// Receive for up to timeout_ms. Returns the packet length or 0.
size_t lora_receive(uint8_t *buf, size_t cap, uint32_t timeout_ms, float *rssi, float *snr);
