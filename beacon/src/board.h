// Board abstraction: pins, PMU rails, IMU, battery, sleep. One of BOARD_TBEAM_SUPREME /
// BOARD_TBEAM_V12 is defined by platformio.ini.
//
// Pin numbers for the two LilyGO boards follow the vendor's pin map (utilities.h in their
// LoRa-series examples). The PMU rail assignment is the part most likely to differ between board
// revisions: every rail the beacon needs is switched on at boot regardless, so a wrong GNSS/LoRa
// rail name costs sleep current, not function. Verify against your board before trusting the
// idle-current figures.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#if defined(BOARD_TBEAM_SUPREME)
// ---- LilyGO T-Beam Supreme: ESP32-S3, SX1262, u-blox GNSS, QMI8658 IMU (SPI), AXP2101 PMU (Wire1)
#define BOARD_NAME        "T-Beam Supreme"
#define BOARD_HAS_IMU     1
#define PIN_PMU_SDA       42
#define PIN_PMU_SCL       41
#define PIN_PMU_IRQ       40
#define PIN_I2C_SDA       17
#define PIN_I2C_SCL       18
#define PIN_GPS_RX        9    // ESP32 RX  ← GNSS TX
#define PIN_GPS_TX        8    // ESP32 TX  → GNSS RX
#define PIN_GPS_WAKEUP    7
#define PIN_GPS_PPS       6
#define PIN_BUTTON        0
#define PIN_RADIO_SCLK    12
#define PIN_RADIO_MISO    13
#define PIN_RADIO_MOSI    11
#define PIN_RADIO_CS      10
#define PIN_RADIO_DIO1    1
#define PIN_RADIO_RST     5
#define PIN_RADIO_BUSY    4
#define PIN_SPI_MOSI      35   // IMU + SD share this bus
#define PIN_SPI_SCK       36
#define PIN_SPI_MISO      37
#define PIN_IMU_CS        34
#define PIN_IMU_INT       33   // not an RTC GPIO on the S3: cannot wake deep sleep, so motion is polled
#define PIN_LED           -1   // no plain LED on the Supreme (the PMU charge LED is not ours)
// trigger inputs: RTC-capable GPIOs (0..21 on the S3) on the expansion header; change to taste
#define PIN_TRIG_CTRL     14   // from the consent controller GP16, active high
#define PIN_TRIG_ROBOT    15   // from a Jetson GPIO, active high
#define PIN_TAMPER        16   // normally-closed switch to GND, pull-up: high = shell opened
// AXP2101 rails (best-known map; all are enabled at boot anyway)
#define PMU_RAIL_SENSORS  XPOWERS_ALDO1
#define PMU_RAIL_LORA     XPOWERS_ALDO3
#define PMU_RAIL_GNSS     XPOWERS_ALDO4

#elif defined(BOARD_TBEAM_V12)
// ---- LilyGO T-Beam v1.2: ESP32, SX1262, u-blox NEO-6M/8M, AXP2101, no IMU
#define BOARD_NAME        "T-Beam v1.2"
#define BOARD_HAS_IMU     0
#define PIN_PMU_SDA       21
#define PIN_PMU_SCL       22
#define PIN_PMU_IRQ       35
#define PIN_I2C_SDA       21
#define PIN_I2C_SCL       22
#define PIN_GPS_RX        34
#define PIN_GPS_TX        12
#define PIN_GPS_WAKEUP    -1
#define PIN_GPS_PPS       -1
#define PIN_BUTTON        38
#define PIN_RADIO_SCLK    5
#define PIN_RADIO_MISO    19
#define PIN_RADIO_MOSI    27
#define PIN_RADIO_CS      18
#define PIN_RADIO_DIO1    33
#define PIN_RADIO_RST     23
#define PIN_RADIO_BUSY    32
#define PIN_LED           4
#define PIN_TRIG_CTRL     13   // RTC-capable on the ESP32: 0,2,4,12-15,25-27,32-39
#define PIN_TRIG_ROBOT    14
#define PIN_TAMPER        25
#define PMU_RAIL_SENSORS  XPOWERS_DCDC3
#define PMU_RAIL_LORA     XPOWERS_ALDO2
#define PMU_RAIL_GNSS     XPOWERS_ALDO3

#elif defined(BOARD_HELTEC_V3)
// ---- Heltec WiFi LoRa 32 V3 (base station): ESP32-S3, SX1262, no GNSS/PMU/IMU
#define BOARD_NAME        "Heltec WiFi LoRa 32 V3"
#define BOARD_HAS_IMU     0
#define PIN_RADIO_SCLK    9
#define PIN_RADIO_MISO    11
#define PIN_RADIO_MOSI    10
#define PIN_RADIO_CS      8
#define PIN_RADIO_DIO1    14
#define PIN_RADIO_RST     12
#define PIN_RADIO_BUSY    13
#define PIN_LED           35
#define PIN_VEXT          36   // active low: powers the OLED; left off
#define PIN_BUTTON        0

#elif !defined(ARDUINO)
#define BOARD_NAME        "native"
#define BOARD_HAS_IMU     0
#else
#error "define BOARD_TBEAM_SUPREME, BOARD_TBEAM_V12 or BOARD_HELTEC_V3"
#endif

#ifdef ARDUINO
#ifdef __cplusplus

// Power on the PMU, enable the rails, set up pins and the LED. Call first.
void board_init();
// Battery cell voltage in mV (0 when unknown).
uint16_t board_battery_mv();
bool     board_battery_charging();
// Switch rails. GNSS and LoRa are gated to save the cell in idle/armed.
void board_gps_power(bool on);
void board_lora_power(bool on);
// Level of the three trigger lines (true = active).
bool board_trig_ctrl();
bool board_trig_robot();
bool board_tamper();
bool board_button();                 // true while pressed
void board_led(bool on);
// IMU: initialise (no-op without one), and "moved since the last sample" by comparing the gravity
// vector with the previous sample kept in RTC memory. Threshold in milli-g.
bool board_imu_begin();
bool board_imu_moving(uint16_t threshold_mg);
// Monotonic milliseconds that survive deep sleep (RTC clock). Never wall time.
uint64_t board_now_ms();
// Enter deep sleep for at most `ms` (0 = no timer), waking on any trigger line going high.
// Does not return.
void board_deep_sleep(uint64_t ms);
// Why we woke: "timer", "pin", "power" (cold boot / reset), "other".
const char *board_wake_reason();
// A short line for logs: wake reason, battery, rails.
void board_print_status();
#endif
#endif
