#include "touch.h"

#include <string.h>

void touch_init(touch_t *t) { memset(t, 0, sizeof *t); }

bool touch_is_down(const touch_t *t) { return t->stable; }

touch_event_t touch_update(touch_t *t, bool raw, uint32_t now_ms)
{
    if (raw != t->raw_last) {
        t->raw_last = raw;
        t->raw_since_ms = now_ms;
    }
    if (raw != t->stable && (uint32_t)(now_ms - t->raw_since_ms) >= TOUCH_DEBOUNCE_MS) {
        t->stable = raw;
        if (raw) {
            t->press_ms = now_ms;
            t->long_fired = false;
            return TOUCH_EV_PRESS;
        }
        return t->long_fired ? TOUCH_EV_RELEASE : TOUCH_EV_SHORT;
    }
    if (t->stable && !t->long_fired && (uint32_t)(now_ms - t->press_ms) >= TOUCH_LONG_MS) {
        t->long_fired = true;
        return TOUCH_EV_LONG;
    }
    return TOUCH_EV_NONE;
}

/* ---- rhythm ---- */

void touch_rhythm_start(touch_rhythm_capture_t *c)
{
    memset(c, 0, sizeof *c);
    c->active = true;
}

void touch_rhythm_stop(touch_rhythm_capture_t *c) { memset(c, 0, sizeof *c); }

bool touch_rhythm_capturing(const touch_rhythm_capture_t *c) { return c->active && c->n_taps > 0; }

touch_rhythm_event_t touch_rhythm_update(touch_rhythm_capture_t *c, bool press, uint32_t now_ms, touch_rhythm_t *out)
{
    if (!c->active) return TOUCH_RHYTHM_NONE;
    if (press) {
        if (c->n_taps > 0) {
            uint32_t gap = now_ms - c->last_tap_ms;
            if (gap >= TOUCH_RHYTHM_LONG_GAP_MS) c->gaps |= (uint8_t)(1u << (c->n_taps - 1));
        }
        c->n_taps++;
        c->last_tap_ms = now_ms;
        if (c->n_taps >= TOUCH_RHYTHM_MAX_TAPS) {
            out->n_taps = c->n_taps;
            out->gaps = c->gaps;
            touch_rhythm_start(c);
            return TOUCH_RHYTHM_DONE;
        }
        return TOUCH_RHYTHM_TAP;
    }
    if (c->n_taps > 0 && (uint32_t)(now_ms - c->last_tap_ms) >= TOUCH_RHYTHM_END_MS) {
        bool enough = c->n_taps >= TOUCH_RHYTHM_MIN_TAPS;
        out->n_taps = c->n_taps;
        out->gaps = c->gaps;
        touch_rhythm_start(c); /* ready for the next attempt */
        return enough ? TOUCH_RHYTHM_DONE : TOUCH_RHYTHM_TOO_FEW;
    }
    return TOUCH_RHYTHM_NONE;
}

bool touch_rhythm_valid(const touch_rhythm_t *r)
{
    if (r->n_taps < TOUCH_RHYTHM_MIN_TAPS || r->n_taps > TOUCH_RHYTHM_MAX_TAPS) return false;
    return (r->gaps >> (r->n_taps - 1)) == 0; /* no bits beyond the real gaps */
}

bool touch_rhythm_equal(const touch_rhythm_t *a, const touch_rhythm_t *b)
{
    return a->n_taps == b->n_taps && a->gaps == b->gaps;
}

void touch_rhythm_encode(const touch_rhythm_t *r, uint8_t out[TOUCH_RHYTHM_CODE_LEN])
{
    /* 'P' 'R' n gaps ~n ~gaps 0x5A 0xA5: 8 bytes, within the SE050 UserID range of 4..16. */
    out[0] = 'P';
    out[1] = 'R';
    out[2] = r->n_taps;
    out[3] = r->gaps;
    out[4] = (uint8_t)~r->n_taps;
    out[5] = (uint8_t)~r->gaps;
    out[6] = 0x5A;
    out[7] = 0xA5;
}

bool touch_rhythm_from_str(const char *s, touch_rhythm_t *out)
{
    size_t n = s ? strlen(s) : 0, i;
    if (n + 1 < TOUCH_RHYTHM_MIN_TAPS || n + 1 > TOUCH_RHYTHM_MAX_TAPS) return false;
    out->n_taps = (uint8_t)(n + 1);
    out->gaps = 0;
    for (i = 0; i < n; i++) {
        if (s[i] == 'L' || s[i] == 'l') out->gaps |= (uint8_t)(1u << i);
        else if (s[i] != 'S' && s[i] != 's') return false;
    }
    return true;
}

void touch_rhythm_to_str(const touch_rhythm_t *r, char *out, size_t cap)
{
    size_t i, n = r->n_taps ? (size_t)r->n_taps - 1 : 0;
    if (cap == 0) return;
    if (n + 1 > cap) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = (r->gaps & (1u << i)) ? 'L' : 'S';
    out[n] = 0;
}
