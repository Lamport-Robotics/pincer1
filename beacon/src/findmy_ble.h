// Find My advertising on the ESP32 through NimBLE: random static address + 31-byte payload from
// findmy_adv_payload(). Device only.
#pragma once
#include <stdbool.h>
#include <stdint.h>

bool findmy_ble_begin();
// (Re)start advertising a 28-byte public key x-coordinate with the given status byte.
bool findmy_ble_advertise(const uint8_t pub[28], uint8_t status);
void findmy_ble_stop();
bool findmy_ble_active();
