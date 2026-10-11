#if defined(ARDUINO) && (defined(BOARD_TBEAM_SUPREME) || defined(BOARD_TBEAM_V12))
#include "findmy_ble.h"
#include "findmy.h"
#include <Arduino.h>
#include <NimBLEDevice.h>          // brings host/ble_gap.h with the right prefix for the build
#if defined(CONFIG_NIMBLE_CPP_IDF)
#include <host/ble_hs_id.h>
#else
#include <nimble/nimble/host/include/host/ble_hs_id.h>
#endif

static bool inited = false;
static bool active = false;

bool findmy_ble_begin() {
    if (inited) return true;
    if (!NimBLEDevice::init("")) return false;     // waits for the host to sync
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);        // +9 dBm, like a tag
    inited = true;
    return true;
}

bool findmy_ble_advertise(const uint8_t pub[28], uint8_t status) {
    if (!inited && !findmy_ble_begin()) return false;
    if (active) { ble_gap_adv_stop(); active = false; }

    uint8_t addr_be[6], addr_le[6], adv[FINDMY_ADV_LEN];
    findmy_adv_address(pub, addr_be);
    for (int i = 0; i < 6; i++) addr_le[i] = addr_be[5 - i];   // the controller takes it little-endian
    int rc = ble_hs_id_set_rnd(addr_le);
    if (rc != 0) { Serial.printf("findmy: set_rnd failed %d\n", rc); return false; }

    findmy_adv_payload(pub, status, adv);
    rc = ble_gap_adv_set_data(adv, FINDMY_ADV_LEN);
    if (rc != 0) { Serial.printf("findmy: adv_set_data failed %d\n", rc); return false; }

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_NON;
    p.disc_mode = BLE_GAP_DISC_MODE_NON;
    p.itvl_min = 0x0640;    // 1000 ms in 0.625 ms units
    p.itvl_max = 0x0640;
    p.channel_map = 0x07;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER, &p, NULL, NULL);
    if (rc != 0) { Serial.printf("findmy: adv_start failed %d\n", rc); return false; }
    active = true;
    return true;
}

void findmy_ble_stop() {
    if (active) { ble_gap_adv_stop(); active = false; }
}

bool findmy_ble_active() { return active; }
#endif
