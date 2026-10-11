#include "triggers.h"
#include <string.h>

void trig_default_config(trig_config_t *c) {
    c->ctrl_hold_ms = 2000;
    c->robot_hold_ms = 500;
    c->tamper_hold_ms = 200;
    c->motion_minutes = 5;
    c->motion_gap_ms = 90000;
    c->owner_ok_grace_ms = 30u * 60u * 1000u;
    c->heartbeat_ms = 3600u * 1000u;
    c->idle_honours_ctrl = true;
}

void trig_init(trig_t *t, const trig_config_t *cfg, beacon_state_t initial, uint64_t now) {
    memset(t, 0, sizeof *t);
    t->cfg = *cfg;
    t->state = initial;
    t->last_heartbeat = now;
    if (initial == BEACON_ALARM) t->alarm_since = now;
}

// Debounce one digital line. Returns true once per high episode, when it has been high ≥ hold.
static bool debounce(bool level, uint64_t now, uint32_t hold, uint64_t *since, bool *fired) {
    if (!level) { *since = 0; *fired = false; return false; }
    if (*since == 0) *since = now ? now : 1;
    if (!*fired && now - *since >= hold) { *fired = true; return true; }
    return false;
}

static bool soft_allowed(const trig_t *t, uint64_t now) {
    return t->owner_ok_at == 0 || now - t->owner_ok_at >= t->cfg.owner_ok_grace_ms;
}

static void raise(trig_t *t, uint64_t now, uint8_t bit, uint8_t *ev) {
    t->cause |= bit;
    if (t->state != BEACON_ALARM) {
        t->state = BEACON_ALARM;
        t->alarm_since = now;
        *ev |= TRIG_EV_ALARM;
    }
}

uint8_t trig_step(trig_t *t, uint64_t now, const trig_inputs_t *in) {
    uint8_t ev = 0;
    const trig_config_t *c = &t->cfg;

    bool ctrl = debounce(in->ctrl, now, c->ctrl_hold_ms, &t->ctrl_since, &t->ctrl_fired);
    bool robot = debounce(in->robot, now, c->robot_hold_ms, &t->robot_since, &t->robot_fired);
    bool tamper = debounce(in->tamper, now, c->tamper_hold_ms, &t->tamper_since, &t->tamper_fired);

    // motion episode bookkeeping
    bool motion = false;
    if (in->moving) {
        if (t->motion_start == 0) t->motion_start = now ? now : 1;
        t->motion_last = now;
    } else if (t->motion_start && now - t->motion_last > c->motion_gap_ms) {
        t->motion_start = 0;
        t->motion_last = 0;
    }
    // sustained = time between the first and the latest movement of the episode (pauses inside the
    // gap tolerance do not end it, but they do not count as movement either)
    if (t->motion_start && t->motion_last - t->motion_start >= (uint64_t)c->motion_minutes * 60000u) motion = true;

    bool ctrl_counts = t->state != BEACON_IDLE || c->idle_honours_ctrl;
    bool armed_or_alarm = t->state == BEACON_ARMED || t->state == BEACON_ALARM;

    if (ctrl && ctrl_counts) raise(t, now, TRIG_CAUSE_CTRL, &ev);
    if (tamper && armed_or_alarm) raise(t, now, TRIG_CAUSE_TAMPER, &ev);
    if (armed_or_alarm && soft_allowed(t, now)) {
        if (robot) raise(t, now, TRIG_CAUSE_ROBOT, &ev);
        if (motion && !(t->cause & TRIG_CAUSE_MOTION)) raise(t, now, TRIG_CAUSE_MOTION, &ev);
    }

    if (t->state == BEACON_ARMED && trig_heartbeat_due(t, now)) ev |= TRIG_EV_HEARTBEAT_DUE;
    return ev;
}

bool trig_heartbeat_due(const trig_t *t, uint64_t now) {
    return t->state == BEACON_ARMED && now - t->last_heartbeat >= t->cfg.heartbeat_ms;
}

void trig_heartbeat_sent(trig_t *t, uint64_t now) { t->last_heartbeat = now; }

bool trig_command(trig_t *t, uint8_t cmd, uint64_t now) {
    beacon_state_t before = t->state;
    switch (cmd) {
        case TRIG_CMD_ARM:
            t->state = BEACON_ARMED;
            t->cause = 0;
            t->alarm_since = 0;
            t->last_heartbeat = now;
            t->motion_start = t->motion_last = 0;
            break;
        case TRIG_CMD_DISARM:
            t->state = BEACON_IDLE;
            t->cause = 0;
            t->alarm_since = 0;
            t->motion_start = t->motion_last = 0;
            break;
        case TRIG_CMD_OK:
            t->owner_ok_at = now ? now : 1;
            t->motion_start = t->motion_last = 0;   // the owner says this movement is theirs
            break;
        default:
            break;
    }
    return t->state != before;
}

beacon_state_t trig_state(const trig_t *t) { return t->state; }
uint8_t trig_cause(const trig_t *t) { return t->cause; }

uint32_t trig_alarm_interval_ms(uint64_t since_alarm_ms) {
    return since_alarm_ms < 3600000u ? 120000u : 600000u;
}

static void consider(uint64_t *best, uint64_t candidate) { if (candidate < *best) *best = candidate; }
static uint64_t remaining(uint64_t now, uint64_t since, uint32_t hold) { return now - since >= hold ? 0 : hold - (now - since); }

uint64_t trig_next_wake_ms(const trig_t *t, uint64_t now, uint32_t sample_ms) {
    uint64_t best = UINT64_MAX;
    const trig_config_t *c = &t->cfg;
    if (t->ctrl_since && !t->ctrl_fired) consider(&best, remaining(now, t->ctrl_since, c->ctrl_hold_ms));
    if (t->state != BEACON_IDLE) {
        if (t->robot_since && !t->robot_fired) consider(&best, remaining(now, t->robot_since, c->robot_hold_ms));
        if (t->tamper_since && !t->tamper_fired) consider(&best, remaining(now, t->tamper_since, c->tamper_hold_ms));
    }
    if (t->state == BEACON_ARMED) {
        consider(&best, remaining(now, t->last_heartbeat, c->heartbeat_ms));
        if (sample_ms) consider(&best, sample_ms);
    }
    return best;
}
