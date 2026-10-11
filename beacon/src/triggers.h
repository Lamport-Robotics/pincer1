// Trigger debounce + arm/alarm state machine. Pure, host-tested (test/test_triggers).
//
// States: idle → armed → alarm. The owner arms and disarms over LoRa (authenticated commands).
//
// Inputs, sampled by the caller and fed with a millisecond clock:
//   ctrl    TRIG_CTRL from the consent controller (trusted): high ≥ ctrl_hold_ms = duress / tamper / wipe
//   robot   TRIG_ROBOT from the main computer (untrusted): robot lifted, owner lost
//   tamper  case switch opened (normally closed, so the line goes high when the shell is opened)
//   moving  the beacon's own IMU says the body is moving right now
//
// "Hard" inputs (ctrl, tamper) raise the alarm as soon as they are debounced. "Soft" inputs (robot,
// sustained motion) are suppressed for owner_ok_grace_ms after the owner sent `ok` from the base:
// the owner carrying the robot to another room should not start a hunt. Motion must be sustained:
// a motion episode (samples with gaps shorter than motion_gap_ms) of motion_minutes raises the alarm.
//
// In idle only ctrl counts (idle_honours_ctrl): a duress or wipe signal from the controller is never
// ignored. In armed everything counts and a heartbeat is due every heartbeat_ms. In alarm nothing
// changes the state except a command; later triggers still add their bit to `cause`.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BEACON_IDLE = 0, BEACON_ARMED = 1, BEACON_ALARM = 2 } beacon_state_t;   // = pkt_state_t

// cause bits = PKT_TRIG_* in packet.h
#define TRIG_CAUSE_CTRL   0x01
#define TRIG_CAUSE_ROBOT  0x02
#define TRIG_CAUSE_TAMPER 0x04
#define TRIG_CAUSE_MOTION 0x08

#define TRIG_CMD_ARM    1   // = PKT_CMD_ARM
#define TRIG_CMD_DISARM 2
#define TRIG_CMD_PING   3
#define TRIG_CMD_OK     4

typedef struct {
    uint32_t ctrl_hold_ms;        // 2000
    uint32_t robot_hold_ms;       // 500
    uint32_t tamper_hold_ms;      // 200
    uint32_t motion_minutes;      // 5: sustained motion this long raises the alarm
    uint32_t motion_gap_ms;       // 90000: a quiet gap longer than this ends a motion episode
    uint32_t owner_ok_grace_ms;   // 1800000 (30 min): soft triggers ignored this long after `ok`
    uint32_t heartbeat_ms;        // 3600000 (1 h)
    bool     idle_honours_ctrl;   // true
} trig_config_t;

typedef struct { bool ctrl, robot, tamper, moving; } trig_inputs_t;

typedef struct {
    trig_config_t  cfg;
    beacon_state_t state;
    uint64_t ctrl_since, robot_since, tamper_since;    // 0 = line currently low
    bool     ctrl_fired, robot_fired, tamper_fired;    // fired for the current high episode
    uint64_t motion_start, motion_last;                // 0 = no motion episode
    uint64_t owner_ok_at;                              // 0 = never
    uint64_t last_heartbeat;
    uint64_t alarm_since;
    uint8_t  cause;                                    // TRIG_CAUSE_* bits
} trig_t;

enum { TRIG_EV_ALARM = 1, TRIG_EV_HEARTBEAT_DUE = 2 };

void trig_default_config(trig_config_t *cfg);
void trig_init(trig_t *t, const trig_config_t *cfg, beacon_state_t initial, uint64_t now_ms);

// Feed one sample of the inputs. Returns TRIG_EV_* bits for what became true in this step.
// TRIG_EV_ALARM is reported once, on the idle/armed → alarm transition.
uint8_t trig_step(trig_t *t, uint64_t now_ms, const trig_inputs_t *in);

void trig_heartbeat_sent(trig_t *t, uint64_t now_ms);

// An authenticated command from the base. Returns true when the state changed.
bool trig_command(trig_t *t, uint8_t cmd, uint64_t now_ms);

beacon_state_t trig_state(const trig_t *t);
uint8_t        trig_cause(const trig_t *t);
bool           trig_heartbeat_due(const trig_t *t, uint64_t now_ms);

// LoRa alarm cadence: 120 s for the first hour of an alarm, 600 s after that.
uint32_t trig_alarm_interval_ms(uint64_t since_alarm_ms);

// How long the caller may sleep (idle/armed) before something needs attention: a debounce that is
// in progress, the next heartbeat, or the next motion sample (sample_ms; 0 = no motion sensor).
// UINT64_MAX when nothing is scheduled.
uint64_t trig_next_wake_ms(const trig_t *t, uint64_t now_ms, uint32_t sample_ms);

#ifdef __cplusplus
}
#endif
