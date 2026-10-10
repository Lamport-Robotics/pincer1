#include "touch.h"

#include "harness.h"

static void test_short_touch(void)
{
    touch_t t;
    uint32_t ms = 1000;
    touch_init(&t);
    CHECK_EQ(touch_update(&t, true, ms), TOUCH_EV_NONE);           /* raw high, not yet debounced */
    CHECK_EQ(touch_update(&t, true, ms + 10), TOUCH_EV_NONE);
    CHECK_EQ(touch_update(&t, true, ms + 30), TOUCH_EV_PRESS);     /* 30 ms stable -> press */
    CHECK(touch_is_down(&t));
    CHECK_EQ(touch_update(&t, true, ms + 500), TOUCH_EV_NONE);
    CHECK_EQ(touch_update(&t, false, ms + 600), TOUCH_EV_NONE);
    CHECK_EQ(touch_update(&t, false, ms + 630), TOUCH_EV_SHORT);   /* released before 3 s */
    CHECK(!touch_is_down(&t));
}

static void test_glitch_filtered(void)
{
    touch_t t;
    touch_init(&t);
    CHECK_EQ(touch_update(&t, true, 0), TOUCH_EV_NONE);
    CHECK_EQ(touch_update(&t, false, 10), TOUCH_EV_NONE);  /* 10 ms blip */
    CHECK_EQ(touch_update(&t, false, 100), TOUCH_EV_NONE);
    CHECK(!touch_is_down(&t));
}

static void test_long_touch(void)
{
    touch_t t;
    uint32_t ms;
    touch_init(&t);
    touch_update(&t, true, 0);
    CHECK_EQ(touch_update(&t, true, 30), TOUCH_EV_PRESS);
    for (ms = 35; ms < 3025; ms += 5) CHECK_EQ(touch_update(&t, true, ms), TOUCH_EV_NONE);
    CHECK_EQ(touch_update(&t, true, 3030), TOUCH_EV_LONG);          /* 3000 ms after the press */
    CHECK_EQ(touch_update(&t, true, 4000), TOUCH_EV_NONE);          /* fires once */
    touch_update(&t, false, 4100);
    CHECK_EQ(touch_update(&t, false, 4130), TOUCH_EV_RELEASE);      /* not a short touch */
}

static void test_wraparound(void)
{
    touch_t t;
    uint32_t ms = 0xFFFFFFF0u;
    touch_init(&t);
    touch_update(&t, true, ms);
    CHECK_EQ(touch_update(&t, true, ms + 30), TOUCH_EV_PRESS); /* crosses the 32-bit boundary */
    CHECK_EQ(touch_update(&t, true, ms + 3030), TOUCH_EV_LONG); /* 3000 ms after the press */
}

/* ---- PIN rhythm ---- */

/* Taps at the given times (ms); ticks every 5 ms in between and for `tail_ms` after the last one. */
static touch_rhythm_event_t run_rhythm(touch_rhythm_capture_t *c, const uint32_t *taps, int n, uint32_t tail_ms,
                                       touch_rhythm_t *out, int *tap_events)
{
    uint32_t t = taps[0], end = taps[n - 1] + tail_ms;
    int i = 0;
    *tap_events = 0;
    for (; t <= end; t += 5) {
        bool press = (i < n && taps[i] == t);
        touch_rhythm_event_t ev;
        if (press) i++;
        ev = touch_rhythm_update(c, press, t, out);
        if (ev == TOUCH_RHYTHM_TAP) (*tap_events)++;
        else if (ev != TOUCH_RHYTHM_NONE) return ev;
    }
    return TOUCH_RHYTHM_NONE;
}

static void test_rhythm_capture(void)
{
    touch_rhythm_capture_t c;
    touch_rhythm_t r, exp;
    int taps;
    /* "short short long short": 5 taps, gaps 300, 400, 900, 200 ms */
    const uint32_t t1[5] = {1000, 1300, 1700, 2600, 2800};
    touch_rhythm_start(&c);
    CHECK(!touch_rhythm_capturing(&c));
    CHECK_EQ(run_rhythm(&c, t1, 5, TOUCH_RHYTHM_END_MS, &r, &taps), TOUCH_RHYTHM_DONE);
    CHECK_EQ(taps, 5);
    CHECK(touch_rhythm_from_str("SSLS", &exp));
    CHECK(touch_rhythm_equal(&r, &exp));
    CHECK(touch_rhythm_valid(&r));
    CHECK_EQ(r.n_taps, 5);
    CHECK_EQ(r.gaps, 0x04);
    /* capture restarted automatically, ready for another attempt */
    CHECK(c.active && c.n_taps == 0);
    /* not finished before the end pause */
    touch_rhythm_start(&c);
    CHECK_EQ(run_rhythm(&c, t1, 5, TOUCH_RHYTHM_END_MS - 10, &r, &taps), TOUCH_RHYTHM_NONE);
    CHECK(touch_rhythm_capturing(&c));
    touch_rhythm_stop(&c);
    CHECK_EQ(touch_rhythm_update(&c, true, 5000, &r), TOUCH_RHYTHM_NONE); /* stopped: ignored */
}

static void test_rhythm_boundaries(void)
{
    touch_rhythm_capture_t c;
    touch_rhythm_t r;
    int taps;
    /* 595 ms = short, 600 ms = long (the tick runs every 5 ms, so tap times sit on that grid) */
    const uint32_t t1[4] = {0, 595, 1190, 1785};
    const uint32_t t2[4] = {0, 600, 1200, 1800};
    touch_rhythm_start(&c);
    CHECK_EQ(run_rhythm(&c, t1, 4, TOUCH_RHYTHM_END_MS, &r, &taps), TOUCH_RHYTHM_DONE);
    CHECK_EQ(r.gaps, 0x00);
    touch_rhythm_start(&c);
    CHECK_EQ(run_rhythm(&c, t2, 4, TOUCH_RHYTHM_END_MS, &r, &taps), TOUCH_RHYTHM_DONE);
    CHECK_EQ(r.gaps, 0x07);
    /* a gap just under the end pause is still a long gap, not an end */
    {
        const uint32_t t3[4] = {0, 1495, 1700, 1900};
        touch_rhythm_start(&c);
        CHECK_EQ(run_rhythm(&c, t3, 4, TOUCH_RHYTHM_END_MS, &r, &taps), TOUCH_RHYTHM_DONE);
        CHECK_EQ(r.n_taps, 4);
        CHECK_EQ(r.gaps, 0x01);
    }
    /* 32-bit tick wraparound inside a rhythm */
    {
        /* gaps 770 (long), 510 (short), 1025 (long) across the 32-bit boundary */
        const uint32_t t4[4] = {0xFFFFFC00u, 0xFFFFFF02u, 0x00000100u, 0x00000501u};
        uint32_t t = t4[0], end = t4[3] + TOUCH_RHYTHM_END_MS;
        int i = 0;
        touch_rhythm_event_t ev = TOUCH_RHYTHM_NONE;
        touch_rhythm_start(&c);
        for (; t != end + 5; t += 5) {
            bool press = (i < 4 && t4[i] == t);
            if (press) i++;
            ev = touch_rhythm_update(&c, press, t, &r);
            if (ev == TOUCH_RHYTHM_DONE) break;
        }
        CHECK_EQ(ev, TOUCH_RHYTHM_DONE);
        CHECK_EQ(r.n_taps, 4);
        CHECK_EQ(r.gaps, 0x05);
    }
}

static void test_rhythm_too_few_and_max(void)
{
    touch_rhythm_capture_t c;
    touch_rhythm_t r;
    int taps;
    const uint32_t t1[3] = {0, 300, 600};
    const uint32_t t8[8] = {0, 200, 400, 600, 800, 1000, 1200, 1400};
    touch_rhythm_start(&c);
    CHECK_EQ(run_rhythm(&c, t1, 3, TOUCH_RHYTHM_END_MS, &r, &taps), TOUCH_RHYTHM_TOO_FEW);
    CHECK_EQ(r.n_taps, 3);
    CHECK(!touch_rhythm_valid(&r));
    CHECK(c.active && c.n_taps == 0); /* restarted */
    /* 8 taps end the rhythm immediately on the 8th tap */
    touch_rhythm_start(&c);
    CHECK_EQ(run_rhythm(&c, t8, 8, 0, &r, &taps), TOUCH_RHYTHM_DONE);
    CHECK_EQ(taps, 7);
    CHECK_EQ(r.n_taps, 8);
    CHECK_EQ(r.gaps, 0x00);
    CHECK(touch_rhythm_valid(&r));
}

static void test_rhythm_encode_and_strings(void)
{
    touch_rhythm_t r, back;
    uint8_t code[TOUCH_RHYTHM_CODE_LEN], code2[TOUCH_RHYTHM_CODE_LEN];
    char s[16];
    CHECK(touch_rhythm_from_str("SLLSLSS", &r)); /* 8 taps */
    CHECK_EQ(r.n_taps, 8);
    CHECK_EQ(r.gaps, 0x16);
    touch_rhythm_to_str(&r, s, sizeof s);
    CHECK_STR(s, "SLLSLSS");
    touch_rhythm_encode(&r, code);
    CHECK_EQ(code[0], 'P'); CHECK_EQ(code[1], 'R'); CHECK_EQ(code[2], 8); CHECK_EQ(code[3], 0x16);
    CHECK_EQ(code[4], (uint8_t)~8); CHECK_EQ(code[5], (uint8_t)~0x16); CHECK_EQ(code[6], 0x5A); CHECK_EQ(code[7], 0xA5);
    /* different rhythms -> different codes; same length different gaps too */
    CHECK(touch_rhythm_from_str("SLLSLSL", &back));
    touch_rhythm_encode(&back, code2);
    CHECK(memcmp(code, code2, 8) != 0);
    CHECK(touch_rhythm_from_str("sss", &back));
    CHECK_EQ(back.n_taps, 4);
    CHECK(!touch_rhythm_from_str("SS", &back));        /* 3 taps: too few */
    CHECK(!touch_rhythm_from_str("SSSSSSSS", &back));  /* 9 taps */
    CHECK(!touch_rhythm_from_str("SSX", &back));
    CHECK(!touch_rhythm_from_str(NULL, &back));
    r.n_taps = 5; r.gaps = 0x10; /* bit beyond the 4 real gaps */
    CHECK(!touch_rhythm_valid(&r));
    touch_rhythm_to_str(&r, s, 3);
    CHECK_STR(s, "SS"); /* truncated safely */
}

int main(void)
{
    RUN(test_short_touch);
    RUN(test_glitch_filtered);
    RUN(test_long_touch);
    RUN(test_wraparound);
    RUN(test_rhythm_capture);
    RUN(test_rhythm_boundaries);
    RUN(test_rhythm_too_few_and_max);
    RUN(test_rhythm_encode_and_strings);
    TEST_END();
}
