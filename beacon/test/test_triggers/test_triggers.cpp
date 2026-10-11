// Trigger state machine tests: each input, debounce, grace window, heartbeat cadence, commands,
// alarm cadence and the sleep scheduler.
#include <unity.h>
#include <string.h>
#include "triggers.h"

static trig_config_t cfg;
static trig_t t;

static trig_inputs_t in(bool ctrl, bool robot, bool tamper, bool moving) {
    trig_inputs_t i = {ctrl, robot, tamper, moving};
    return i;
}
static const trig_inputs_t QUIET = {false, false, false, false};

void setUp(void) {
    trig_default_config(&cfg);
    trig_init(&t, &cfg, BEACON_ARMED, 1000);
}
void tearDown(void) {}

void test_defaults(void) {
    TEST_ASSERT_EQUAL(2000, cfg.ctrl_hold_ms);
    TEST_ASSERT_EQUAL(5, cfg.motion_minutes);
    TEST_ASSERT_EQUAL(3600000, cfg.heartbeat_ms);
    TEST_ASSERT_TRUE(cfg.idle_honours_ctrl);
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    TEST_ASSERT_EQUAL(0, trig_cause(&t));
}

void test_ctrl_needs_two_seconds(void) {
    trig_inputs_t hi = in(true, false, false, false);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000, &hi));
    TEST_ASSERT_EQUAL(0, trig_step(&t, 2500, &hi));           // 1.5 s: not yet
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    TEST_ASSERT_EQUAL(0, trig_step(&t, 2900, &QUIET));         // dropped: a glitch, episode resets
    TEST_ASSERT_EQUAL(0, trig_step(&t, 3000, &hi));
    TEST_ASSERT_EQUAL(0, trig_step(&t, 4500, &hi));
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 5000, &hi));   // 2.0 s held
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));
    TEST_ASSERT_EQUAL(TRIG_CAUSE_CTRL, trig_cause(&t));
    TEST_ASSERT_EQUAL(5000, t.alarm_since);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 6000, &hi));            // reported once
    TEST_ASSERT_EQUAL(0, trig_step(&t, 9000, &QUIET));
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));            // stays in alarm when the line drops
}

void test_ctrl_counts_in_idle_others_do_not(void) {
    trig_init(&t, &cfg, BEACON_IDLE, 0);
    trig_inputs_t soft = in(false, true, true, true);
    for (uint64_t ms = 0; ms <= 20 * 60000; ms += 10000) TEST_ASSERT_EQUAL(0, trig_step(&t, ms, &soft));
    TEST_ASSERT_EQUAL(BEACON_IDLE, trig_state(&t));
    TEST_ASSERT_EQUAL(0, trig_cause(&t));
    trig_inputs_t hi = in(true, false, false, false);
    trig_step(&t, 2000000, &hi);
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 2002000, &hi));
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));
    // and with idle_honours_ctrl off, idle ignores everything
    cfg.idle_honours_ctrl = false;
    trig_init(&t, &cfg, BEACON_IDLE, 0);
    trig_step(&t, 0, &hi);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 5000, &hi));
    TEST_ASSERT_EQUAL(BEACON_IDLE, trig_state(&t));
}

void test_tamper_is_hard_and_fast(void) {
    trig_command(&t, TRIG_CMD_OK, 1000);                        // owner ok does not cover tamper
    trig_inputs_t hi = in(false, false, true, false);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000, &hi));
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 1200, &hi));
    TEST_ASSERT_EQUAL(TRIG_CAUSE_TAMPER, trig_cause(&t));
}

void test_robot_is_soft(void) {
    trig_inputs_t hi = in(false, true, false, false);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000, &hi));
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 1500, &hi));
    TEST_ASSERT_EQUAL(TRIG_CAUSE_ROBOT, trig_cause(&t));
    // with a recent owner ok it is ignored for the grace window
    setUp();
    trig_command(&t, TRIG_CMD_OK, 1000);
    trig_step(&t, 1000, &hi);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 2000, &hi));
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    // the line stays high through the grace window: once it expires the (already debounced) line
    // does not re-fire by itself; a new rising edge is needed
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000 + cfg.owner_ok_grace_ms + 1, &hi));
    trig_step(&t, 1000 + cfg.owner_ok_grace_ms + 2, &QUIET);
    trig_step(&t, 1000 + cfg.owner_ok_grace_ms + 3, &hi);
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 1000 + cfg.owner_ok_grace_ms + 600, &hi));
}

void test_motion_must_be_sustained(void) {
    trig_inputs_t mv = in(false, false, false, true);
    uint64_t ms = 1000;
    // 4 minutes of motion, then quiet for 2 minutes (longer than the 90 s gap): episode ends
    for (; ms <= 1000 + 4 * 60000; ms += 20000) TEST_ASSERT_EQUAL(0, trig_step(&t, ms, &mv));
    for (; ms <= 1000 + 6 * 60000; ms += 20000) TEST_ASSERT_EQUAL(0, trig_step(&t, ms, &QUIET));
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    TEST_ASSERT_EQUAL(0, t.motion_start);
    // 5 minutes of motion with short gaps (under 90 s) raises the alarm
    uint64_t start = ms;
    uint8_t ev = 0;
    for (; ms < start + 10 * 60000 && !ev; ms += 20000) {
        bool moving = ((ms - start) / 20000) % 4 != 3;           // one quiet sample in four (20 s gaps)
        trig_inputs_t i = in(false, false, false, moving);
        ev = trig_step(&t, ms, &i);
    }
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, ev);
    TEST_ASSERT_EQUAL(TRIG_CAUSE_MOTION, trig_cause(&t));
    TEST_ASSERT_TRUE(ms - start >= 5 * 60000);
    TEST_ASSERT_TRUE(ms - start < 6 * 60000);
}

void test_motion_respects_owner_ok(void) {
    trig_inputs_t mv = in(false, false, false, true);
    trig_command(&t, TRIG_CMD_OK, 1000);
    uint64_t ms = 1000;
    for (; ms <= 1000 + 20 * 60000; ms += 20000) TEST_ASSERT_EQUAL(0, trig_step(&t, ms, &mv));
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    // grace expires at 31 min; the motion episode has run since then → alarm
    uint8_t ev = 0;
    for (; ms <= 1000 + 40 * 60000 && !ev; ms += 20000) ev = trig_step(&t, ms, &mv);
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, ev);
    TEST_ASSERT_TRUE(ms - 1000 >= cfg.owner_ok_grace_ms);
    // an `ok` in the middle of an episode resets the episode
    setUp();
    ms = 1000;
    for (; ms <= 1000 + 3 * 60000; ms += 20000) trig_step(&t, ms, &mv);
    trig_command(&t, TRIG_CMD_OK, ms);
    TEST_ASSERT_EQUAL(0, t.motion_start);
}

void test_heartbeat_cadence(void) {
    TEST_ASSERT_FALSE(trig_heartbeat_due(&t, 1000));
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000 + 3599000, &QUIET));
    TEST_ASSERT_EQUAL(TRIG_EV_HEARTBEAT_DUE, trig_step(&t, 1000 + 3600000, &QUIET));
    TEST_ASSERT_EQUAL(TRIG_EV_HEARTBEAT_DUE, trig_step(&t, 1000 + 3600500, &QUIET));   // until sent
    trig_heartbeat_sent(&t, 1000 + 3600500);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 1000 + 3601000, &QUIET));
    TEST_ASSERT_EQUAL(TRIG_EV_HEARTBEAT_DUE, trig_step(&t, 1000 + 3600500 + 3600000, &QUIET));
    // no heartbeat in idle or alarm
    trig_command(&t, TRIG_CMD_DISARM, 2000);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 2000 + 10 * 3600000, &QUIET));
    trig_command(&t, TRIG_CMD_ARM, 3000);
    trig_inputs_t tamper = in(false, false, true, false);
    trig_step(&t, 3000, &tamper);
    trig_step(&t, 3300, &tamper);
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));
    TEST_ASSERT_EQUAL(0, trig_step(&t, 3000 + 10 * 3600000, &QUIET));
}

void test_commands(void) {
    trig_inputs_t tamper = in(false, false, true, false);
    trig_step(&t, 1000, &tamper);
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 1300, &tamper));
    TEST_ASSERT_FALSE(trig_command(&t, TRIG_CMD_PING, 2000));          // ping changes nothing
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));
    TEST_ASSERT_FALSE(trig_command(&t, TRIG_CMD_OK, 2000));            // ok does not end an alarm
    TEST_ASSERT_EQUAL(BEACON_ALARM, trig_state(&t));
    TEST_ASSERT_TRUE(trig_command(&t, TRIG_CMD_DISARM, 2500));
    TEST_ASSERT_EQUAL(BEACON_IDLE, trig_state(&t));
    TEST_ASSERT_EQUAL(0, trig_cause(&t));
    TEST_ASSERT_EQUAL(0, t.alarm_since);
    TEST_ASSERT_FALSE(trig_command(&t, TRIG_CMD_DISARM, 2600));        // already idle
    TEST_ASSERT_TRUE(trig_command(&t, TRIG_CMD_ARM, 3000));
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    TEST_ASSERT_EQUAL(3000, t.last_heartbeat);                         // heartbeat clock starts at arming
    // the tamper line is still high from before: no re-fire without a new edge (fired flag kept)
    TEST_ASSERT_EQUAL(0, trig_step(&t, 3100, &tamper));
    trig_step(&t, 3200, &QUIET);
    trig_step(&t, 3300, &tamper);
    TEST_ASSERT_EQUAL(TRIG_EV_ALARM, trig_step(&t, 3600, &tamper));
    // arm from alarm silences it and keeps watching
    TEST_ASSERT_TRUE(trig_command(&t, TRIG_CMD_ARM, 4000));
    TEST_ASSERT_EQUAL(BEACON_ARMED, trig_state(&t));
    TEST_ASSERT_FALSE(trig_command(&t, 99, 4000));                      // unknown command ignored
}

void test_causes_accumulate_in_alarm(void) {
    trig_inputs_t robot = in(false, true, false, false);
    trig_step(&t, 1000, &robot);
    trig_step(&t, 1500, &robot);
    TEST_ASSERT_EQUAL(TRIG_CAUSE_ROBOT, trig_cause(&t));
    trig_inputs_t both = in(true, true, false, false);
    trig_step(&t, 2000, &both);
    TEST_ASSERT_EQUAL(0, trig_step(&t, 4000, &both));                  // no second ALARM event
    TEST_ASSERT_EQUAL(TRIG_CAUSE_ROBOT | TRIG_CAUSE_CTRL, trig_cause(&t));
    TEST_ASSERT_EQUAL(1500, t.alarm_since);                            // first cause keeps the clock
}

void test_alarm_interval(void) {
    TEST_ASSERT_EQUAL(120000, trig_alarm_interval_ms(0));
    TEST_ASSERT_EQUAL(120000, trig_alarm_interval_ms(3599999));
    TEST_ASSERT_EQUAL(600000, trig_alarm_interval_ms(3600000));
    TEST_ASSERT_EQUAL(600000, trig_alarm_interval_ms(86400000ull * 3));
}

void test_next_wake(void) {
    // armed, nothing pending: the heartbeat, or the motion sample if sooner
    TEST_ASSERT_EQUAL_UINT64(3600000, trig_next_wake_ms(&t, 1000, 0));
    TEST_ASSERT_EQUAL_UINT64(20000, trig_next_wake_ms(&t, 1000, 20000));
    TEST_ASSERT_EQUAL_UINT64(1800000, trig_next_wake_ms(&t, 1000 + 1800000, 0));
    // a ctrl debounce in progress wins
    trig_inputs_t hi = in(true, false, false, false);
    trig_step(&t, 5000, &hi);
    TEST_ASSERT_EQUAL_UINT64(2000, trig_next_wake_ms(&t, 5000, 20000));
    TEST_ASSERT_EQUAL_UINT64(500, trig_next_wake_ms(&t, 6500, 20000));
    TEST_ASSERT_EQUAL_UINT64(0, trig_next_wake_ms(&t, 9000, 20000));
    // idle: nothing scheduled unless ctrl is being debounced
    trig_init(&t, &cfg, BEACON_IDLE, 0);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, trig_next_wake_ms(&t, 0, 20000));
    trig_inputs_t tamper = in(false, false, true, false);
    trig_step(&t, 0, &tamper);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, trig_next_wake_ms(&t, 0, 20000));    // tamper ignored in idle
    trig_step(&t, 100, &hi);
    TEST_ASSERT_EQUAL_UINT64(2000, trig_next_wake_ms(&t, 100, 20000));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_defaults);
    RUN_TEST(test_ctrl_needs_two_seconds);
    RUN_TEST(test_ctrl_counts_in_idle_others_do_not);
    RUN_TEST(test_tamper_is_hard_and_fast);
    RUN_TEST(test_robot_is_soft);
    RUN_TEST(test_motion_must_be_sustained);
    RUN_TEST(test_motion_respects_owner_ok);
    RUN_TEST(test_heartbeat_cadence);
    RUN_TEST(test_commands);
    RUN_TEST(test_causes_accumulate_in_alarm);
    RUN_TEST(test_alarm_interval);
    RUN_TEST(test_next_wake);
    return UNITY_END();
}
