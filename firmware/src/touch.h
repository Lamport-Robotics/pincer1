/* Pincer1 - TTP223 touch pad: debounce + short/long classification, and the PIN tap-rhythm capture.
 * PURE LOGIC (caller samples the pin). */
#ifndef PINCER_TOUCH_H
#define PINCER_TOUCH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TOUCH_DEBOUNCE_MS 30u
#define TOUCH_LONG_MS 3000u

typedef enum {
    TOUCH_EV_NONE = 0,
    TOUCH_EV_PRESS,   /* debounced finger down (protocol event "touch") */
    TOUCH_EV_SHORT,   /* released before TOUCH_LONG_MS */
    TOUCH_EV_LONG,    /* held for TOUCH_LONG_MS, fired once while still held (event "long_touch") */
    TOUCH_EV_RELEASE, /* finger up after a long touch (no protocol meaning) */
} touch_event_t;

typedef struct {
    bool raw_last;
    uint32_t raw_since_ms;
    bool stable;
    uint32_t press_ms;
    bool long_fired;
} touch_t;

void touch_init(touch_t *t);
/* Feed one sample (true = pad active). Returns at most one event per call. */
touch_event_t touch_update(touch_t *t, bool raw, uint32_t now_ms);
bool touch_is_down(const touch_t *t);

/* ---- PIN rhythm ----
 * 4..8 taps. The gap between consecutive taps (press to press) is "short" below
 * TOUCH_RHYTHM_LONG_GAP_MS and "long" from there up; a pause of TOUCH_RHYTHM_END_MS ends the rhythm.
 * "short short long short" = 5 taps, gaps S S L S. The code is what the SE050 verifies (UserID). */
#define TOUCH_RHYTHM_MIN_TAPS 4
#define TOUCH_RHYTHM_MAX_TAPS 8
#define TOUCH_RHYTHM_LONG_GAP_MS 600u
#define TOUCH_RHYTHM_END_MS 1500u
#define TOUCH_RHYTHM_CODE_LEN 8

typedef struct {
    uint8_t n_taps; /* 4..8 */
    uint8_t gaps;   /* bit i (0 = first gap) set = long; n_taps-1 gaps */
} touch_rhythm_t;

typedef struct {
    bool active;
    uint8_t n_taps;
    uint8_t gaps;
    uint32_t last_tap_ms;
} touch_rhythm_capture_t;

typedef enum {
    TOUCH_RHYTHM_NONE = 0,
    TOUCH_RHYTHM_TAP,     /* a tap was recorded (UI feedback) */
    TOUCH_RHYTHM_DONE,    /* `out` holds a valid rhythm; capture is idle again */
    TOUCH_RHYTHM_TOO_FEW, /* ended with fewer than 4 taps; capture restarted */
} touch_rhythm_event_t;

void touch_rhythm_start(touch_rhythm_capture_t *c);
void touch_rhythm_stop(touch_rhythm_capture_t *c);
bool touch_rhythm_capturing(const touch_rhythm_capture_t *c); /* at least one tap recorded */
/* Call on every tick with `press` = a TOUCH_EV_PRESS happened at now_ms. */
touch_rhythm_event_t touch_rhythm_update(touch_rhythm_capture_t *c, bool press, uint32_t now_ms, touch_rhythm_t *out);

bool touch_rhythm_valid(const touch_rhythm_t *r);
bool touch_rhythm_equal(const touch_rhythm_t *a, const touch_rhythm_t *b);
/* Fixed 8-byte encoding used as the SE050 UserID value. */
void touch_rhythm_encode(const touch_rhythm_t *r, uint8_t out[TOUCH_RHYTHM_CODE_LEN]);
/* "SSLS" <-> rhythm (one letter per gap, so 3..7 letters). Returns false on bad input. */
bool touch_rhythm_from_str(const char *s, touch_rhythm_t *out);
void touch_rhythm_to_str(const touch_rhythm_t *r, char *out, size_t cap);
#endif
