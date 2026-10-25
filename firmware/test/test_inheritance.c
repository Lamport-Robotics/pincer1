/* Inheritance dead-man's switch: powered-uptime timer, arming / release / clear, warnings, survival of
 * lock / duress / wipe, the separate flash record, the durable-nonce check and the protocol messages. */
#include "consent.h"
#include "harness.h"
#include "msgbuild.h"
#include "proto.h"
#include "solmsg.h"
#include "storage.h"
#include "util.h"

#define SOL 1000000000ull
#define T0 1791600000ull
#define DAY_MS 86400000u
#define DAY_S 86400ull

static uint8_t SYS[32], OWNER[32], NONCE[32], HEIR[32], SYSVAR[32], OTHER[32];
static const uint8_t PIN[8] = {'P', 'R', 5, 0x04, (uint8_t)~5, (uint8_t)~0x04, 0x5A, 0xA5};
static const uint8_t (*P_SYS)[32] = (const uint8_t (*)[32])SYS;

/* initialised + unlocked controller, fresh (never saved) timer */
static void ready(consent_t *c, uint32_t *ms)
{
    consent_persist_t p;
    consent_action_t a;
    consent_reason_t why;
    memset(&p, 0, sizeof p);
    p.initialized = true; p.pin_set = true; p.daily_limit = 2 * SOL;
    *ms = 1000;
    consent_init(c, &p, *ms);
    consent_init_inheritance(c, NULL);
    consent_set_time(c, T0, *ms);
    CHECK_EQ(consent_begin_unlock(c, "u0", *ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(c, PIN, *ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(c, true, *ms, &a), CONSENT_PIN_OK);
    CHECK(!c->locked);
    c->dirty = false;
}

/* Drive the 5 ms tick forward by whole days; the uint32 tick wraps every 49.7 days, the counter must not care. */
static void advance_days(consent_t *c, uint32_t *ms, unsigned days)
{
    consent_action_t a;
    while (days--) {
        *ms += DAY_MS;
        consent_tick(c, *ms, &a);
    }
}
static void advance_s(consent_t *c, uint32_t *ms, uint32_t seconds)
{
    consent_action_t a;
    *ms += seconds * 1000u;
    consent_tick(c, *ms, &a);
}

static consent_inheritance_t req90(void)
{
    consent_inheritance_t r;
    memset(&r, 0, sizeof r);
    r.inactivity_days = 90;
    r.n_warn = 3; r.warn_days[0] = 14; r.warn_days[1] = 7; r.warn_days[2] = 1;
    memcpy(r.heir, HEIR, 32);
    memcpy(r.signer, OWNER, 32);
    return r;
}

/* arm with a long touch; returns the action the firmware would sign with */
static void arm(consent_t *c, uint32_t ms, consent_action_t *out)
{
    consent_inheritance_t r = req90();
    consent_reason_t why;
    CHECK_EQ(consent_begin_inheritance_set(c, "h1", &r, true, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(c, 0, 1);
    CHECK_EQ(consent_pending_need(c), CONSENT_NEED_LONG_TOUCH);
    CHECK(!consent_touch(c, false, ms, out)); /* a tap is not enough to arm */
    CHECK(consent_touch(c, true, ms, out));
    CHECK_EQ(out->op, CONSENT_OP_INHERITANCE_SET);
    CHECK_EQ(out->slot, CONSENT_SLOT_REAL);
    CHECK(!c->ih.inh.armed); /* the caller arms after the signature exists */
    consent_commit_inheritance(c, out);
    CHECK(c->ih.inh.armed && c->ih_dirty);
    CHECK_EQ(c->ih.inh.inactivity_days, 90);
    CHECK_EQ(c->ih.inh.n_warn, 3);
    CHECK_MEM(c->ih.inh.heir, HEIR, 32);
    CHECK_MEM(c->ih.inh.signer, OWNER, 32);
    consent_inheritance_persisted(c);
}

/* ---- arming ---- */

static void test_arm_requires_long_touch_and_nonce(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    consent_inheritance_t r;
    uint32_t ms;
    ready(&c, &ms);
    r = req90();
    /* not a durable-nonce message -> policy, nothing pending */
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, false, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    /* bad parameters */
    r.inactivity_days = 0;
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    r = req90(); r.warn_days[0] = 90; /* threshold must be below the period */
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    r = req90(); r.warn_days[2] = 0;
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    r = req90(); r.n_warn = 5;
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    /* locked / duress / not initialised */
    r = req90();
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_LOCKED);
    ready(&c, &ms);
    consent_duress(&c, NULL);
    /* in duress the request goes through like in the real slot; the action carries the decoy slot, and
     * main.c signs with the decoy key and stores nothing (the real arrangement is never touched) */
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_INHERITANCE_SET);
    CHECK_EQ(a.slot, CONSENT_SLOT_DECOY);
    CHECK(!c.ih.inh.armed);
    consent_init(&c, NULL, 0);
    consent_init_inheritance(&c, NULL);
    CHECK_EQ(consent_begin_inheritance_set(&c, "h0", &r, true, 0, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    /* the happy path */
    ready(&c, &ms);
    arm(&c, ms, &a);
    CHECK_STR(a.id, "h1");
    CHECK_EQ(a.inh.inactivity_days, 90);
    CHECK_MEM(a.inh.heir, HEIR, 32);
    CHECK_U64(c.ih.last_touch_powered_seconds, c.ih.powered_seconds);
    /* pending set times out like anything else */
    advance_days(&c, &ms, 1);
    CHECK_EQ(consent_begin_inheritance_set(&c, "h2", &r, true, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH); /* replacing an arrangement: a plain touch */
    CHECK(consent_tick(&c, ms + CONSENT_DEFAULT_TTL_MS, &a));
    CHECK_EQ(a.op, CONSENT_OP_INHERITANCE_SET);
    CHECK_STR(a.id, "h2");
    CHECK_EQ(c.ih.inh.inactivity_days, 90); /* the old arrangement is untouched */
    /* a different key cannot replace it (wiped + re-initialised controller) */
    ms += CONSENT_DEFAULT_TTL_MS;
    memcpy(r.signer, OTHER, 32);
    CHECK_EQ(consent_begin_inheritance_set(&c, "h3", &r, true, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    memcpy(r.signer, OWNER, 32);
    /* replace with a tap */
    r.inactivity_days = 30; r.n_warn = 1; r.warn_days[0] = 3;
    CHECK_EQ(consent_begin_inheritance_set(&c, "h3", &r, true, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    consent_commit_inheritance(&c, &a);
    CHECK_EQ(c.ih.inh.inactivity_days, 30);
    CHECK_EQ(c.ih.inh.n_warn, 1);
    CHECK_EQ(c.ih.inh.warned, 0);
}

static void test_durable_nonce_detection(void)
{
    mb_t m;
    solmsg_t p;
    uint8_t keys[5][32];
    uint8_t d[64], nonce[32];
    uint8_t adv[3] = {1, 2, 0}, xfer[2] = {0, 3};
    memcpy(keys[0], OWNER, 32); memcpy(keys[1], NONCE, 32); memcpy(keys[2], SYSVAR, 32);
    memcpy(keys[3], HEIR, 32); memcpy(keys[4], SYS, 32);
    /* advanceNonce first, then the transfer to the heir */
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5);
    mb_n_ix(&m, 2);
    wr_le32(d, 4);
    mb_ix(&m, 4, adv, 3, d, 4);
    mb_ix(&m, 4, xfer, 2, d, ix_sys_transfer(d, 5 * SOL));
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK(solmsg_starts_with_advance_nonce(&p, nonce));
    CHECK_MEM(nonce, NONCE, 32);
    CHECK(memcmp(nonce, OWNER, 32) != 0);
    {
        solmsg_summary_t s;
        solmsg_analyze(&p, OWNER, &s);
        CHECK_U64(s.lamports_out, 5 * SOL);
        CHECK(s.has_dest);
        CHECK_MEM(s.dest, HEIR, 32);
        CHECK(s.fee_payer_is_owner);
    }
    /* transfer first -> not durable */
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5);
    mb_n_ix(&m, 2);
    mb_ix(&m, 4, xfer, 2, d, ix_sys_transfer(d, 5 * SOL));
    wr_le32(d, 4);
    mb_ix(&m, 4, adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK(!solmsg_starts_with_advance_nonce(&p, nonce));
    /* a nonce account that is the owner key itself is detected as such by the caller */
    adv[0] = 0;
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5);
    mb_n_ix(&m, 1);
    wr_le32(d, 4);
    mb_ix(&m, 4, adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK);
    CHECK(solmsg_starts_with_advance_nonce(&p, nonce));
    CHECK_MEM(nonce, OWNER, 32); /* main.c refuses this */
    /* wrong tag / short data / too few accounts / wrong program */
    adv[0] = 1;
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5); mb_n_ix(&m, 1); wr_le32(d, 5); mb_ix(&m, 4, adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); CHECK(!solmsg_starts_with_advance_nonce(&p, NULL));
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5); mb_n_ix(&m, 1); wr_le32(d, 4); mb_ix(&m, 4, adv, 3, d, 3);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); CHECK(!solmsg_starts_with_advance_nonce(&p, NULL));
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5); mb_n_ix(&m, 1); wr_le32(d, 4); mb_ix(&m, 4, adv, 2, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); CHECK(!solmsg_starts_with_advance_nonce(&p, NULL));
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5); mb_n_ix(&m, 1); wr_le32(d, 4); mb_ix(&m, 3, adv, 3, d, 4);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); CHECK(!solmsg_starts_with_advance_nonce(&p, NULL));
    mb_begin(&m, -1, 1, 0, 3, (const uint8_t (*)[32])keys, 5); mb_n_ix(&m, 0);
    CHECK_EQ(solmsg_parse(m.buf, m.len, &p), SOLMSG_OK); CHECK(!solmsg_starts_with_advance_nonce(&p, NULL));
}

/* ---- timer math, release, touches ---- */

static void test_status_math_and_release(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    uint64_t p0;
    ready(&c, &ms);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_ARMED);
    CHECK_STR(consent_reason_str(why), "not_armed");
    arm(&c, ms, &a);
    p0 = c.ih.powered_seconds;
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0);
    /* 12 days + 0.4 day */
    advance_days(&c, &ms, 12);
    advance_s(&c, &ms, (uint32_t)(DAY_S * 4 / 10));
    CHECK_U64(c.ih.powered_seconds, p0 + 12 * DAY_S + DAY_S * 4 / 10);
    CHECK_U64(consent_seconds_since_touch(&c), 12 * DAY_S + DAY_S * 4 / 10);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 124);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_EXPIRED);
    CHECK_STR(consent_reason_str(why), "not_expired");
    /* one second before 90 days: still denied; at 90 days: released */
    advance_days(&c, &ms, 77);
    advance_s(&c, &ms, (uint32_t)(DAY_S * 6 / 10) - 1);
    CHECK_U64(consent_seconds_since_touch(&c), 90 * DAY_S - 1);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_EXPIRED);
    advance_s(&c, &ms, 1);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 900);
    /* release is a read: still armed, still released afterwards, and host time was never involved */
    CHECK(c.ih.inh.armed);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
    c.have_time = false;
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
    /* a touch (even a thief's, even while locked) restarts the clock */
    consent_lock(&c, NULL);
    consent_touch_seen(&c);
    CHECK_U64(consent_seconds_since_touch(&c), 0);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_EXPIRED);
}

static void test_touches_reset_timer(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    arm(&c, ms, &a);
    advance_days(&c, &ms, 40);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 400);
    /* a short touch with nothing pending */
    CHECK(!consent_touch(&c, false, ms, &a));
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0);
    advance_days(&c, &ms, 40);
    /* a touch that approves a sign */
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0);
    advance_days(&c, &ms, 40);
    /* a long touch */
    CHECK(!consent_touch(&c, true, ms, &a));
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0);
    advance_days(&c, &ms, 40);
    /* a rhythm tap: the firmware reports every debounced press through consent_touch_seen() */
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_unlock(&c, "u1", ms, &why), CONSENT_R_PENDING);
    CHECK(!consent_touch(&c, false, ms, &a)); /* ignored as approval ... */
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0); /* ... but counted as life */
    advance_days(&c, &ms, 40);
    consent_touch_seen(&c);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 0);
    /* 89 days of silence, one tap, 89 more: never expires */
    advance_days(&c, &ms, 89);
    consent_touch_seen(&c);
    advance_days(&c, &ms, 89);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    advance_days(&c, &ms, 1);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
}

static void test_warnings_once_each(void)
{
    consent_t c;
    consent_action_t a;
    uint32_t ms, d;
    ready(&c, &ms);
    CHECK(!consent_inheritance_warning(&c, &d)); /* not armed */
    arm(&c, ms, &a);
    CHECK(!consent_inheritance_warning(&c, &d));
    advance_days(&c, &ms, 75);
    CHECK(!consent_inheritance_warning(&c, &d)); /* 15 days left: nothing yet */
    advance_days(&c, &ms, 1);
    c.ih_dirty = false;
    CHECK(consent_inheritance_warning(&c, &d)); /* exactly 14 left */
    CHECK_EQ(d, 14);
    CHECK(c.ih_dirty); /* `warned` is persisted */
    CHECK_EQ(c.ih.inh.warned, 1);
    CHECK(!consent_inheritance_warning(&c, &d)); /* once */
    advance_days(&c, &ms, 3);
    CHECK(!consent_inheritance_warning(&c, &d));
    advance_days(&c, &ms, 4);
    CHECK(consent_inheritance_warning(&c, &d)); CHECK_EQ(d, 7);
    CHECK(!consent_inheritance_warning(&c, &d));
    advance_days(&c, &ms, 6);
    CHECK(consent_inheritance_warning(&c, &d)); CHECK_EQ(d, 1);
    CHECK(!consent_inheritance_warning(&c, &d));
    CHECK_EQ(c.ih.inh.warned, 7);
    advance_days(&c, &ms, 5); /* past expiry: no further events */
    CHECK(!consent_inheritance_warning(&c, &d));
    /* a touch starts a new cycle: the thresholds fire again */
    consent_touch_seen(&c);
    CHECK_EQ(c.ih.inh.warned, 0);
    advance_days(&c, &ms, 83);
    CHECK(consent_inheritance_warning(&c, &d)); CHECK_EQ(d, 14);
    CHECK(consent_inheritance_warning(&c, &d)); CHECK_EQ(d, 7); /* two thresholds crossed at once: two events */
    CHECK(!consent_inheritance_warning(&c, &d));
    /* warnings persist across a reboot: the emitted ones are not repeated */
    {
        consent_inh_persist_t saved = c.ih;
        consent_persist_t p = c.p;
        consent_init(&c, &p, ms);
        consent_init_inheritance(&c, &saved);
        CHECK(!consent_inheritance_warning(&c, &d));
        advance_days(&c, &ms, 6);
        CHECK(consent_inheritance_warning(&c, &d)); CHECK_EQ(d, 1);
    }
}

/* ---- survival ---- */

static void test_lock_duress_wipe_keep_inheritance(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    int i;
    ready(&c, &ms);
    arm(&c, ms, &a);
    advance_days(&c, &ms, 30);
    consent_inheritance_persisted(&c); /* hourly checkpoints during those 30 days were written */
    consent_lock(&c, NULL);
    CHECK(c.ih.inh.armed);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 300);
    consent_duress(&c, NULL);
    CHECK(c.ih.inh.armed);
    CHECK(!c.ih_dirty); /* neither lock nor duress touched the record */
    /* the PIN wipe: 5 wrong rhythms */
    for (i = 0; i < 4; i++) {
        consent_begin_unlock(&c, "u", ms, &why);
        consent_rhythm(&c, PIN, ms, &a);
        CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
    }
    consent_begin_unlock(&c, "u", ms, &why);
    consent_rhythm(&c, PIN, ms, &a);
    CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WIPE);
    CHECK(!c.p.initialized);
    CHECK(c.ih.inh.armed);
    CHECK_EQ(c.ih.inh.inactivity_days, 90);
    CHECK_MEM(c.ih.inh.heir, HEIR, 32);
    CHECK_EQ(consent_days_since_touch_tenths(&c), 300);
    consent_wipe(&c);
    CHECK(c.ih.inh.armed);
    /* the timer keeps running and the release still works on the wiped device */
    advance_days(&c, &ms, 60);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
    /* the wiped (factory-unlocked) device cannot clear it ... */
    CHECK_EQ(consent_begin_inheritance_clear(&c, "x1", OWNER, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    /* ... and neither can a thief who re-initialises it with fresh keys: bound to the signing key */
    consent_begin_init(&c, "i", ms, &why);
    consent_touch(&c, true, ms, &a);
    consent_mark_initialized(&c);
    consent_begin_set_pin(&c, "n", ms, &why);
    consent_rhythm(&c, PIN, ms, &a);
    consent_rhythm(&c, PIN, ms, &a);
    consent_mark_pin_set(&c);
    CHECK(!c.locked);
    CHECK_EQ(consent_begin_inheritance_clear(&c, "x2", OTHER, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    {
        consent_inheritance_t r = req90();
        memcpy(r.signer, OTHER, 32);
        CHECK_EQ(consent_begin_inheritance_set(&c, "x3", &r, true, ms, &why), CONSENT_R_DENIED);
        consent_display_shown(&c, 0, 1);
        CHECK_EQ(why, CONSENT_DENY_POLICY);
    }
    CHECK(c.ih.inh.armed);
    /* the touches above restarted the clock, as any touch does; it simply runs again */
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_EXPIRED);
    advance_days(&c, &ms, 90);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DONE);
}

static void test_clear(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    uint64_t powered_before;
    ready(&c, &ms);
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c0", OWNER, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_ARMED);
    arm(&c, ms, &a);
    advance_days(&c, &ms, 2);
    powered_before = c.ih.powered_seconds;
    /* locked / duress cannot clear */
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c1", OWNER, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_LOCKED);
    CHECK_EQ(consent_begin_unlock(&c, "u", ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a);
    consent_pin_result(&c, true, ms, &a);
    consent_duress(&c, NULL);
    /* in duress a clear looks exactly like the real thing and changes nothing */
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c2", OWNER, ms, &why), CONSENT_R_PENDING);
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_INHERITANCE_CLEAR);
    CHECK(c.ih.inh.armed);
    CHECK_EQ(consent_begin_unlock(&c, "u", ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a);
    consent_pin_result(&c, true, ms, &a);
    /* the wrong key cannot clear */
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c2b", OTHER, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    /* long touch required */
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c3", OWNER, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_LONG_TOUCH);
    CHECK(!consent_touch(&c, false, ms, &a));
    CHECK(c.ih.inh.armed);
    c.ih_dirty = false;
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_INHERITANCE_CLEAR);
    CHECK_STR(a.id, "c3");
    CHECK(!c.ih.inh.armed);
    CHECK_EQ(c.ih.inh.inactivity_days, 0);
    CHECK(c.ih_dirty);
    CHECK_EQ(consent_inheritance_release(&c, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_ARMED);
    CHECK_EQ(consent_begin_inheritance_clear(&c, "c4", OWNER, ms, &why), CONSENT_R_DENIED);
    /* the uptime counter itself is not reset by a clear */
    CHECK_U64(c.ih.powered_seconds, powered_before);
}

/* ---- counter persistence ---- */

static void test_counter_never_goes_backwards(void)
{
    consent_t c;
    consent_persist_t p;
    consent_inh_persist_t saved;
    uint32_t ms = 0;
    memset(&p, 0, sizeof p);
    p.initialized = true; p.pin_set = true;
    consent_init(&c, &p, ms);
    consent_init_inheritance(&c, NULL);
    CHECK_U64(c.ih.powered_seconds, 0);
    CHECK(!c.ih_dirty);
    /* sub-second ticks accumulate without loss */
    { int i; for (i = 0; i < 200; i++) { ms += 5; advance_s(&c, &ms, 0); } }
    CHECK_U64(c.ih.powered_seconds, 1);
    CHECK_EQ(c.tick_frac_ms, 0);
    /* the hourly checkpoint */
    advance_s(&c, &ms, 3598);
    CHECK(!c.ih_dirty);
    advance_s(&c, &ms, 1);
    CHECK_U64(c.ih.powered_seconds, 3600);
    CHECK(c.ih_dirty);
    saved = c.ih; /* firmware: storage_inh_save */
    consent_inheritance_persisted(&c);
    CHECK(!c.ih_dirty);
    advance_s(&c, &ms, 1800); /* 30 min later: power cut without a save */
    CHECK_U64(c.ih.powered_seconds, 5400);
    /* reset: resume from the stored value; the unsaved half hour is lost but nothing rewinds */
    consent_init(&c, &p, 777);
    consent_init_inheritance(&c, &saved);
    CHECK_U64(c.ih.powered_seconds, 3600);
    ms = 777;
    advance_s(&c, &ms, 10);
    CHECK_U64(c.ih.powered_seconds, 3610);
    /* main-record writes carry the counter along: the firmware does that through ih_persisted_seconds */
    CHECK(c.ih.powered_seconds != c.ih_persisted_seconds);
    /* a stored last_touch in the future (torn write, clock games) is clamped, never negative */
    saved.powered_seconds = 100; saved.last_touch_powered_seconds = 500;
    consent_init(&c, &p, 0);
    consent_init_inheritance(&c, &saved);
    CHECK_U64(c.ih.last_touch_powered_seconds, 100);
    CHECK_U64(consent_seconds_since_touch(&c), 0);
    /* a touch pulls the checkpoint forward only when one is due (no flash write per rhythm tap) */
    consent_init(&c, &p, 0);
    consent_init_inheritance(&c, NULL);
    ms = 0;
    advance_s(&c, &ms, 100);
    consent_touch_seen(&c);
    CHECK(!c.ih_dirty);
    advance_s(&c, &ms, 3500);
    CHECK(c.ih_dirty);
    /* 32-bit tick wrap: 50 days in one-day steps */
    consent_init(&c, &p, 0xFFFF0000u);
    consent_init_inheritance(&c, NULL);
    ms = 0xFFFF0000u;
    advance_days(&c, &ms, 50);
    CHECK_U64(c.ih.powered_seconds, 50 * DAY_S);
}

/* ---- storage: the inheritance region ---- */

static uint8_t g_flash[STORAGE_INH_REGION_SIZE];
static int g_fail_program;
static int mock_read(uint32_t off, void *buf, size_t len)
{
    if (off + len > sizeof g_flash) return -1;
    memcpy(buf, g_flash + off, len);
    return 0;
}
static int mock_erase(uint32_t off)
{
    if (off % STORAGE_SECTOR_SIZE || off + STORAGE_SECTOR_SIZE > sizeof g_flash) return -1;
    memset(g_flash + off, 0xff, STORAGE_SECTOR_SIZE);
    return 0;
}
static int mock_program(uint32_t off, const void *buf, size_t len)
{
    size_t i;
    if (off % STORAGE_PAGE_SIZE || len % STORAGE_PAGE_SIZE || off + len > sizeof g_flash) return -1;
    if (g_fail_program) {
        for (i = 0; i < STORAGE_PAGE_SIZE; i++) g_flash[off + i] &= ((const uint8_t *)buf)[i];
        return -1;
    }
    for (i = 0; i < len; i++) g_flash[off + i] &= ((const uint8_t *)buf)[i];
    return 0;
}
static const storage_flash_t FL = {mock_read, mock_erase, mock_program};

static void fill_inh(storage_inh_data_t *d, int v)
{
    size_t i;
    memset(d, 0, sizeof *d);
    d->ih.powered_seconds = 1234567890123ull + (uint64_t)v;
    d->ih.last_touch_powered_seconds = 1234567000000ull + (uint64_t)v;
    d->ih.inh.armed = true;
    d->ih.inh.inactivity_days = (uint16_t)(90 + v);
    d->ih.inh.n_warn = 3;
    d->ih.inh.warn_days[0] = 14; d->ih.inh.warn_days[1] = 7; d->ih.inh.warn_days[2] = 1;
    d->ih.inh.warned = (uint8_t)(v & 7);
    memcpy(d->ih.inh.heir, HEIR, 32);
    memcpy(d->ih.inh.signer, OWNER, 32);
    d->msg_len = (uint16_t)(300 + v);
    for (i = 0; i < d->msg_len; i++) d->msg[i] = (uint8_t)(i * 7 + v);
    for (i = 0; i < 64; i++) d->sig[i] = (uint8_t)(0xA0 + i + v);
}

static void test_storage_round_trip(void)
{
    storage_inh_data_t d, back;
    uint8_t buf[STORAGE_INH_RECORD_SIZE];
    uint32_t seq = 0;
    size_t n;
    CHECK(STORAGE_INH_RECORD_SIZE <= STORAGE_SECTOR_SIZE);
    CHECK(STORAGE_INH_RECORD_SIZE % STORAGE_PAGE_SIZE == 0);
    fill_inh(&d, 5);
    n = storage_inh_encode(&d, 42, buf, sizeof buf);
    CHECK(n > 0 && n <= STORAGE_INH_RECORD_SIZE);
    CHECK(storage_inh_decode(buf, sizeof buf, &back, &seq));
    CHECK_EQ(seq, 42);
    CHECK_U64(back.ih.powered_seconds, d.ih.powered_seconds);
    CHECK_U64(back.ih.last_touch_powered_seconds, d.ih.last_touch_powered_seconds);
    CHECK(back.ih.inh.armed);
    CHECK_EQ(back.ih.inh.inactivity_days, 95);
    CHECK_EQ(back.ih.inh.n_warn, 3);
    CHECK_EQ(back.ih.inh.warn_days[0], 14); CHECK_EQ(back.ih.inh.warn_days[1], 7); CHECK_EQ(back.ih.inh.warn_days[2], 1);
    CHECK_EQ(back.ih.inh.warned, 5);
    CHECK_MEM(back.ih.inh.heir, HEIR, 32);
    CHECK_MEM(back.ih.inh.signer, OWNER, 32);
    CHECK_EQ(back.msg_len, 305);
    CHECK_MEM(back.msg, d.msg, sizeof d.msg);
    CHECK_MEM(back.sig, d.sig, 64);
    /* the biggest message that fits the chip */
    d.msg_len = CONSENT_INH_MSG_MAX;
    CHECK(storage_inh_encode(&d, 1, buf, sizeof buf) > 0);
    CHECK(storage_inh_decode(buf, sizeof buf, &back, &seq));
    CHECK_EQ(back.msg_len, CONSENT_INH_MSG_MAX);
    d.msg_len = CONSENT_INH_MSG_MAX + 1;
    CHECK_EQ(storage_inh_encode(&d, 1, buf, sizeof buf), 0);
    d.msg_len = 10;
    CHECK_EQ(storage_inh_encode(&d, 1, buf, 900), 0);
    /* armed without a transaction loads as disarmed */
    d.msg_len = 0;
    CHECK(storage_inh_encode(&d, 1, buf, sizeof buf) > 0);
    CHECK(storage_inh_decode(buf, sizeof buf, &back, &seq));
    CHECK(!back.ih.inh.armed);
    /* a bare timer record (never armed) round-trips */
    memset(&d, 0, sizeof d);
    d.ih.powered_seconds = 3600;
    CHECK(storage_inh_encode(&d, 7, buf, sizeof buf) > 0);
    CHECK(storage_inh_decode(buf, sizeof buf, &back, &seq));
    CHECK_U64(back.ih.powered_seconds, 3600);
    CHECK(!back.ih.inh.armed);
    /* CRC, magic, version */
    fill_inh(&d, 1);
    n = storage_inh_encode(&d, 3, buf, sizeof buf);
    buf[40] ^= 1;
    CHECK(!storage_inh_decode(buf, sizeof buf, &back, &seq));
    buf[40] ^= 1;
    CHECK(storage_inh_decode(buf, sizeof buf, &back, &seq));
    buf[n - 1] ^= 0x80;
    CHECK(!storage_inh_decode(buf, sizeof buf, &back, &seq));
    buf[n - 1] ^= 0x80;
    buf[4] = 2;
    CHECK(!storage_inh_decode(buf, sizeof buf, &back, &seq));
    /* the main-record decoder does not accept an inheritance record and vice versa */
    {
        storage_data_t md;
        uint8_t mbuf[STORAGE_RECORD_SIZE];
        memset(&md, 0, sizeof md);
        CHECK(storage_encode(&md, 1, mbuf, sizeof mbuf) > 0);
        CHECK(!storage_inh_decode(mbuf, sizeof mbuf, &back, &seq));
        buf[4] = 1;
        CHECK(!storage_decode(buf, sizeof buf, &md, &seq));
    }
}

static void test_storage_slots_and_power_loss(void)
{
    storage_t s;
    storage_inh_data_t d, back;
    int i;
    memset(g_flash, 0xff, sizeof g_flash);
    storage_inh_init(&s, &FL);
    CHECK(!storage_inh_load(&s, &back));
    CHECK(!back.ih.inh.armed);
    for (i = 1; i <= 5; i++) {
        fill_inh(&d, i);
        CHECK_EQ(storage_inh_save(&s, &d), 0);
        CHECK_EQ(s.newest_slot, (i - 1) % 2);
        CHECK_EQ(s.seq, i);
    }
    storage_inh_init(&s, &FL);
    CHECK(storage_inh_load(&s, &back));
    CHECK_EQ(s.seq, 5);
    CHECK_EQ(back.ih.inh.inactivity_days, 95);
    CHECK_EQ(back.msg_len, 305);
    /* torn write keeps the previous record */
    fill_inh(&d, 6);
    g_fail_program = 1;
    CHECK(storage_inh_save(&s, &d) != 0);
    g_fail_program = 0;
    storage_inh_init(&s, &FL);
    CHECK(storage_inh_load(&s, &back));
    CHECK_EQ(back.ih.inh.inactivity_days, 95);
    fill_inh(&d, 7);
    CHECK_EQ(storage_inh_save(&s, &d), 0);
    storage_inh_init(&s, &FL);
    CHECK(storage_inh_load(&s, &back));
    CHECK_EQ(back.ih.inh.inactivity_days, 97);
    /* the firmware's wipe writes an all-zero MAIN record; this region is simply not written, so a
     * fresh boot still finds the arrangement and a monotonic counter */
    storage_inh_init(&s, &FL);
    CHECK(storage_inh_load(&s, &back));
    CHECK(back.ih.inh.armed);
    CHECK_U64(back.ih.powered_seconds, 1234567890123ull + 7);
}

/* ---- protocol ---- */

static void test_proto_messages(void)
{
    proto_req_t r;
    char out[2600];
    const char *line =
        "{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQIDBA==\","
        "\"claims\":{\"lamports_out\":\"5000000000\",\"dest\":\"11111111111111111111111111111111\",\"programs\":[\"11111111111111111111111111111111\"]},"
        "\"inactivity_days\":90,\"warn_days\":[14,7,1],\"heir\":\"11111111111111111111111111111111\"}";
    CHECK_EQ(proto_parse_request(line, &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_INHERITANCE_SET);
    CHECK_STR(r.id, "h1");
    CHECK(!r.sign.is_msg);
    CHECK_EQ(r.sign.len, 4);
    CHECK_U64(r.sign.claims.lamports_out, 5000000000ull);
    CHECK_EQ(r.sign.claims.n_programs, 1);
    CHECK_EQ(r.inheritance.inactivity_days, 90);
    CHECK_EQ(r.inheritance.n_warn, 3);
    CHECK_EQ(r.inheritance.warn_days[0], 14); CHECK_EQ(r.inheritance.warn_days[1], 7); CHECK_EQ(r.inheritance.warn_days[2], 1);
    CHECK_EQ(r.inheritance.heir[0], 0);
    CHECK(!r.inheritance.armed);
    /* no warn_days is fine */
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30,\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_OK);
    CHECK_EQ(r.inheritance.n_warn, 0);
    /* errors: no claims, no heir, bad heir, inactivity 0 / missing, warn 0, warn not a number, too many warns */
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"inactivity_days\":30,\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30,\"heir\":\"nope\"}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":0,\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30,\"warn_days\":[0],\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30,\"warn_days\":[\"7\"],\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_FIELD);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"AQID\",\"claims\":{},\"inactivity_days\":30,\"warn_days\":[5,4,3,2,1],\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_TOOBIG);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_set\",\"id\":\"h1\",\"bytes\":\"@@\",\"claims\":{},\"inactivity_days\":30,\"heir\":\"11111111111111111111111111111111\"}", &r), PROTO_E_ENCODING);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_status\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_INHERITANCE_STATUS);
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_release\",\"id\":\"h2\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_INHERITANCE_RELEASE); CHECK_STR(r.id, "h2");
    CHECK_EQ(proto_parse_request("{\"t\":\"inheritance_clear\",\"id\":\"h3\"}", &r), PROTO_OK);
    CHECK_EQ(r.type, PROTO_REQ_INHERITANCE_CLEAR);

    /* replies */
    {
        uint8_t sig[64];
        memset(sig, 0xAB, 64);
        CHECK(proto_fmt_inheritance_set(out, sizeof out, "h1", sig) > 0);
        CHECK_STR(out, "{\"t\":\"inheritance_set\",\"id\":\"h1\",\"ok\":true,\"sig\":\"q6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urq6urqw==\"}\n");
    }
    {
        consent_inheritance_t h = req90();
        proto_inh_status_t st;
        h.armed = true; h.warned = 0;
        memset(h.heir, 0, 32);
        st.inh = &h; st.days_since_touch_tenths = 124; st.powered_days_tenths = 1030;
        CHECK(proto_fmt_inheritance_status(out, sizeof out, "q1", &st) > 0);
        CHECK_STR(out, "{\"t\":\"inheritance_status\",\"id\":\"q1\",\"armed\":true,\"inactivity_days\":90,\"days_since_touch\":12.4,\"powered_days\":103.0,\"heir\":\"11111111111111111111111111111111\",\"warn_days\":[14,7,1],\"warned\":[]}\n");
        h.warned = 3;
        CHECK(proto_fmt_inheritance_status(out, sizeof out, NULL, &st) > 0);
        CHECK_STR(out, "{\"t\":\"inheritance_status\",\"armed\":true,\"inactivity_days\":90,\"days_since_touch\":12.4,\"powered_days\":103.0,\"heir\":\"11111111111111111111111111111111\",\"warn_days\":[14,7,1],\"warned\":[14,7]}\n");
        memset(&h, 0, sizeof h);
        st.days_since_touch_tenths = 0; st.powered_days_tenths = 5;
        CHECK(proto_fmt_inheritance_status(out, sizeof out, NULL, &st) > 0);
        CHECK_STR(out, "{\"t\":\"inheritance_status\",\"armed\":false,\"inactivity_days\":0,\"days_since_touch\":0.0,\"powered_days\":0.5,\"heir\":null,\"warn_days\":[],\"warned\":[]}\n");
    }
    {
        uint8_t tx[1 + 64 + 3] = {1};
        memset(tx + 1, 0xAB, 64); tx[65] = 1; tx[66] = 2; tx[67] = 3;
        CHECK(proto_fmt_inheritance_release(out, sizeof out, "h2", tx, sizeof tx) > 0);
        CHECK(strncmp(out, "{\"t\":\"inheritance_release\",\"id\":\"h2\",\"tx\":\"Aaurq6ur", 50) == 0);
        CHECK(strstr(out, "q6sBAgM=\"}\n") != NULL); /* 68 bytes: one pad char */
        /* the biggest transaction we can hold still fits one line */
        {
            static uint8_t big[1 + 64 * CONSENT_INH_MAX_SIGNERS + CONSENT_INH_MSG_MAX];
            static char line_out[PROTO_LINE_MAX];
            memset(big, 0x55, sizeof big);
            CHECK(proto_fmt_inheritance_release(line_out, sizeof line_out, "h2", big, sizeof big) > 0);
        }
    }
    CHECK(proto_fmt_event_days_left(out, sizeof out, 7) > 0);
    CHECK_STR(out, "{\"t\":\"event\",\"ev\":\"inheritance_warning\",\"days_left\":7}\n");
    CHECK(proto_fmt_denied(out, sizeof out, "h2", "not_expired") > 0);
    CHECK_STR(out, "{\"t\":\"denied\",\"id\":\"h2\",\"reason\":\"not_expired\"}\n");
    /* hello */
    {
        uint8_t key[32] = {0};
        proto_hello_t h;
        memset(&h, 0, sizeof h);
        h.fw = PROTO_FW_VERSION; h.se050 = true; h.initialized = true; h.pubkey = key; h.pin_set = true;
        h.inh_armed = true; h.inh_inactivity_days = 90; h.inh_days_since_touch_tenths = 124;
        CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
        CHECK(strstr(out, "\"pin_failures\":0,\"inheritance\":{\"armed\":true,\"inactivity_days\":90,\"days_since_touch\":12.4},\"display\":") != NULL);
        h.inh_armed = false; h.inh_inactivity_days = 0; h.inh_days_since_touch_tenths = 9;
        CHECK(proto_fmt_hello(out, sizeof out, &h) > 0);
        CHECK(strstr(out, "\"inheritance\":{\"armed\":false,\"inactivity_days\":0,\"days_since_touch\":0.9},\"display\":") != NULL);
    }
}

int main(void)
{
    memset(SYS, 0, 32); memset(OWNER, 0x11, 32); memset(NONCE, 0x22, 32); memset(HEIR, 0x33, 32); memset(SYSVAR, 0x44, 32); memset(OTHER, 0x55, 32);
    RUN(test_arm_requires_long_touch_and_nonce);
    RUN(test_durable_nonce_detection);
    RUN(test_status_math_and_release);
    RUN(test_touches_reset_timer);
    RUN(test_warnings_once_each);
    RUN(test_lock_duress_wipe_keep_inheritance);
    RUN(test_clear);
    RUN(test_counter_never_goes_backwards);
    RUN(test_storage_round_trip);
    RUN(test_storage_slots_and_power_loss);
    RUN(test_proto_messages);
    TEST_END();
}
