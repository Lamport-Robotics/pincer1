#include "consent.h"

#include "harness.h"

#define SOL 1000000000ull
#define T0 1791600000ull /* some unix time */

static uint8_t SYS[32], JUP[32], TOK[32];
static const uint8_t PIN[8] = {'P', 'R', 5, 0x04, (uint8_t)~5, (uint8_t)~0x04, 0x5A, 0xA5};   /* SSLS */
static const uint8_t PIN2[8] = {'P', 'R', 5, 0x05, (uint8_t)~5, (uint8_t)~0x05, 0x5A, 0xA5};  /* LSLS */
static const uint8_t (*P_SYS)[32] = (const uint8_t (*)[32])SYS;
static const uint8_t (*P_JUP)[32] = (const uint8_t (*)[32])JUP;

/* initialised + unlocked controller with a 2 SOL daily limit and host time */
static void ready(consent_t *c, uint32_t *ms)
{
    consent_persist_t p;
    consent_action_t a;
    consent_reason_t why;
    memset(&p, 0, sizeof p);
    p.initialized = true; p.pin_set = true;
    p.daily_limit = 2 * SOL;
    *ms = 1000;
    consent_init(c, &p, *ms);
    CHECK(c->locked); /* boots locked */
    consent_set_time(c, T0, *ms);
    CHECK_EQ(consent_begin_unlock(c, "u0", *ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(c), CONSENT_NEED_PIN);
    CHECK(!consent_touch(c, true, *ms, &a)); /* taps are rhythm input now, not approval */
    CHECK_EQ(consent_rhythm(c, PIN, *ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK_MEM(a.pin, PIN, 8);
    CHECK(c->locked); /* still locked until the signer says yes */
    CHECK_EQ(consent_pin_result(c, true, *ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK(!c->locked);
    c->dirty = false;
}

/* unlock helper for tests that lock in the middle */
static void unlock_with_pin(consent_t *c, const char *id, uint32_t ms)
{
    consent_action_t a;
    consent_reason_t why;
    CHECK_EQ(consent_begin_unlock(c, id, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(c, true, ms, &a), CONSENT_PIN_OK);
}

static void test_init_flow(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why = CONSENT_DENY_NONE;
    uint32_t ms = 0;
    consent_init(&c, NULL, ms);
    CHECK(!c.locked);
    CHECK(!c.p.initialized);
    CHECK_EQ(consent_begin_sign(&c, "s0", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    CHECK_EQ(consent_begin_init(&c, "i1", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_LONG_TOUCH);
    CHECK(!consent_touch(&c, false, ms + 100, &a)); /* short touch is not enough */
    CHECK_EQ(consent_begin_init(&c, "i2", ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_BUSY);
    CHECK(consent_touch(&c, true, ms + 3500, &a));
    CHECK_EQ(a.op, CONSENT_OP_INIT);
    CHECK_STR(a.id, "i1");
    consent_mark_initialized(&c);
    CHECK(c.p.initialized && c.dirty);
    CHECK_EQ(consent_begin_init(&c, "i3", ms, &why), CONSENT_R_DENIED); /* only once */
    CHECK_EQ(why, CONSENT_DENY_POLICY);
}

static void test_sign_touch_and_timeout(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    CHECK_EQ(consent_begin_sign(&c, "s7", SOL / 2, P_SYS, 1, false, 60000, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    CHECK(!consent_tick(&c, ms + 59999, &a));
    CHECK(consent_touch(&c, false, ms + 1000, &a));
    CHECK_EQ(a.op, CONSENT_OP_SIGN);
    CHECK_STR(a.id, "s7");
    CHECK_EQ(a.slot, CONSENT_SLOT_REAL);
    CHECK_U64(a.lamports, SOL / 2);
    CHECK(!a.via_allowance);
    consent_commit_spend(&c, &a);
    CHECK_U64(c.p.spent_today, SOL / 2);
    CHECK(c.dirty);
    /* timeout: default ttl when 0 */
    CHECK_EQ(consent_begin_sign(&c, "s8", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(!consent_tick(&c, ms + CONSENT_DEFAULT_TTL_MS - 1, &a));
    CHECK(consent_tick(&c, ms + CONSENT_DEFAULT_TTL_MS, &a));
    CHECK_EQ(a.op, CONSENT_OP_SIGN);
    CHECK_STR(a.id, "s8");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    CHECK(!consent_touch(&c, false, ms, &a)); /* nothing pending any more */
    /* a touch that arrives after the deadline is ignored (tick reports the timeout) */
    CHECK_EQ(consent_begin_sign(&c, "s9", 1, P_SYS, 1, false, 1000, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(!consent_touch(&c, false, ms + 1000, &a));
    CHECK(consent_tick(&c, ms + 1000, &a));
    /* ttl clamp */
    CHECK_EQ(consent_begin_sign(&c, "s10", 1, P_SYS, 1, false, 0xFFFFFFFFu, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(!consent_tick(&c, ms + CONSENT_MAX_TTL_MS - 1, &a));
    CHECK(consent_tick(&c, ms + CONSENT_MAX_TTL_MS, &a));
    /* a long touch on a sign request CANCELS it (hold 3 s = cancel on the trusted display) */
    CHECK_EQ(consent_begin_sign(&c, "s11", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_touch_ex(&c, true, ms + 1, &a), CONSENT_TOUCH_CANCELLED);
    CHECK_EQ(a.op, CONSENT_OP_SIGN);
    CHECK_STR(a.id, "s11");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    CHECK_U64(c.p.spent_today, SOL / 2); /* nothing accounted */
}

/* The trusted display gates every sign request: no touch counts before the screen for it was rendered, short
 * touches turn pages, the final page must have been shown, a long touch cancels. */
static void test_display_gating(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    CHECK_EQ(consent_begin_sign(&c, "d1", SOL / 4, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_DISPLAY);
    CHECK_STR(consent_need_str(consent_pending_need(&c)), "display");
    CHECK(consent_pending_display_gated(&c));
    /* nothing rendered yet: touches of any length do nothing */
    CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_NONE);
    CHECK_EQ(consent_touch_ex(&c, true, ms, &a), CONSENT_TOUCH_NONE);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_DISPLAY);
    /* page 1 of 3 shown: now pending(touch), but a touch only turns the page */
    CHECK(consent_display_shown(&c, 0, 3));
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    CHECK_EQ(consent_pending_page(&c), 0); CHECK_EQ(consent_pending_pages(&c), 3);
    CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_PAGE);
    CHECK_EQ(a.page, 1); CHECK_STR(a.id, "d1");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH); /* still pending */
    CHECK(consent_display_shown(&c, 1, 3));
    CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_PAGE);
    CHECK_EQ(a.page, 2);
    /* the caller has not rendered page 3 yet: another touch is NOT consent, it asks for the page again */
    CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_PAGE);
    CHECK_EQ(a.page, 2);
    CHECK(consent_display_shown(&c, 2, 3));
    CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_APPROVED);
    CHECK_EQ(a.op, CONSENT_OP_SIGN); CHECK_U64(a.lamports, SOL / 4);
    /* cancel from a middle page */
    CHECK_EQ(consent_begin_sign(&c, "d2", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    CHECK(consent_display_shown(&c, 0, 2));
    CHECK_EQ(consent_touch_ex(&c, true, ms, &a), CONSENT_TOUCH_CANCELLED);
    CHECK_STR(a.id, "d2");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    /* display_shown with nothing pending / bad page numbers */
    CHECK(!consent_display_shown(&c, 0, 1));
    CHECK_EQ(consent_begin_sign(&c, "d3", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    CHECK(!consent_display_shown(&c, 1, 1)); /* page >= pages */
    CHECK(!consent_display_shown(&c, 0, 0));
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_DISPLAY);
    CHECK(consent_tick(&c, ms + CONSENT_DEFAULT_TTL_MS, &a)); /* times out like anything else */
    /* non-gated requests (unlock before a PIN exists) are untouched: a long touch still approves */
    {
        consent_t f;
        consent_init(&f, NULL, ms);
        consent_begin_init(&f, "i", ms, &why);
        consent_touch(&f, true, ms, &a);
        consent_mark_initialized(&f);
        consent_lock(&f, NULL);
        CHECK_EQ(consent_begin_unlock(&f, "u", ms, &why), CONSENT_R_PENDING);
        CHECK_EQ(consent_pending_need(&f), CONSENT_NEED_TOUCH);
        CHECK(!consent_pending_display_gated(&f));
        CHECK_EQ(consent_touch_ex(&f, true, ms, &a), CONSENT_TOUCH_APPROVED);
    }
}

/* After init the owner taps through the seed words on the display before the device is initialised. */
static void test_init_words_flow(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms = 0;
    uint8_t i;
    consent_init(&c, NULL, ms);
    CHECK_EQ(consent_begin_init(&c, "i1", ms, &why), CONSENT_R_PENDING);
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_INIT);
    /* the firmware generated and imported the keys; now the words */
    CHECK_EQ(consent_begin_init_words(&c, "i1", 28, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_DISPLAY);
    CHECK(!c.p.initialized);
    CHECK_EQ(consent_begin_sign(&c, "s", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED); /* not initialised */
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    CHECK(consent_display_shown(&c, 0, 28));
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    for (i = 0; i < 27; i++) {
        CHECK_EQ(consent_touch_ex(&c, false, ms, &a), CONSENT_TOUCH_PAGE);
        CHECK_EQ(a.page, i + 1);
        CHECK(consent_display_shown(&c, (uint8_t)(i + 1), 28));
    }
    CHECK(!consent_tick(&c, ms + CONSENT_MAX_TTL_MS, &a)); /* a long window: the owner is writing */
    CHECK_EQ(consent_touch_ex(&c, false, ms + CONSENT_MAX_TTL_MS, &a), CONSENT_TOUCH_APPROVED);
    CHECK_EQ(a.op, CONSENT_OP_INIT_WORDS);
    CHECK_STR(a.id, "i1");
    consent_mark_initialized(&c);
    CHECK(c.p.initialized);
    CHECK_EQ(consent_begin_init_words(&c, "i2", 28, ms, &why), CONSENT_R_DENIED); /* only before initialised */
    /* cancel (long touch) and timeout hand the op back so the firmware can destroy the keys */
    consent_init(&c, NULL, ms);
    CHECK_EQ(consent_begin_init_words(&c, "i3", 28, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 28);
    consent_touch_ex(&c, false, ms, &a);
    consent_display_shown(&c, 1, 28);
    CHECK_EQ(consent_touch_ex(&c, true, ms, &a), CONSENT_TOUCH_CANCELLED);
    CHECK_EQ(a.op, CONSENT_OP_INIT_WORDS); CHECK_STR(a.id, "i3");
    CHECK_EQ(consent_begin_init_words(&c, "i4", 28, ms, &why), CONSENT_R_PENDING);
    CHECK(!consent_tick(&c, ms + CONSENT_INIT_WORDS_TTL_MS - 1, &a));
    CHECK(consent_tick(&c, ms + CONSENT_INIT_WORDS_TTL_MS, &a));
    CHECK_EQ(a.op, CONSENT_OP_INIT_WORDS); CHECK_STR(a.id, "i4");
    CHECK_EQ(consent_begin_init_words(&c, "i5", 0, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(consent_begin_init_words(&c, "i5", CONSENT_MAX_PAGES + 1, ms, &why), CONSENT_R_DENIED);
}

static void test_daily_limit_and_elevate(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    c.p.spent_today = 15 * SOL / 10; /* 1.5 of 2 SOL spent */
    CHECK_EQ(consent_begin_sign(&c, "s1", SOL / 2, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING); /* exactly at limit */
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_OVER_DAILY_LIMIT);
    CHECK_STR(consent_reason_str(why), "over_daily_limit");
    /* elevate: long touch, then one request may go up to the elevated limit */
    CHECK_EQ(consent_begin_elevate(&c, "e1", 20 * SOL, ms, &why), CONSENT_R_PENDING);
    CHECK(!consent_touch(&c, false, ms, &a));
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_ELEVATE);
    CHECK(c.elevate.active);
    CHECK_EQ(c.elevate.until_ms, ms + CONSENT_ELEVATE_WINDOW_MS);
    CHECK_EQ(consent_begin_sign(&c, "s3", 10 * SOL, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    consent_commit_spend(&c, &a);
    CHECK_U64(c.p.spent_today, 12 * SOL);
    CHECK(!c.elevate.active); /* consumed */
    CHECK_EQ(consent_begin_sign(&c, "s4", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_OVER_DAILY_LIMIT);
    /* elevate expires after the window */
    CHECK_EQ(consent_begin_elevate(&c, "e2", 20 * SOL, ms, &why), CONSENT_R_PENDING);
    consent_touch(&c, true, ms, &a);
    consent_tick(&c, ms + CONSENT_ELEVATE_WINDOW_MS, &a);
    CHECK(!c.elevate.active);
    CHECK_EQ(consent_begin_sign(&c, "s5", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    /* an overflowing amount is never accepted */
    c.p.spent_today = 0; c.p.daily_limit = UINT64_MAX;
    CHECK_EQ(consent_begin_sign(&c, "s6", UINT64_MAX, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
}

static void test_policy_change(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    CHECK_EQ(consent_begin_policy(&c, "p1", 5 * SOL, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK(!consent_touch(&c, false, ms, &a));
    CHECK(!consent_touch(&c, true, ms, &a)); /* a long touch is not enough any more */
    CHECK_U64(c.p.daily_limit, 2 * SOL);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(a.op, CONSENT_OP_POLICY);
    CHECK_U64(c.p.daily_limit, 2 * SOL); /* nothing applied before the chip's verdict */
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_POLICY);
    CHECK_U64(c.p.daily_limit, 5 * SOL);
    CHECK(c.dirty);
    /* a wrong rhythm leaves the limit alone and is counted */
    CHECK_EQ(consent_begin_policy(&c, "p2", 7 * SOL, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN2, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
    CHECK_STR(a.id, "p2");
    CHECK_U64(c.p.daily_limit, 5 * SOL);
    CHECK_EQ(c.p.pin_failures, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
}

static void test_lock_unlock(void)
{
    consent_t c;
    consent_action_t a, cancelled;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_lock(&c, &cancelled));
    CHECK_EQ(cancelled.op, CONSENT_OP_SIGN);
    CHECK_STR(cancelled.id, "s1");
    CHECK(c.locked);
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_LOCKED);
    CHECK_EQ(consent_begin_elevate(&c, "e1", SOL, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(consent_begin_policy(&c, "p1", SOL, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(consent_begin_init(&c, "i1", ms, &why), CONSENT_R_DENIED);
    CHECK(!consent_lock(&c, &cancelled)); /* idempotent */
    CHECK_EQ(consent_begin_unlock(&c, "u1", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK(!consent_touch(&c, false, ms, &a));
    CHECK(c.locked);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK(!c.locked);
    CHECK_EQ(consent_begin_sign(&c, "s3", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    /* unlock pending times out like anything else */
    consent_lock(&c, NULL);
    consent_begin_unlock(&c, "u2", ms, &why);
    CHECK(consent_tick(&c, ms + 60000, &a));
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK(c.locked);
}

static void test_duress(void)
{
    consent_t c;
    consent_action_t a, cancelled;
    consent_reason_t why;
    uint32_t ms;
    consent_persist_t saved;
    ready(&c, &ms);
    CHECK_EQ(consent_active_slot(&c), CONSENT_SLOT_REAL);
    consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, ms, &a, &why);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_duress(&c, &cancelled));
    CHECK_STR(cancelled.id, "s1");
    CHECK(c.p.duress && c.dirty);
    CHECK_EQ(consent_active_slot(&c), CONSENT_SLOT_DECOY);
    /* signing still "works" - with the decoy key */
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(a.slot, CONSENT_SLOT_DECOY);
    /* owner-authority operations answer exactly as they would in the real slot: pending, then done.
     * They act on the decoy slot's own limits; the real slot's are kept aside. */
    CHECK_U64(c.p.daily_limit, 2 * SOL); /* the decoy starts with the real slot's limits, its own counters */
    CHECK_U64(c.p.shadow.daily_limit, 2 * SOL);
    CHECK(c.p.shadow_valid);
    CHECK_EQ(consent_begin_elevate(&c, "e1", SOL, ms, &why), CONSENT_R_PENDING);
    consent_cancel_pending(&c, NULL);
    CHECK_EQ(consent_begin_policy(&c, "p1", 5 * SOL, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_POLICY);
    CHECK_U64(c.p.daily_limit, 5 * SOL);          /* the decoy's limit */
    CHECK_U64(c.p.shadow.daily_limit, 2 * SOL);   /* the real one, untouched */
    /* duress survives a reboot, with the decoy's limits active */
    saved = c.p;
    consent_init(&c, &saved, ms);
    CHECK(c.p.duress);
    CHECK(c.locked);
    CHECK_EQ(consent_active_slot(&c), CONSENT_SLOT_DECOY);
    CHECK_U64(c.p.daily_limit, 5 * SOL);
    /* cleared by unlock (PIN rhythm verified in the chip): the real limits come back */
    unlock_with_pin(&c, "u1", ms);
    CHECK(!c.p.duress);
    CHECK_EQ(consent_active_slot(&c), CONSENT_SLOT_REAL);
    CHECK_U64(c.p.daily_limit, 2 * SOL);
    CHECK_U64(c.p.shadow.daily_limit, 5 * SOL);
}

static void test_day_rollover(void)
{
    consent_t c;
    uint32_t ms;
    ready(&c, &ms);
    CHECK_EQ(c.p.day, (uint32_t)(T0 / 86400));
    c.p.spent_today = SOL;
    c.dirty = false;
    CHECK_EQ(consent_set_time(&c, T0 + 60, ms + 60000), CONSENT_TIME_SAME); /* same day: nothing */
    CHECK_U64(c.p.spent_today, SOL);
    CHECK(!c.dirty);
    /* host time jumps to the next unix day, but the controller has only been powered 2 h: NO rollover */
    c.ih.powered_seconds = 2 * 3600;
    CHECK_EQ(consent_set_time(&c, (T0 / 86400 + 1) * 86400, ms + 120000), CONSENT_TIME_SAME);
    CHECK_U64(c.p.spent_today, SOL);
    CHECK_EQ(c.p.day, (uint32_t)(T0 / 86400));
    CHECK(c.dirty); /* only the time floor moved (more than an hour on): that alone is written */
    CHECK_U64(c.p.last_unix, (T0 / 86400 + 1) * 86400);
    c.dirty = false;
    /* 20 h of powered uptime + the host day change: rollover */
    c.ih.powered_seconds = CONSENT_TIME_MIN_POWERED_S;
    CHECK_EQ(consent_set_time(&c, (T0 / 86400 + 1) * 86400 + 10, ms + 130000), CONSENT_TIME_CHANGED);
    CHECK_U64(c.p.spent_today, 0);
    CHECK_EQ(c.p.day, (uint32_t)(T0 / 86400 + 1));
    CHECK_U64(c.p.rollover_powered_seconds, CONSENT_TIME_MIN_POWERED_S);
    CHECK(c.dirty);
    /* the next day needs another 20 h of uptime since THAT rollover */
    c.p.spent_today = 3;
    c.ih.powered_seconds = CONSENT_TIME_MIN_POWERED_S + 19 * 3600;
    CHECK_EQ(consent_set_time(&c, (T0 / 86400 + 2) * 86400, ms + 140000), CONSENT_TIME_SAME);
    CHECK_U64(c.p.spent_today, 3);
    c.ih.powered_seconds = 2 * CONSENT_TIME_MIN_POWERED_S;
    CHECK_EQ(consent_set_time(&c, (T0 / 86400 + 2) * 86400 + 1, ms + 150000), CONSENT_TIME_CHANGED);
    CHECK_U64(c.p.spent_today, 0);
    /* a persisted day from a previous boot is honoured */
    {
        consent_persist_t p;
        memset(&p, 0, sizeof p);
        p.initialized = true; p.pin_set = true; p.daily_limit = SOL; p.spent_today = 5; p.day = 20395;
        consent_init(&c, &p, 0);
        c.ih.powered_seconds = CONSENT_TIME_MIN_POWERED_S;
        consent_set_time(&c, 20395ull * 86400 + 100, 0);
        CHECK_U64(c.p.spent_today, 5);
        consent_set_time(&c, 20396ull * 86400, 0);
        CHECK_U64(c.p.spent_today, 0);
    }
    /* no host time: now_unix unavailable, ttl still ticks */
    {
        consent_action_t a;
        consent_reason_t why;
        uint64_t u;
        consent_persist_t p;
        memset(&p, 0, sizeof p);
        p.initialized = true; p.pin_set = true; p.daily_limit = SOL;
        consent_init(&c, &p, 0);
        c.locked = false;
        CHECK(!consent_now_unix(&c, 0, &u));
        CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, 0, &a, &why), CONSENT_R_PENDING);
        consent_display_shown(&c, 0, 1);
        CHECK(consent_tick(&c, 60000, &a));
    }
    /* interpolated clock */
    ready(&c, &ms);
    {
        uint64_t u;
        CHECK(consent_now_unix(&c, ms + 90500, &u));
        CHECK_U64(u, T0 + 90);
    }
}

/* Host time is untrusted: backwards or far-forward values are ignored and never move the counters. */
static void test_host_time_rejected(void)
{
    consent_t c;
    uint32_t ms;
    uint64_t u;
    ready(&c, &ms); /* T0 accepted at ms */
    c.p.spent_today = SOL;
    c.ih.powered_seconds = 10 * CONSENT_TIME_MIN_POWERED_S; /* uptime is not the obstacle here */
    c.dirty = false;
    /* backwards: ignored, last accepted value kept */
    CHECK_EQ(consent_set_time(&c, T0 - 1, ms + 1000), CONSENT_TIME_REJECTED);
    CHECK_EQ(consent_set_time(&c, T0 - 5 * 86400, ms + 1000), CONSENT_TIME_REJECTED);
    CHECK(consent_now_unix(&c, ms + 1000, &u)); CHECK_U64(u, T0 + 1);
    CHECK_U64(c.p.spent_today, SOL); CHECK(!c.dirty);
    CHECK_EQ(c.p.day, (uint32_t)(T0 / 86400));
    /* forward by more than 36 h: ignored, even though it names another day */
    CHECK_EQ(consent_set_time(&c, T0 + CONSENT_TIME_MAX_JUMP_S + 2, ms + 1000), CONSENT_TIME_REJECTED);
    CHECK_EQ(consent_set_time(&c, T0 + 30 * 86400, ms + 1000), CONSENT_TIME_REJECTED);
    CHECK_U64(c.p.spent_today, SOL);
    CHECK(consent_now_unix(&c, ms + 1000, &u)); CHECK_U64(u, T0 + 1);
    /* forward by less than 36 h into the next day: accepted, and with the uptime present it rolls over */
    CHECK_EQ(consent_set_time(&c, T0 + 30 * 3600, ms + 2000), CONSENT_TIME_CHANGED);
    CHECK_U64(c.p.spent_today, 0);
    /* equal to the last accepted value is not "backwards" */
    CHECK_EQ(consent_set_time(&c, T0 + 30 * 3600, ms + 3000), CONSENT_TIME_SAME);
    /* the very first time after boot is always accepted (the controller has no clock of its own) */
    {
        consent_persist_t p;
        memset(&p, 0, sizeof p);
        p.initialized = true; p.pin_set = true; p.day = 20395; p.spent_today = 7;
        consent_init(&c, &p, 0);
        CHECK_EQ(consent_set_time(&c, 1000, 0), CONSENT_TIME_SAME); /* a day far in the past: accepted as time ... */
        CHECK_U64(c.p.spent_today, 7);                               /* ... but no uptime, so no rollover */
        CHECK_EQ(c.p.day, 20395);
    }
}

/* ---- allowance ---- */

static void grant(consent_t *c, uint32_t ms, uint64_t lamports, uint64_t per_tx, uint64_t until)
{
    consent_allowance_t a;
    consent_action_t act;
    consent_reason_t why;
    memset(&a, 0, sizeof a);
    a.lamports = lamports; a.max_per_tx = per_tx; a.until_unix = until;
    a.n_programs = 2; memcpy(a.programs[0], JUP, 32); memcpy(a.programs[1], SYS, 32);
    CHECK_EQ(consent_begin_allowance(c, "a1", &a, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(c), CONSENT_NEED_LONG_TOUCH);
    CHECK(!consent_touch(c, false, ms, &act));
    CHECK(consent_touch(c, true, ms, &act));
    CHECK_EQ(act.op, CONSENT_OP_ALLOWANCE);
    CHECK(c->p.allowance.active);
    CHECK(c->dirty);
}

/* the allowance is a SOL budget: a token outflow (or anything dangerous) under an active allowance still
 * takes a touch, so a compromised host cannot drain token balances "for free" overnight */
static void test_allowance_never_covers_tokens(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    CHECK(consent_allowance_active(&c, ms));
    /* lamports_out = 0, allowed programs only, but touch_only (a token transfer): pending, not done */
    CHECK_EQ(consent_begin_sign(&c, "t1", 0, P_SYS, 1, true, 60000, ms, &a, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_DISPLAY); /* the screen first, as for any touch */
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    CHECK(consent_touch(&c, false, ms, &a)); /* a short touch signs it */
    CHECK(!a.via_allowance);
    CHECK_U64(c.p.allowance.spent, 0);
}

static void test_allowance_signs_without_touch(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    CHECK(consent_allowance_active(&c, ms));
    CHECK_EQ(consent_begin_sign(&c, "s1", SOL / 5, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_DONE);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(a.op, CONSENT_OP_SIGN);
    CHECK_STR(a.id, "s1");
    CHECK(a.via_allowance);
    CHECK_EQ(a.slot, CONSENT_SLOT_REAL);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    consent_commit_spend(&c, &a);
    CHECK_U64(c.p.allowance.spent, SOL / 5);
    CHECK_U64(c.p.spent_today, SOL / 5); /* still counts against the day */
    /* two programs both inside the allowance */
    {
        uint8_t both[2][32];
        memcpy(both[0], JUP, 32); memcpy(both[1], SYS, 32);
        CHECK_EQ(consent_begin_sign(&c, "s2", 1, (const uint8_t (*)[32])both, 2, false, 0, ms, &a, &why), CONSENT_R_DONE);
        consent_display_shown(&c, 0, 1);
    }
    /* per-tx cap: needs a touch instead */
    CHECK_EQ(consent_begin_sign(&c, "s3", SOL / 5 + 1, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    consent_tick(&c, ms + 60000, &a);
    /* dangerous instruction: needs a touch */
    CHECK_EQ(consent_begin_sign(&c, "s4", 1, P_JUP, 1, true, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_tick(&c, ms + 60000, &a);
    /* the daily limit still applies */
    c.p.spent_today = 2 * SOL;
    CHECK_EQ(consent_begin_sign(&c, "s5", 1, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_OVER_DAILY_LIMIT);
}

static void test_allowance_program_outside_needs_touch(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    uint8_t progs[2][32];
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    memcpy(progs[0], JUP, 32); memcpy(progs[1], TOK, 32);
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, (const uint8_t (*)[32])progs, 2, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK(!a.via_allowance);
    consent_commit_spend(&c, &a);
    CHECK_U64(c.p.allowance.spent, 0); /* touched requests do not eat the allowance */
    /* a message with no recognised program at all is never auto-signed */
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_JUP, 0, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
}

static void test_allowance_cumulative_exhaustion(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    int i;
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 4, T0 + 50000);
    for (i = 0; i < 4; i++) {
        CHECK_EQ(consent_begin_sign(&c, "s", SOL / 4, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_DONE);
        consent_display_shown(&c, 0, 1);
        consent_commit_spend(&c, &a);
    }
    CHECK_U64(c.p.allowance.spent, SOL);
    CHECK(c.p.allowance.active); /* still active, just exhausted */
    CHECK_EQ(consent_begin_sign(&c, "s5", 1, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
}

static void test_allowance_expiry(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 5, T0 + 100);
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_JUP, 1, false, 0, ms + 99000, &a, &why), CONSENT_R_DONE);
    consent_display_shown(&c, 0, 1);
    /* interpolated tick clock reaches until_unix */
    CHECK(!consent_allowance_active(&c, ms + 100000));
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_JUP, 1, false, 0, ms + 100000, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(!consent_tick(&c, ms + 100000, &a));
    CHECK(!c.p.allowance.active); /* tick() dropped it */
    CHECK(consent_tick(&c, ms + 160000, &a)); /* let the touch request time out */
    /* host time message also expires it */
    grant(&c, ms, SOL, SOL / 5, T0 + 100);
    c.dirty = false;
    CHECK(consent_set_time(&c, T0 + 100, ms));
    CHECK(!c.p.allowance.active);
    CHECK(c.dirty);
    /* cannot grant one that is already expired, or without host time */
    {
        consent_allowance_t al;
        memset(&al, 0, sizeof al);
        al.lamports = 1; al.max_per_tx = 1; al.until_unix = T0 + 50; al.n_programs = 1; memcpy(al.programs[0], JUP, 32);
        CHECK_EQ(consent_begin_allowance(&c, "a9", &al, ms, &why), CONSENT_R_DENIED);
        CHECK_EQ(why, CONSENT_DENY_POLICY);
        al.until_unix = T0 + 5000; al.max_per_tx = 2; /* per-tx above total */
        CHECK_EQ(consent_begin_allowance(&c, "a9", &al, ms, &why), CONSENT_R_DENIED);
        c.have_time = false;
        al.max_per_tx = 1;
        CHECK_EQ(consent_begin_allowance(&c, "a9", &al, ms, &why), CONSENT_R_DENIED);
    }
    /* a persisted allowance is not usable until the host proves the time */
    {
        consent_persist_t p;
        memset(&p, 0, sizeof p);
        p.initialized = true; p.pin_set = true; p.daily_limit = 2 * SOL;
        p.allowance.active = true; p.allowance.lamports = SOL; p.allowance.max_per_tx = SOL; p.allowance.until_unix = T0 + 5000;
        p.allowance.n_programs = 1; memcpy(p.allowance.programs[0], JUP, 32);
        consent_init(&c, &p, 0);
        c.locked = false;
        CHECK(!consent_allowance_active(&c, 0));
        CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_JUP, 1, false, 0, 0, &a, &why), CONSENT_R_PENDING);
        consent_display_shown(&c, 0, 1);
        consent_tick(&c, 60000, &a);
        consent_set_time(&c, T0, 60000);
        CHECK(consent_allowance_active(&c, 60000));
        CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_JUP, 1, false, 0, 60000, &a, &why), CONSENT_R_DONE);
        consent_display_shown(&c, 0, 1);
    }
}

static void test_allowance_revoke_lock_duress(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    c.dirty = false;
    CHECK(consent_allowance_revoke(&c));
    CHECK(!c.p.allowance.active && c.dirty);
    CHECK(!consent_allowance_revoke(&c));
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_tick(&c, ms + 60000, &a);
    /* lock clears it */
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    consent_lock(&c, NULL);
    CHECK(!c.p.allowance.active);
    unlock_with_pin(&c, "u", ms);
    /* duress clears it and the decoy never auto-signs */
    grant(&c, ms, SOL, SOL / 5, T0 + 50000);
    consent_duress(&c, NULL);
    CHECK(!c.p.allowance.active);
    CHECK_EQ(consent_begin_sign(&c, "s2", 1, P_JUP, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_tick(&c, ms + 60000, &a);
    /* an allowance in duress is the decoy slot's own, granted and spent like the real one */
    {
        consent_allowance_t a2 = {0};
        a2.lamports = SOL; a2.max_per_tx = SOL; a2.until_unix = T0 + 3600; a2.n_programs = 1; memcpy(a2.programs[0], P_JUP[0], 32);
        CHECK_EQ(consent_begin_allowance(&c, "a2", &a2, ms, &why), CONSENT_R_PENDING);
        consent_cancel_pending(&c, NULL);
    }
}

/* ---- PIN ---- */

static void test_set_pin_flow(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms = 0;
    consent_init(&c, NULL, ms);
    CHECK_EQ(consent_begin_set_pin(&c, "n0", ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    consent_begin_init(&c, "i1", ms, &why);
    consent_touch(&c, true, ms, &a);
    consent_mark_initialized(&c);
    CHECK(!c.p.pin_set);
    /* nothing that uses the keys works before set_pin */
    CHECK_EQ(consent_begin_sign(&c, "s0", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_PIN_NOT_SET);
    CHECK_STR(consent_reason_str(why), "pin_not_set");
    CHECK_EQ(consent_begin_policy(&c, "p0", SOL, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_PIN_NOT_SET);
    /* unlock before a PIN exists still works with a touch (init flow not finished) */
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_unlock(&c, "u0", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH);
    CHECK(consent_touch(&c, false, ms, &a));
    /* set_pin: rhythm twice, mismatch denies */
    CHECK_EQ(consent_begin_set_pin(&c, "n1", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK_STR(consent_need_str(consent_pending_need(&c)), "pin");
    CHECK(!consent_touch(&c, true, ms, &a));
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK_EQ(consent_rhythm(&c, PIN2, ms + 100, &a), CONSENT_RHYTHM_MISMATCH);
    CHECK_STR(a.id, "n1");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    CHECK(!c.p.pin_set);
    CHECK_EQ(c.p.pin_failures, 0); /* a mismatch during setup is not an attack */
    /* second try matches */
    CHECK_EQ(consent_begin_set_pin(&c, "n2", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_rhythm(&c, PIN, ms + 100, &a), CONSENT_RHYTHM_SET);
    CHECK_EQ(a.op, CONSENT_OP_SET_PIN);
    CHECK_STR(a.id, "n2");
    CHECK_MEM(a.pin, PIN, 8);
    CHECK(!c.p.pin_set); /* the caller marks it after the chip accepted the object */
    consent_mark_pin_set(&c);
    CHECK(c.p.pin_set && c.dirty);
    CHECK_EQ(consent_begin_set_pin(&c, "n3", ms, &why), CONSENT_R_DENIED); /* only once */
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    CHECK_EQ(consent_begin_sign(&c, "s1", 0, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING); /* limit is 0 on a fresh device */
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_TOUCH); /* ordinary signatures: touch, unchanged */
    /* the confirmation window times out like any pending request */
    consent_tick(&c, ms + 60000, &a);
    CHECK_EQ(consent_begin_set_pin(&c, "n4", ms, &why), CONSENT_R_DENIED); /* pin already set */
}

static void test_unlock_wrong_pin_and_wipe(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    consent_persist_t saved;
    uint32_t ms;
    int i;
    ready(&c, &ms);
    consent_lock(&c, NULL);
    for (i = 1; i <= 4; i++) {
        CHECK_EQ(consent_begin_unlock(&c, "u", ms, &why), CONSENT_R_PENDING);
        CHECK_EQ(consent_rhythm(&c, PIN2, ms, &a), CONSENT_RHYTHM_VERIFY);
        CHECK(c.locked);
        c.dirty = false;
        CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
        CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
        CHECK_STR(a.id, "u");
        CHECK_EQ(c.p.pin_failures, i);
        CHECK(c.dirty);
        CHECK(c.locked);
        CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
        CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_NONE); /* nothing pending */
    }
    /* the mirror survives a reboot */
    saved = c.p;
    consent_init(&c, &saved, ms);
    CHECK_EQ(c.p.pin_failures, 4);
    CHECK(c.locked);
    /* a correct rhythm resets the counter */
    CHECK_EQ(consent_begin_unlock(&c, "u5", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(c.p.pin_failures, 0);
    CHECK(!c.locked);
    /* fifth consecutive failure -> wipe */
    consent_lock(&c, NULL);
    for (i = 1; i <= 4; i++) {
        consent_begin_unlock(&c, "u", ms, &why);
        consent_rhythm(&c, PIN2, ms, &a);
        CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
    }
    CHECK_EQ(consent_begin_unlock(&c, "u9", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN2, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WIPE);
    CHECK_STR(a.id, "u9");
    CHECK(!c.p.initialized);
    CHECK(!c.p.pin_set);
    CHECK_EQ(c.p.pin_failures, 0);
    CHECK_U64(c.p.daily_limit, 0);
    CHECK(!c.p.allowance.active);
    CHECK(!c.locked); /* factory state: init allowed again */
    CHECK(c.dirty);
    CHECK_EQ(consent_begin_sign(&c, "s", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_NOT_INITIALIZED);
    CHECK_EQ(consent_begin_init(&c, "i", ms, &why), CONSENT_R_PENDING);
}

static void test_pin_timeout_and_lock_cancel(void)
{
    consent_t c;
    consent_action_t a, cancelled;
    consent_reason_t why;
    uint32_t ms;
    ready(&c, &ms);
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_unlock(&c, "u1", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms + 60000, &a), CONSENT_RHYTHM_NONE); /* too late */
    CHECK(consent_tick(&c, ms + 60000, &a));
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK(c.locked);
    /* a lock while a PIN entry is pending cancels it */
    CHECK_EQ(consent_begin_unlock(&c, "u2", ms, &why), CONSENT_R_PENDING);
    CHECK(consent_lock(&c, &cancelled));
    CHECK_STR(cancelled.id, "u2");
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_NONE);
    /* a link error drops the request without counting a failure */
    CHECK_EQ(consent_begin_unlock(&c, "u2b", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK(consent_cancel_pending(&c, &cancelled));
    CHECK_STR(cancelled.id, "u2b");
    CHECK_EQ(c.p.pin_failures, 0);
    CHECK(c.locked);
    CHECK(!consent_cancel_pending(&c, &cancelled));
    /* a rhythm while a touch request is pending is ignored */
    unlock_with_pin(&c, "u3", ms);
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_NONE);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_NONE);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(a.op, CONSENT_OP_SIGN);
}

/* ---- per-chain daily limits (check 6), elevate per chain, policy subsets, rollover ---- */
static void test_chain_limits(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    consent_policy_req_t pr;
    u256_t wei, big;
    uint32_t ms;
    ready(&c, &ms); /* SOL limit 2 SOL, the other chains start at 0 */
    u256_from_dec("1000000000000000000", &wei); /* 1 ETH */
    /* EVM: nothing is allowed until a wei limit exists (except zero-value messages) */
    CHECK_EQ(consent_begin_sign_chain(&c, "e0", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_OVER_DAILY_LIMIT);
    CHECK_EQ(consent_begin_sign_chain(&c, "m0", CONSENT_CHAIN_EVM, 0, NULL, 0, ms, &why), CONSENT_R_PENDING); /* message */
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(a.chain, CONSENT_CHAIN_EVM);
    consent_commit_spend(&c, &a);
    CHECK(u256_is_zero(&c.p.spent_today_wei));
    /* policy: set wei + sats in one PIN-gated request, SOL untouched */
    memset(&pr, 0, sizeof pr);
    pr.has_wei = true; u256_from_dec("2000000000000000000", &pr.wei);
    pr.has_sats = true; pr.sats = 300000;
    CHECK_EQ(consent_begin_policy_multi(&c, "p1", &pr, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK_U64(c.p.daily_limit, 2 * SOL);
    CHECK_EQ(u256_cmp(&c.p.daily_limit_wei, &pr.wei), 0);
    CHECK_U64(c.p.daily_limit_sats, 300000);
    memset(&pr, 0, sizeof pr);
    CHECK_EQ(consent_begin_policy_multi(&c, "p2", &pr, ms, &why), CONSENT_R_DENIED); /* nothing to set */
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    /* EVM: 1 ETH twice is the limit, the third is over; SOL and BTC counters are independent */
    CHECK_EQ(consent_begin_sign_chain(&c, "e1", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(a.chain, CONSENT_CHAIN_EVM);
    CHECK_EQ(u256_cmp(&a.wei, &wei), 0);
    consent_commit_spend(&c, &a);
    CHECK_EQ(u256_cmp(&c.p.spent_today_wei, &wei), 0);
    CHECK_U64(c.p.spent_today, 0);
    CHECK_EQ(consent_begin_sign_chain(&c, "e2", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK_EQ(consent_begin_sign_chain(&c, "e3", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_OVER_DAILY_LIMIT);
    CHECK_EQ(consent_begin_sign(&c, "s1", SOL, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING); /* SOL unaffected */
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK_U64(c.p.spent_today, SOL);
    /* BTC: 150000 + 150000 = limit; 1 more is over; an EVM elevate does not help BTC */
    CHECK_EQ(consent_begin_sign_chain(&c, "b1", CONSENT_CHAIN_BTC, 150000, NULL, 0, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a);
    CHECK_EQ(a.chain, CONSENT_CHAIN_BTC); CHECK_U64(a.lamports, 150000);
    consent_commit_spend(&c, &a);
    CHECK_U64(c.p.spent_today_sats, 150000);
    CHECK_EQ(consent_begin_sign_chain(&c, "b2", CONSENT_CHAIN_BTC, 150001, NULL, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    u256_from_dec("10000000000000000000", &big);
    CHECK_EQ(consent_begin_elevate_chain(&c, "x1", CONSENT_CHAIN_EVM, 0, &big, ms, &why), CONSENT_R_PENDING);
    CHECK(consent_touch(&c, true, ms, &a));
    CHECK(c.elevate.active); CHECK_EQ(c.elevate.chain, CONSENT_CHAIN_EVM);
    CHECK_EQ(consent_begin_sign_chain(&c, "b3", CONSENT_CHAIN_BTC, 150001, NULL, 0, ms, &why), CONSENT_R_DENIED); /* wrong chain */
    consent_display_shown(&c, 0, 1);
    CHECK(c.elevate.active);
    CHECK_EQ(consent_begin_sign_chain(&c, "e4", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_PENDING); /* 3rd ETH under the elevate */
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK(!c.elevate.active); /* consumed by the EVM request */
    CHECK_EQ(consent_begin_sign_chain(&c, "e5", CONSENT_CHAIN_EVM, 0, &wei, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    /* a BTC elevate, consumed by a BTC request, leaves SOL spends alone */
    CHECK_EQ(consent_begin_elevate_chain(&c, "x2", CONSENT_CHAIN_BTC, 1000000, NULL, ms, &why), CONSENT_R_PENDING);
    consent_touch(&c, true, ms, &a);
    CHECK_EQ(consent_begin_sign(&c, "s2", SOL / 2, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK(c.elevate.active); /* a SOL sign does not consume a BTC elevate */
    CHECK_EQ(consent_begin_sign_chain(&c, "b4", CONSENT_CHAIN_BTC, 800000, NULL, 0, ms, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    consent_touch(&c, false, ms, &a); consent_commit_spend(&c, &a);
    CHECK(!c.elevate.active);
    CHECK_U64(c.p.spent_today_sats, 950000);
    /* saturation never passes */
    u256_from_dec("115792089237316195423570985008687907853269984665640564039457584007913129639935", &big);
    c.p.daily_limit_wei = big;
    CHECK_EQ(consent_begin_sign_chain(&c, "e6", CONSENT_CHAIN_EVM, 0, &big, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    /* day rollover (host day change + 20 h powered) clears all three counters */
    c.ih.powered_seconds = CONSENT_TIME_MIN_POWERED_S;
    CHECK_EQ(consent_set_time(&c, (T0 / 86400 + 1) * 86400, ms + 1000), CONSENT_TIME_CHANGED);
    CHECK_U64(c.p.spent_today, 0);
    CHECK(u256_is_zero(&c.p.spent_today_wei));
    CHECK_U64(c.p.spent_today_sats, 0);
    /* lock / duress gates apply to the chain variants too */
    CHECK(consent_lock(&c, &a) == false);
    CHECK_EQ(consent_begin_sign_chain(&c, "e7", CONSENT_CHAIN_EVM, 0, NULL, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
    CHECK_EQ(why, CONSENT_DENY_LOCKED);
    CHECK_EQ(consent_begin_elevate_chain(&c, "x3", CONSENT_CHAIN_BTC, 1, NULL, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(consent_begin_sign_chain(&c, "e8", (consent_chain_t)7, 0, NULL, 0, ms, &why), CONSENT_R_DENIED);
    consent_display_shown(&c, 0, 1);
}

/* Router allowlist changes go through the PIN-gated policy request and land in the persisted state. */
static void test_router_policy(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    consent_policy_req_t pr;
    uint32_t ms;
    uint8_t thor[20];
    int i;
    ready(&c, &ms);
    memset(thor, 0x7c, 20);
    memset(&pr, 0, sizeof pr);
    pr.has_router_add = true; pr.router.chain = ROUTER_CHAIN_EVM; memcpy(pr.router.addr, thor, 20);
    CHECK(!routers_is_allowed(&c.p.routers, ROUTER_CHAIN_EVM, thor));
    CHECK_EQ(consent_begin_policy_multi(&c, "r1", &pr, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK(!consent_touch(&c, true, ms, &a)); /* a touch is not enough */
    CHECK(!routers_is_allowed(&c.p.routers, ROUTER_CHAIN_EVM, thor));
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_OK);
    CHECK(a.policy.has_router_add); CHECK_MEM(a.policy.router.addr, thor, 20); /* for the "ROUTER ADDED" screen */
    CHECK(routers_is_allowed(&c.p.routers, ROUTER_CHAIN_EVM, thor));
    CHECK_EQ(c.p.routers.n, 1);
    CHECK(c.dirty);
    /* a wrong PIN changes nothing */
    memset(thor, 0x7d, 20); memcpy(pr.router.addr, thor, 20);
    CHECK_EQ(consent_begin_policy_multi(&c, "r2", &pr, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN2, ms, &a);
    CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
    CHECK(!routers_is_allowed(&c.p.routers, ROUTER_CHAIN_EVM, thor));
    /* remove */
    memset(thor, 0x7c, 20);
    memset(&pr, 0, sizeof pr);
    pr.has_router_remove = true; pr.router.chain = ROUTER_CHAIN_EVM; memcpy(pr.router.addr, thor, 20);
    CHECK_EQ(consent_begin_policy_multi(&c, "r3", &pr, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a); consent_pin_result(&c, true, ms, &a);
    CHECK(!routers_is_allowed(&c.p.routers, ROUTER_CHAIN_EVM, thor));
    CHECK_EQ(c.p.routers.n, 0);
    /* bad requests: both at once, unknown chain, list full */
    pr.has_router_add = true;
    CHECK_EQ(consent_begin_policy_multi(&c, "r4", &pr, ms, &why), CONSENT_R_DENIED);
    pr.has_router_remove = false; pr.router.chain = 9;
    CHECK_EQ(consent_begin_policy_multi(&c, "r5", &pr, ms, &why), CONSENT_R_DENIED);
    pr.router.chain = ROUTER_CHAIN_SOL;
    for (i = 0; i < ROUTERS_MAX_OWNER; i++) { memset(pr.router.addr, 0x30 + i, 32); CHECK_EQ(routers_add(&c.p.routers, ROUTER_CHAIN_SOL, pr.router.addr), 0); }
    memset(pr.router.addr, 0x55, 32);
    CHECK_EQ(consent_begin_policy_multi(&c, "r6", &pr, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_POLICY);
    memset(pr.router.addr, 0x31, 32); /* already present: fine (no-op) */
    CHECK_EQ(consent_begin_policy_multi(&c, "r7", &pr, ms, &why), CONSENT_R_PENDING);
    /* lock / duress gates */
    consent_cancel_pending(&c, NULL);
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_policy_multi(&c, "r8", &pr, ms, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_LOCKED);
}

/* A second rhythm: set under the real PIN (or right after set_pin), must differ, unlocks into the decoy slot
 * with a reply and state indistinguishable from a real unlock except for the active slot. */
static void test_duress_rhythm(void)
{
    consent_t c;
    consent_action_t a;
    consent_reason_t why;
    uint32_t ms;
    static const uint8_t DUR[8] = {'P', 'R', 6, 0x12, (uint8_t)~6, (uint8_t)~0x12, 0x5A, 0xA5};
    ready(&c, &ms);
    /* gated flow: real rhythm first (VERIFY -> CONTINUE), then the new one twice */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d1", NULL, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(a.op, CONSENT_OP_SET_DURESS_PIN); CHECK_MEM(a.pin, PIN, 8);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_CONTINUE);
    CHECK_STR(a.id, "d1");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN); /* still pending: now the new rhythm */
    CHECK(!c.p.duress_pin_set);
    /* identical to the real PIN: refused as pin_mismatch */
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_MISMATCH);
    CHECK_STR(a.id, "d1");
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    CHECK_EQ(c.p.pin_failures, 0);
    /* confirmation that differs from the first entry: mismatch too */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d2", NULL, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a); consent_pin_result(&c, true, ms, &a);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_rhythm(&c, PIN2, ms, &a), CONSENT_RHYTHM_MISMATCH);
    /* a wrong real rhythm at stage 0 counts as a failure like anywhere else */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d3", NULL, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN2, ms, &a);
    CHECK_EQ(consent_pin_result(&c, false, ms, &a), CONSENT_PIN_WRONG);
    CHECK_EQ(c.p.pin_failures, 1);
    /* the good case */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d4", NULL, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a);
    CHECK_EQ(consent_pin_result(&c, true, ms, &a), CONSENT_PIN_CONTINUE);
    CHECK_EQ(c.p.pin_failures, 0);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_SET);
    CHECK_EQ(a.op, CONSENT_OP_SET_DURESS_PIN); CHECK_STR(a.id, "d4"); CHECK_MEM(a.pin, DUR, 8);
    consent_mark_duress_pin_set(&c);
    CHECK(c.p.duress_pin_set && c.dirty);
    /* replaceable: a second set_duress_pin is allowed (unlike set_pin) */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d5", NULL, ms, &why), CONSENT_R_PENDING);
    consent_cancel_pending(&c, NULL);
    /* init-flow step 3: the real code is handed in, no VERIFY round */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d6", PIN, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_CONFIRM); /* first entry recorded ... */
    CHECK_EQ(consent_rhythm(&c, PIN, ms, &a), CONSENT_RHYTHM_MISMATCH); /* ... but equal to the real one */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d7", PIN, ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_CONFIRM);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_SET);
    CHECK_STR(a.id, "d7");
    /* ---- unlock with the duress rhythm ---- */
    consent_lock(&c, NULL);
    c.p.pin_failures = 2; /* a couple of earlier typos */
    c.dirty = false;
    CHECK_EQ(consent_begin_unlock(&c, "u1", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_rhythm(&c, DUR, ms, &a), CONSENT_RHYTHM_VERIFY);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_DURESS, ms, &a), CONSENT_PIN_OK); /* same result code as real */
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK); CHECK_STR(a.id, "u1");             /* same action -> same reply */
    CHECK(!c.locked);
    CHECK(c.p.duress);
    CHECK_EQ(consent_active_slot(&c), CONSENT_SLOT_DECOY);
    CHECK_EQ(c.p.pin_failures, 2); /* not incremented, not reset: nothing to report */
    CHECK(c.dirty);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_NONE);
    /* in duress: the decoy signs, owner-authority operations are refused, incl. set_duress_pin and decoy_addresses */
    CHECK_EQ(consent_begin_sign(&c, "s1", 1, P_SYS, 1, false, 0, ms, &a, &why), CONSENT_R_PENDING);
    consent_display_shown(&c, 0, 1);
    CHECK(consent_touch(&c, false, ms, &a));
    CHECK_EQ(a.slot, CONSENT_SLOT_DECOY);
    /* in duress these go pending like in the real slot (main.c verifies the rhythm that opened the decoy) */
    CHECK_EQ(consent_begin_set_duress_pin(&c, "d8", NULL, ms, &why), CONSENT_R_PENDING);
    consent_cancel_pending(&c, NULL);
    CHECK_EQ(consent_begin_decoy_addresses(&c, "x1", ms, &why), CONSENT_R_PENDING);
    consent_cancel_pending(&c, NULL);
    /* the real rhythm unlocks out of duress, as before */
    consent_lock(&c, NULL);
    CHECK_EQ(consent_begin_unlock(&c, "u2", ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, PIN, ms, &a);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_REAL, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_UNLOCK);
    CHECK(!c.p.duress); CHECK_EQ(c.p.pin_failures, 0);
    /* the duress rhythm on anything but unlock is just a wrong PIN */
    CHECK_EQ(consent_begin_policy(&c, "p1", SOL, ms, &why), CONSENT_R_PENDING);
    consent_rhythm(&c, DUR, ms, &a);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_DURESS, ms, &a), CONSENT_PIN_WRONG);
    CHECK_EQ(c.p.pin_failures, 1);
    CHECK(!c.p.duress);
    /* decoy_addresses: PIN-gated, completes on the real verdict */
    CHECK_EQ(consent_begin_decoy_addresses(&c, "x2", ms, &why), CONSENT_R_PENDING);
    CHECK_EQ(consent_pending_need(&c), CONSENT_NEED_PIN);
    consent_rhythm(&c, PIN, ms, &a);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_REAL, ms, &a), CONSENT_PIN_OK);
    CHECK_EQ(a.op, CONSENT_OP_DECOY_ADDRESSES); CHECK_STR(a.id, "x2");
    /* ... and a duress unlock after wrong attempts does not wipe (4 wrong + duress != 5) */
    consent_lock(&c, NULL);
    c.p.pin_failures = 4;
    consent_begin_unlock(&c, "u3", ms, &why);
    consent_rhythm(&c, DUR, ms, &a);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_DURESS, ms, &a), CONSENT_PIN_OK);
    CHECK(c.p.initialized); CHECK_EQ(c.p.pin_failures, 4);
    /* unlock in duress with a wrong code still counts and still wipes on the fifth */
    consent_lock(&c, NULL);
    consent_begin_unlock(&c, "u4", ms, &why);
    consent_rhythm(&c, PIN2, ms, &a);
    CHECK_EQ(consent_pin_verdict(&c, CONSENT_VERDICT_WRONG, ms, &a), CONSENT_PIN_WIPE);
}

static void test_update_apply(void)
{
    consent_t c;
    consent_reason_t why;
    consent_action_t act;
    const uint8_t hash[4] = {1, 2, 3, 4};
    memset(&c, 0, sizeof c);
    consent_init(&c, NULL, 1000);
    c.locked = false;
    /* blank controller: a long touch is enough */
    CHECK_EQ(consent_begin_update_apply(&c, "u1", "0.5.0", hash, 1000, &why), CONSENT_R_PENDING);
    CHECK_EQ(c.pending.need, CONSENT_NEED_LONG_TOUCH);
    CHECK(!consent_touch(&c, false, 1100, &act)); /* a tap is not consent */
    CHECK(consent_touch(&c, true, 1200, &act));
    CHECK_EQ(act.op, CONSENT_OP_UPDATE_APPLY);
    CHECK_STR(act.update_version, "0.5.0");
    CHECK_EQ(act.update_hash[3], 4);
    /* too long a version string is refused */
    CHECK_EQ(consent_begin_update_apply(&c, "u2", "0123456789abcdefg", hash, 1300, &why), CONSENT_R_DENIED);
    /* with a wallet: PIN must exist and the controller must be unlocked */
    c.p.initialized = true;
    c.p.pin_set = false;
    CHECK_EQ(consent_begin_update_apply(&c, "u3", "0.5.0", hash, 1400, &why), CONSENT_R_DENIED);
    CHECK_EQ(why, CONSENT_DENY_PIN_NOT_SET);
    c.p.pin_set = true;
    c.locked = true;
    CHECK_EQ(consent_begin_update_apply(&c, "u4", "0.5.0", hash, 1500, &why), CONSENT_R_DENIED);
    c.locked = false;
    CHECK_EQ(consent_begin_update_apply(&c, "u5", "0.5.0", hash, 1600, &why), CONSENT_R_PENDING);
}

static void test_time_floor_persists(void)
{
    consent_t c;
    uint32_t ms;
    ready(&c, &ms); /* T0 accepted */
    c.dirty = false;
    CHECK_U64(c.p.last_unix, T0);
    /* within the hour: the floor stays, nothing to write */
    CHECK(consent_set_time(&c, T0 + 3599, ms + 3599000) != CONSENT_TIME_REJECTED);
    CHECK_U64(c.p.last_unix, T0); CHECK(!c.dirty);
    /* an hour on: floor moves, flash write requested */
    CHECK(consent_set_time(&c, T0 + 3600, ms + 3600000) != CONSENT_TIME_REJECTED);
    CHECK_U64(c.p.last_unix, T0 + 3600); CHECK(c.dirty);
    /* reboot with that record: a host clock behind the floor is refused, the floor itself is fine */
    {
        consent_t r;
        consent_persist_t p = c.p;
        consent_init(&r, &p, 5);
        CHECK_EQ(consent_set_time(&r, T0 + 3599, 5), CONSENT_TIME_REJECTED);
        CHECK(!r.have_time);
        CHECK_EQ(consent_set_time(&r, T0, 5), CONSENT_TIME_REJECTED);
        CHECK(consent_set_time(&r, T0 + 3600, 5) != CONSENT_TIME_REJECTED);
        CHECK(r.have_time);
    }
}

int main(void)
{
    RUN(test_update_apply);
    memset(SYS, 0, 32); memset(JUP, 0x99, 32); memset(TOK, 0x77, 32);
    RUN(test_init_flow);
    RUN(test_sign_touch_and_timeout);
    RUN(test_daily_limit_and_elevate);
    RUN(test_policy_change);
    RUN(test_lock_unlock);
    RUN(test_duress);
    RUN(test_day_rollover);
    RUN(test_host_time_rejected);
    RUN(test_display_gating);
    RUN(test_init_words_flow);
    RUN(test_router_policy);
    RUN(test_duress_rhythm);
    RUN(test_allowance_signs_without_touch);
    RUN(test_allowance_never_covers_tokens);
    RUN(test_allowance_program_outside_needs_touch);
    RUN(test_allowance_cumulative_exhaustion);
    RUN(test_allowance_expiry);
    RUN(test_allowance_revoke_lock_duress);
    RUN(test_set_pin_flow);
    RUN(test_unlock_wrong_pin_and_wipe);
    RUN(test_pin_timeout_and_lock_cancel);
    RUN(test_time_floor_persists);
    RUN(test_chain_limits);
    TEST_END();
}
