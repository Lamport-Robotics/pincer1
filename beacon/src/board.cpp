// Board layer for the T-Beam boards: AXP2101 through XPowersLib, QMI8658 through SensorLib,
// deep sleep with pin + timer wake. Untested on hardware.
#if defined(ARDUINO) && (defined(BOARD_TBEAM_SUPREME) || defined(BOARD_TBEAM_V12))
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <sys/time.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#if BOARD_HAS_IMU
#include <SensorQMI8658.hpp>
static SensorQMI8658 qmi;
static bool imu_ok = false;
RTC_DATA_ATTR static float rtc_last_acc[3] = {0, 0, 0};
RTC_DATA_ATTR static bool rtc_have_acc = false;
#endif

static XPowersAXP2101 pmu;                  // concrete: begin(), battery, charger
static XPowersLibInterface *rails = &pmu;   // the rail switches are public only on the interface
static bool pmu_ok = false;
#ifdef BOARD_TBEAM_SUPREME
static TwoWire &pmu_wire = Wire1;
#else
static TwoWire &pmu_wire = Wire;
#endif

static void pin_in(int pin, bool pullup) {
    if (pin < 0) return;
    pinMode(pin, pullup ? INPUT_PULLUP : INPUT_PULLDOWN);
}

void board_init() {
    pin_in(PIN_TRIG_CTRL, false);
    pin_in(PIN_TRIG_ROBOT, false);
    pin_in(PIN_TAMPER, true);
    pinMode(PIN_BUTTON, INPUT_PULLUP);
    if (PIN_LED >= 0) { pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW); }

    pmu_wire.begin(PIN_PMU_SDA, PIN_PMU_SCL);
    pmu_ok = pmu.begin(pmu_wire, AXP2101_SLAVE_ADDRESS, PIN_PMU_SDA, PIN_PMU_SCL);
    if (pmu_ok) {
        // keep the ESP32 rail as the bootloader left it; switch on everything the beacon may use
        rails->setPowerChannelVoltage(PMU_RAIL_SENSORS, 3300);
        rails->enablePowerOutput(PMU_RAIL_SENSORS);
        rails->setPowerChannelVoltage(PMU_RAIL_LORA, 3300);
        rails->enablePowerOutput(PMU_RAIL_LORA);
        rails->setPowerChannelVoltage(PMU_RAIL_GNSS, 3300);
        rails->enablePowerOutput(PMU_RAIL_GNSS);
#ifdef BOARD_TBEAM_SUPREME
        // rails we do not use (SD card, display) stay off; the M.2 / other rails are left alone
        rails->disablePowerOutput(XPOWERS_BLDO1);
        rails->disablePowerOutput(XPOWERS_BLDO2);
#endif
        pmu.disableTSPinMeasure();          // no thermistor on these boards; measuring it blocks charging
        pmu.enableBattVoltageMeasure();
        pmu.enableVbusVoltageMeasure();
        pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
        pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
        pmu.setChargingLedMode(XPOWERS_CHG_LED_OFF);   // a blinking LED inside a stolen robot helps nobody
        pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);   // a long press on the PMU key still powers off
    }
    delay(50);   // rails settle
}

uint16_t board_battery_mv() {
    if (!pmu_ok) return 0;
    if (!pmu.isBatteryConnect()) return 0;
    return (uint16_t)pmu.getBattVoltage();
}

bool board_battery_charging() { return pmu_ok && pmu.isCharging(); }

void board_gps_power(bool on) {
    if (!pmu_ok) return;
    if (on) { rails->setPowerChannelVoltage(PMU_RAIL_GNSS, 3300); rails->enablePowerOutput(PMU_RAIL_GNSS); }
    else rails->disablePowerOutput(PMU_RAIL_GNSS);
}

void board_lora_power(bool on) {
    if (!pmu_ok) return;
    if (on) { rails->setPowerChannelVoltage(PMU_RAIL_LORA, 3300); rails->enablePowerOutput(PMU_RAIL_LORA); }
    else rails->disablePowerOutput(PMU_RAIL_LORA);
}

bool board_trig_ctrl()  { return digitalRead(PIN_TRIG_CTRL) == HIGH; }
bool board_trig_robot() { return digitalRead(PIN_TRIG_ROBOT) == HIGH; }
bool board_tamper()     { return digitalRead(PIN_TAMPER) == HIGH; }   // NC switch opened or cut
bool board_button()     { return digitalRead(PIN_BUTTON) == LOW; }
void board_led(bool on) { if (PIN_LED >= 0) digitalWrite(PIN_LED, on ? HIGH : LOW); }

bool board_imu_begin() {
#if BOARD_HAS_IMU
    if (imu_ok) return true;
    SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
    if (!qmi.begin(PIN_IMU_CS, PIN_SPI_MOSI, PIN_SPI_MISO, PIN_SPI_SCK, SPI)) return false;
    qmi.configAccelerometer(SensorQMI8658::ACC_RANGE_4G, SensorQMI8658::ACC_ODR_125Hz, SensorQMI8658::LPF_MODE_0);
    qmi.enableAccelerometer();
    pinMode(PIN_IMU_INT, INPUT);
    imu_ok = true;
    return true;
#else
    return false;
#endif
}

bool board_imu_moving(uint16_t threshold_mg) {
#if BOARD_HAS_IMU
    if (!imu_ok && !board_imu_begin()) return false;
    // average a few samples of the gravity vector; compare with the previous wake's vector
    float ax = 0, ay = 0, az = 0;
    int n = 0;
    uint32_t t0 = millis();
    while (n < 8 && millis() - t0 < 200) {
        if (qmi.getDataReady()) {
            float x, y, z;
            if (qmi.getAccelerometer(x, y, z)) { ax += x; ay += y; az += z; n++; }
        }
        delay(5);
    }
    if (n == 0) return false;
    ax /= n; ay /= n; az /= n;
    bool moving = false;
    if (rtc_have_acc) {
        float dx = ax - rtc_last_acc[0], dy = ay - rtc_last_acc[1], dz = az - rtc_last_acc[2];
        float d_mg = sqrtf(dx * dx + dy * dy + dz * dz) * 1000.0f;   // SensorLib reports g
        moving = d_mg > (float)threshold_mg;
    }
    rtc_last_acc[0] = ax; rtc_last_acc[1] = ay; rtc_last_acc[2] = az;
    rtc_have_acc = true;
    return moving;
#else
    (void)threshold_mg;
    return false;
#endif
}

uint64_t board_now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);                         // backed by the RTC timer: survives deep sleep
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

static void rtc_wake_pin(int pin, bool pullup) {
    if (pin < 0) return;
    gpio_num_t g = (gpio_num_t)pin;
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    if (pullup) { rtc_gpio_pullup_en(g); rtc_gpio_pulldown_dis(g); }
    else        { rtc_gpio_pulldown_en(g); rtc_gpio_pullup_dis(g); }
}

void board_deep_sleep(uint64_t ms) {
    // ext1 wake is level-triggered: a line that is high right now would wake us at once, forever.
    // Such lines (already debounced and acted on by the caller) are left out of the mask and the
    // sleep is capped at a minute so they are re-checked when they drop.
    uint64_t mask = 0;
    bool held = false;
    rtc_wake_pin(PIN_TRIG_CTRL, false);  if (!board_trig_ctrl())  mask |= 1ull << PIN_TRIG_CTRL;  else held = true;
    rtc_wake_pin(PIN_TRIG_ROBOT, false); if (!board_trig_robot()) mask |= 1ull << PIN_TRIG_ROBOT; else held = true;
    rtc_wake_pin(PIN_TAMPER, true);      if (!board_tamper())     mask |= 1ull << PIN_TAMPER;     else held = true;
    if (held && (ms == 0 || ms > 60000)) ms = 60000;
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);   // keep the pulls alive
    if (mask) esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_HIGH);
    if (ms) esp_sleep_enable_timer_wakeup(ms * 1000ull);
    Serial.flush();
    esp_deep_sleep_start();
}

const char *board_wake_reason() {
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: return "timer";
        case ESP_SLEEP_WAKEUP_EXT1: return "pin";
        case ESP_SLEEP_WAKEUP_UNDEFINED: return "power";
        default: return "other";
    }
}

void board_print_status() {
    Serial.printf("board %s wake=%s batt=%umV%s pmu=%d imu=%d ctrl=%d robot=%d tamper=%d\n",
                  BOARD_NAME, board_wake_reason(), board_battery_mv(), board_battery_charging() ? " charging" : "",
                  pmu_ok,
#if BOARD_HAS_IMU
                  imu_ok,
#else
                  0,
#endif
                  board_trig_ctrl(), board_trig_robot(), board_tamper());
}
#endif
