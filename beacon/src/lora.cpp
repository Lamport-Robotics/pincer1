#if defined(ARDUINO) && (defined(BOARD_TBEAM_SUPREME) || defined(BOARD_TBEAM_V12))
#include "lora.h"
#include "board.h"
#include <Arduino.h>
#include <RadioLib.h>

// Region defaults: long range first (SF10..12, 4/8), a few packets per hour. EU868 sits in the
// 869.4–869.65 MHz sub-band (10 % duty cycle, 500 mW ERP allowed) because an alarm sends a 48-byte
// packet every 2 minutes at SF11 (~2.5 s on air). US915 uses one channel under FCC 15.247 digital
// modulation rules with 500 kHz bandwidth. AS923 is the Thai 920–925 MHz band (AS923-1 plan).
// Local rules are the owner's responsibility; change the table if your base and beacon live elsewhere.
static const lora_region_t REGIONS[] = {
    {"EU868", 869.525f, 125.0f, 11, 8, 20, 12},
    {"US915", 915.000f, 500.0f, 11, 8, 20, 12},
    {"AS923", 923.200f, 125.0f, 11, 8, 16, 12},
};

const lora_region_t *lora_region() {
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
static bool ready = false;
static bool listening = false;

static void IRAM_ATTR on_dio1() { rx_flag = true; }

bool lora_begin() {
    if (ready) return true;
    board_lora_power(true);
    delay(20);
    radio_spi.begin(PIN_RADIO_SCLK, PIN_RADIO_MISO, PIN_RADIO_MOSI, PIN_RADIO_CS);
    const lora_region_t *r = lora_region();
    int16_t st = radio.begin(r->freq_mhz, r->bw_khz, r->sf, r->cr, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, r->power_dbm, r->preamble, 1.6f, false);
    if (st != RADIOLIB_ERR_NONE) {
        Serial.printf("lora: begin failed %d\n", st);
        return false;
    }
    radio.setDio2AsRfSwitch(true);
    radio.setCurrentLimit(140);
    radio.setCRC(true);
    radio.setDio1Action(on_dio1);
    ready = true;
    listening = false;
    return true;
}

void lora_end(bool cut_power) {
    if (ready) { radio.standby(); radio.sleep(); }
    ready = false;
    listening = false;
    if (cut_power) board_lora_power(false);
}

bool lora_send(const uint8_t *data, size_t len) {
    if (!ready && !lora_begin()) return false;
    listening = false;
    int16_t st = radio.transmit(data, len);
    if (st != RADIOLIB_ERR_NONE) Serial.printf("lora: tx failed %d\n", st);
    rx_flag = false;
    return st == RADIOLIB_ERR_NONE;
}

void lora_listen() {
    if (!ready && !lora_begin()) return;
    rx_flag = false;
    radio.startReceive();
    listening = true;
}

size_t lora_poll(uint8_t *buf, size_t cap, float *rssi, float *snr) {
    if (!ready) return 0;
    if (!listening) lora_listen();
    if (!rx_flag) return 0;
    rx_flag = false;
    size_t len = radio.getPacketLength();
    size_t got = 0;
    if (len > 0 && len <= cap) {
        int16_t st = radio.readData(buf, len);
        if (st == RADIOLIB_ERR_NONE) {
            got = len;
            if (rssi) *rssi = radio.getRSSI();
            if (snr) *snr = radio.getSNR();
        }
    }
    radio.startReceive();
    return got;
}

size_t lora_receive(uint8_t *buf, size_t cap, uint32_t timeout_ms, float *rssi, float *snr) {
    lora_listen();
    uint32_t t0 = millis();
    while (millis() - t0 < timeout_ms) {
        size_t n = lora_poll(buf, cap, rssi, snr);
        if (n) return n;
        delay(10);
    }
    return 0;
}
#endif
