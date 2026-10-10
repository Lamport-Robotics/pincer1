#include "consent.h"

#include <string.h>

#include "util.h"

#define SECONDS_PER_DAY CONSENT_SECONDS_PER_DAY

static void copy_id(char *dst, const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n >= CONSENT_ID_MAX) n = CONSENT_ID_MAX - 1;
    memcpy(dst, id, n);
    dst[n] = 0;
}

static void clear_pending(consent_t *c) { secure_zero(&c->pending, sizeof c->pending); }

static void clear_allowance(consent_t *c)
{
    if (c->p.allowance.active) c->dirty = true;
    memset(&c->p.allowance, 0, sizeof c->p.allowance);
}

static void clear_elevate(consent_t *c) { memset(&c->elevate, 0, sizeof c->elevate); }

static bool time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static consent_result_t deny(consent_reason_t r, consent_reason_t *why)
{
    if (why) *why = r;
    return CONSENT_R_DENIED;
}

void consent_init(consent_t *c, const consent_persist_t *persisted, uint32_t now_ms)
{
    memset(c, 0, sizeof *c);
    if (persisted) c->p = *persisted;
    /* A power cycle must never be a way around a lock: initialised controllers boot locked. */
    c->locked = c->p.initialized;
    c->tick_last_ms = now_ms;
}

void consent_init_inheritance(consent_t *c, const consent_inh_persist_t *persisted)
{
    memset(&c->ih, 0, sizeof c->ih);
    if (persisted) c->ih = *persisted;
    /* Resume, never rewind: the stored value is the floor from now on. */
    if (c->ih.last_touch_powered_seconds > c->ih.powered_seconds)
        c->ih.last_touch_powered_seconds = c->ih.powered_seconds;
    if (c->ih.inh.n_warn > CONSENT_INH_MAX_WARN) c->ih.inh.n_warn = CONSENT_INH_MAX_WARN;
    c->ih_persisted_seconds = c->ih.powered_seconds;
    c->ih_dirty = false;
}

/* ---- dead-man timer ---- */

static void advance_uptime(consent_t *c, uint32_t now_ms)
{
    uint32_t delta = now_ms - c->tick_last_ms; /* wraps correctly at 49.7 days */
    uint32_t whole;
    c->tick_last_ms = now_ms;
    c->tick_frac_ms += delta;
    whole = c->tick_frac_ms / 1000u;
    if (!whole) return;
    c->tick_frac_ms -= whole * 1000u;
    c->ih.powered_seconds = u64_sat_add(c->ih.powered_seconds, whole);
    if (c->ih.powered_seconds - c->ih_persisted_seconds >= CONSENT_POWERED_CHECKPOINT_S) c->ih_dirty = true;
}

static void mark_touch(consent_t *c)
{
    c->ih.last_touch_powered_seconds = c->ih.powered_seconds;
    if (c->ih.inh.warned) {
        c->ih.inh.warned = 0; /* a new cycle warns again */
        c->ih_dirty = true;
    }
    /* Pull the hourly checkpoint forward so a touch is not lost to a power cut, without writing flash on
     * every tap of a rhythm: at most one write per checkpoint period comes from touches. */
    if (c->ih.powered_seconds - c->ih_persisted_seconds >= CONSENT_POWERED_CHECKPOINT_S) c->ih_dirty = true;
}

void consent_touch_seen(consent_t *c) { mark_touch(c); }

uint64_t consent_seconds_since_touch(const consent_t *c)
{
    if (c->ih.last_touch_powered_seconds > c->ih.powered_seconds) return 0;
    return c->ih.powered_seconds - c->ih.last_touch_powered_seconds;
}

uint32_t consent_days_since_touch_tenths(const consent_t *c)
{
    uint64_t s = consent_seconds_since_touch(c);
    uint64_t days = s / SECONDS_PER_DAY, tenths = (s % SECONDS_PER_DAY) * 10u / SECONDS_PER_DAY;
    uint64_t v = days * 10u + tenths;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

void consent_inheritance_persisted(consent_t *c)
{
    c->ih_persisted_seconds = c->ih.powered_seconds;
    c->ih_dirty = false;
}

consent_result_t consent_inheritance_release(const consent_t *c, consent_reason_t *why)
{
    if (!c->ih.inh.armed) return deny(CONSENT_DENY_NOT_ARMED, why);
    if (consent_seconds_since_touch(c) < (uint64_t)c->ih.inh.inactivity_days * SECONDS_PER_DAY)
        return deny(CONSENT_DENY_NOT_EXPIRED, why);
    return CONSENT_R_DONE;
}

bool consent_inheritance_warning(consent_t *c, uint32_t *days_left)
{
    const consent_inheritance_t *h = &c->ih.inh;
    uint64_t total, since, remaining;
    uint8_t i;
    if (!h->armed) return false;
    total = (uint64_t)h->inactivity_days * SECONDS_PER_DAY;
    since = consent_seconds_since_touch(c);
    remaining = since >= total ? 0 : total - since;
    for (i = 0; i < h->n_warn && i < CONSENT_INH_MAX_WARN; i++) {
        if (c->ih.inh.warned & (1u << i)) continue;
        if (remaining <= (uint64_t)h->warn_days[i] * SECONDS_PER_DAY) {
            c->ih.inh.warned |= (uint8_t)(1u << i);
            c->ih_dirty = true;
            *days_left = h->warn_days[i];
            return true;
        }
    }
    return false;
}

bool consent_now_unix(const consent_t *c, uint32_t now_ms, uint64_t *out)
{
    if (!c->have_time) return false;
    *out = c->unix_at_sync + (uint64_t)(uint32_t)(now_ms - c->ms_at_sync) / 1000u;
    return true;
}

static bool allowance_expired_at(const consent_t *c, uint64_t unix_now)
{
    return c->p.allowance.active && unix_now >= c->p.allowance.until_unix;
}

consent_time_result_t consent_set_time(consent_t *c, uint64_t unix_now, uint32_t now_ms)
{
    uint32_t day = (uint32_t)(unix_now / SECONDS_PER_DAY);
    bool changed = false;
    if (c->have_time) {
        uint64_t cur;
        /* Backwards relative to the last accepted value, or further ahead than the controller could have
         * missed: the host is lying (or broken). Keep what we had. */
        if (unix_now < c->unix_at_sync) return CONSENT_TIME_REJECTED;
        if (consent_now_unix(c, now_ms, &cur) && unix_now > u64_sat_add(cur, CONSENT_TIME_MAX_JUMP_S)) return CONSENT_TIME_REJECTED;
    }
    c->have_time = true;
    c->unix_at_sync = unix_now;
    c->ms_at_sync = now_ms;
    if (c->p.day == 0) {
        c->p.day = day; /* first time ever: nothing to reset */
        c->dirty = true;
        changed = true;
    } else if (c->p.day != day) {
        /* A host day change alone never resets the counters: the controller must also have been powered for
         * CONSENT_TIME_MIN_POWERED_S since the last rollover. Otherwise the day stays as it was and the
         * rollover happens at the next time message once the uptime is there. */
        uint64_t since = c->ih.powered_seconds >= c->p.rollover_powered_seconds
                             ? c->ih.powered_seconds - c->p.rollover_powered_seconds : 0;
        if (since >= CONSENT_TIME_MIN_POWERED_S) {
            c->p.spent_today = 0;
            u256_zero(&c->p.spent_today_wei);
            c->p.spent_today_sats = 0;
            c->p.day = day;
            c->p.rollover_powered_seconds = c->ih.powered_seconds;
            c->dirty = true;
            changed = true;
        }
    }
    if (allowance_expired_at(c, unix_now)) {
        clear_allowance(c);
        changed = true;
    }
    return changed ? CONSENT_TIME_CHANGED : CONSENT_TIME_SAME;
}

bool consent_allowance_active(const consent_t *c, uint32_t now_ms)
{
    uint64_t now;
    if (!c->p.allowance.active) return false;
    if (!consent_now_unix(c, now_ms, &now)) return false; /* no host time: cannot prove it is unexpired */
    return now < c->p.allowance.until_unix;
}

bool consent_tick(consent_t *c, uint32_t now_ms, consent_action_t *out)
{
    uint64_t now;
    advance_uptime(c, now_ms);
    if (c->elevate.active && time_reached(now_ms, c->elevate.until_ms)) clear_elevate(c);
    if (c->p.allowance.active && consent_now_unix(c, now_ms, &now) && now >= c->p.allowance.until_unix)
        clear_allowance(c);
    if (c->pending.op != CONSENT_OP_NONE && time_reached(now_ms, c->pending.deadline_ms)) {
        memset(out, 0, sizeof *out);
        out->op = c->pending.op;
        copy_id(out->id, c->pending.id);
        clear_pending(c);
        return true;
    }
    return false;
}

static consent_result_t start_pending(consent_t *c, consent_op_t op, const char *id, consent_need_t need,
                                      uint32_t ttl_ms, uint32_t now_ms)
{
    clear_pending(c);
    c->pending.op = op;
    copy_id(c->pending.id, id);
    c->pending.need = need;
    if (ttl_ms == 0) ttl_ms = CONSENT_DEFAULT_TTL_MS;
    if (ttl_ms > CONSENT_MAX_TTL_MS) ttl_ms = CONSENT_MAX_TTL_MS;
    c->pending.deadline_ms = now_ms + ttl_ms;
    return CONSENT_R_PENDING;
}

/* A display-gated request: nothing is approvable until the caller has rendered the trusted screen built from
 * the controller's own parse; `approve_need` is what the final page then asks for. */
static void gate_on_display(consent_t *c, consent_need_t approve_need)
{
    c->pending.display_gated = true;
    c->pending.approve_need = approve_need;
    c->pending.need = CONSENT_NEED_DISPLAY;
    c->pending.n_pages = 0;
    c->pending.page = 0;
    c->pending.final_shown = false;
}

/* Common gate for everything that needs the real key / owner authority. */
static bool gate(consent_t *c, consent_reason_t *why, bool allow_duress)
{
    if (c->pending.op != CONSENT_OP_NONE) { deny(CONSENT_DENY_BUSY, why); return false; }
    if (c->locked) { deny(CONSENT_DENY_LOCKED, why); return false; }
    if (c->p.duress && !allow_duress) { deny(CONSENT_DENY_POLICY, why); return false; }
    return true;
}

consent_result_t consent_begin_init(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why)
{
    if (c->pending.op != CONSENT_OP_NONE) return deny(CONSENT_DENY_BUSY, why);
    if (c->p.initialized) return deny(CONSENT_DENY_POLICY, why);
    if (c->locked) return deny(CONSENT_DENY_LOCKED, why);
    return start_pending(c, CONSENT_OP_INIT, id, CONSENT_NEED_LONG_TOUCH, 0, now_ms);
}

consent_result_t consent_begin_init_words(consent_t *c, const char *id, uint8_t n_pages, uint32_t now_ms,
                                          consent_reason_t *why)
{
    if (c->pending.op != CONSENT_OP_NONE) return deny(CONSENT_DENY_BUSY, why);
    if (c->p.initialized) return deny(CONSENT_DENY_POLICY, why);
    if (n_pages == 0 || n_pages > CONSENT_MAX_PAGES) return deny(CONSENT_DENY_POLICY, why);
    start_pending(c, CONSENT_OP_INIT_WORDS, id, CONSENT_NEED_TOUCH, 0, now_ms);
    c->pending.deadline_ms = now_ms + CONSENT_INIT_WORDS_TTL_MS; /* longer than any sign ttl: the owner is writing */
    gate_on_display(c, CONSENT_NEED_TOUCH);
    c->pending.n_pages = n_pages;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_set_pin(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why)
{
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED;
    if (c->p.pin_set) return deny(CONSENT_DENY_POLICY, why); /* changing the PIN = re-init */
    return start_pending(c, CONSENT_OP_SET_PIN, id, CONSENT_NEED_PIN, 0, now_ms);
}

consent_result_t consent_begin_set_duress_pin(consent_t *c, const char *id, const uint8_t *real_code, uint32_t now_ms,
                                              consent_reason_t *why)
{
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED; /* never in duress: the decoy must not learn the real flow */
    start_pending(c, CONSENT_OP_SET_DURESS_PIN, id, CONSENT_NEED_PIN, 0, now_ms);
    if (real_code) {
        memcpy(c->pending.pin_real, real_code, CONSENT_PIN_CODE_LEN);
        c->pending.have_pin_real = true;
        c->pending.pin_stage = 1; /* init flow: the real rhythm was just set, go straight to the new one */
    }
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_decoy_addresses(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why)
{
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED; /* refused in duress */
    return start_pending(c, CONSENT_OP_DECOY_ADDRESSES, id, CONSENT_NEED_PIN, 0, now_ms);
}

static bool programs_subset(const uint8_t (*programs)[32], uint8_t n, const consent_allowance_t *a)
{
    uint8_t i, j;
    for (i = 0; i < n; i++) {
        bool found = false;
        for (j = 0; j < a->n_programs; j++)
            if (memcmp(programs[i], a->programs[j], 32) == 0) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

/* check 5 / 6: spent_today(chain) + amount <= daily_limit(chain), or the elevated limit when the elevate
 * targets this chain and is still within its window. */
static bool within_daily_limit(consent_t *c, consent_chain_t chain, uint64_t amount, const u256_t *wei, uint32_t now_ms)
{
    bool elevated;
    if (c->elevate.active && time_reached(now_ms, c->elevate.until_ms)) clear_elevate(c);
    elevated = c->elevate.active && c->elevate.chain == chain;
    if (chain == CONSENT_CHAIN_EVM) {
        u256_t projected;
        const u256_t *limit = elevated ? &c->elevate.limit_wei : &c->p.daily_limit_wei;
        if (u256_add_sat(&projected, &c->p.spent_today_wei, wei)) return false;
        return u256_cmp(&projected, limit) <= 0;
    } else {
        uint64_t limit = elevated ? c->elevate.limit : (chain == CONSENT_CHAIN_BTC ? c->p.daily_limit_sats : c->p.daily_limit);
        uint64_t spent = chain == CONSENT_CHAIN_BTC ? c->p.spent_today_sats : c->p.spent_today;
        uint64_t projected = u64_sat_add(spent, amount);
        return projected <= limit && projected != UINT64_MAX;
    }
}

consent_result_t consent_begin_sign(consent_t *c, const char *id, uint64_t lamports_out,
                                    const uint8_t (*programs)[32], uint8_t n_programs, bool dangerous,
                                    uint32_t ttl_ms, uint32_t now_ms, consent_action_t *now, consent_reason_t *why)
{
    u256_t zero;
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why); /* the keys are unusable until then */
    if (!gate(c, why, true)) return CONSENT_R_DENIED;
    u256_zero(&zero);
    if (!within_daily_limit(c, CONSENT_CHAIN_SOL, lamports_out, &zero, now_ms)) return deny(CONSENT_DENY_OVER_DAILY_LIMIT, why);

    /* allowance: pre-authorised, no touch */
    if (!c->p.duress && consent_allowance_active(c, now_ms) && !dangerous && n_programs > 0 &&
        programs_subset(programs, n_programs, &c->p.allowance) && lamports_out <= c->p.allowance.max_per_tx &&
        u64_sat_add(c->p.allowance.spent, lamports_out) <= c->p.allowance.lamports) {
        memset(now, 0, sizeof *now);
        now->op = CONSENT_OP_SIGN;
        copy_id(now->id, id);
        now->slot = consent_active_slot(c);
        now->chain = CONSENT_CHAIN_SOL;
        now->lamports = lamports_out;
        now->via_allowance = true;
        return CONSENT_R_DONE;
    }

    start_pending(c, CONSENT_OP_SIGN, id, CONSENT_NEED_TOUCH, ttl_ms, now_ms);
    gate_on_display(c, CONSENT_NEED_TOUCH);
    c->pending.chain = CONSENT_CHAIN_SOL;
    c->pending.lamports = lamports_out;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_sign_chain(consent_t *c, const char *id, consent_chain_t chain, uint64_t amount,
                                          const u256_t *wei, uint32_t ttl_ms, uint32_t now_ms, consent_reason_t *why)
{
    u256_t w;
    if (chain >= CONSENT_CHAIN_COUNT) return deny(CONSENT_DENY_POLICY, why);
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, true)) return CONSENT_R_DENIED;
    if (wei) w = *wei; else u256_zero(&w);
    if (chain != CONSENT_CHAIN_EVM) u256_zero(&w);
    if (chain == CONSENT_CHAIN_EVM) amount = 0;
    if (!within_daily_limit(c, chain, amount, &w, now_ms)) return deny(CONSENT_DENY_OVER_DAILY_LIMIT, why);
    start_pending(c, CONSENT_OP_SIGN, id, CONSENT_NEED_TOUCH, ttl_ms, now_ms);
    gate_on_display(c, CONSENT_NEED_TOUCH);
    c->pending.chain = chain;
    c->pending.lamports = amount;
    c->pending.wei = w;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_elevate_chain(consent_t *c, const char *id, consent_chain_t chain, uint64_t limit,
                                             const u256_t *limit_wei, uint32_t now_ms, consent_reason_t *why)
{
    if (chain >= CONSENT_CHAIN_COUNT) return deny(CONSENT_DENY_POLICY, why);
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED;
    start_pending(c, CONSENT_OP_ELEVATE, id, CONSENT_NEED_LONG_TOUCH, 0, now_ms);
    c->pending.chain = chain;
    c->pending.limit = chain == CONSENT_CHAIN_EVM ? 0 : limit;
    if (chain == CONSENT_CHAIN_EVM && limit_wei) c->pending.limit_wei = *limit_wei;
    else u256_zero(&c->pending.limit_wei);
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_elevate(consent_t *c, const char *id, uint64_t limit, uint32_t now_ms,
                                       consent_reason_t *why)
{
    return consent_begin_elevate_chain(c, id, CONSENT_CHAIN_SOL, limit, NULL, now_ms, why);
}

consent_result_t consent_begin_unlock(consent_t *c, const char *id, uint32_t now_ms, consent_reason_t *why)
{
    if (c->pending.op != CONSENT_OP_NONE) return deny(CONSENT_DENY_BUSY, why);
    return start_pending(c, CONSENT_OP_UNLOCK, id, c->p.pin_set ? CONSENT_NEED_PIN : CONSENT_NEED_TOUCH, 0, now_ms);
}

consent_result_t consent_begin_policy_multi(consent_t *c, const char *id, const consent_policy_req_t *req,
                                            uint32_t now_ms, consent_reason_t *why)
{
    if (!req->has_sol && !req->has_wei && !req->has_sats && !req->has_router_add && !req->has_router_remove)
        return deny(CONSENT_DENY_POLICY, why);
    if (req->has_router_add && req->has_router_remove) return deny(CONSENT_DENY_POLICY, why);
    if ((req->has_router_add || req->has_router_remove) && routers_addr_len(req->router.chain) == 0)
        return deny(CONSENT_DENY_POLICY, why);
    if (req->has_router_add && c->p.routers.n >= ROUTERS_MAX_OWNER &&
        !routers_is_allowed(&c->p.routers, req->router.chain, req->router.addr))
        return deny(CONSENT_DENY_POLICY, why); /* list full */
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED;
    start_pending(c, CONSENT_OP_POLICY, id, CONSENT_NEED_PIN, 0, now_ms);
    c->pending.policy = *req;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_policy(consent_t *c, const char *id, uint64_t daily_limit, uint32_t now_ms,
                                      consent_reason_t *why)
{
    consent_policy_req_t req;
    memset(&req, 0, sizeof req);
    req.has_sol = true;
    req.sol = daily_limit;
    return consent_begin_policy_multi(c, id, &req, now_ms, why);
}

consent_result_t consent_begin_allowance(consent_t *c, const char *id, const consent_allowance_t *a,
                                         uint32_t now_ms, consent_reason_t *why)
{
    uint64_t now;
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED;
    if (a->n_programs == 0 || a->n_programs > CONSENT_ALLOWANCE_MAX_PROGRAMS) return deny(CONSENT_DENY_POLICY, why);
    if (a->lamports == 0 || a->max_per_tx == 0 || a->max_per_tx > a->lamports) return deny(CONSENT_DENY_POLICY, why);
    /* Needs host time, and must end in the future. */
    if (!consent_now_unix(c, now_ms, &now) || a->until_unix <= now) return deny(CONSENT_DENY_POLICY, why);
    start_pending(c, CONSENT_OP_ALLOWANCE, id, CONSENT_NEED_LONG_TOUCH, 0, now_ms);
    c->pending.allowance = *a;
    c->pending.allowance.active = true;
    c->pending.allowance.spent = 0;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_inheritance_set(consent_t *c, const char *id, const consent_inheritance_t *req,
                                               bool durable_nonce, uint32_t now_ms, consent_reason_t *why)
{
    uint8_t i;
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED; /* never with the decoy key */
    if (!durable_nonce) return deny(CONSENT_DENY_POLICY, why);
    if (req->inactivity_days == 0 || req->n_warn > CONSENT_INH_MAX_WARN) return deny(CONSENT_DENY_POLICY, why);
    for (i = 0; i < req->n_warn; i++)
        if (req->warn_days[i] == 0 || req->warn_days[i] >= req->inactivity_days) return deny(CONSENT_DENY_POLICY, why);
    /* An arrangement is bound to the key that signed it: a wiped-and-re-initialised controller (a thief's
     * fresh wallet) can neither replace nor clear it. */
    if (c->ih.inh.armed && memcmp(c->ih.inh.signer, req->signer, 32) != 0) return deny(CONSENT_DENY_POLICY, why);
    /* Arming is a long touch; replacing an existing arrangement (balances drifted) is a plain touch. */
    start_pending(c, CONSENT_OP_INHERITANCE_SET, id, c->ih.inh.armed ? CONSENT_NEED_TOUCH : CONSENT_NEED_LONG_TOUCH,
                  0, now_ms);
    gate_on_display(c, c->ih.inh.armed ? CONSENT_NEED_TOUCH : CONSENT_NEED_LONG_TOUCH);
    c->pending.inh = *req;
    c->pending.inh.armed = true;
    c->pending.inh.warned = 0;
    return CONSENT_R_PENDING;
}

consent_result_t consent_begin_inheritance_clear(consent_t *c, const char *id, const uint8_t owner[32],
                                                 uint32_t now_ms, consent_reason_t *why)
{
    if (!c->p.initialized) return deny(CONSENT_DENY_NOT_INITIALIZED, why);
    if (!c->p.pin_set) return deny(CONSENT_DENY_PIN_NOT_SET, why);
    if (!gate(c, why, false)) return CONSENT_R_DENIED;
    if (!c->ih.inh.armed) return deny(CONSENT_DENY_NOT_ARMED, why);
    if (memcmp(c->ih.inh.signer, owner, 32) != 0) return deny(CONSENT_DENY_POLICY, why);
    return start_pending(c, CONSENT_OP_INHERITANCE_CLEAR, id, CONSENT_NEED_LONG_TOUCH, 0, now_ms);
}

static bool cancel_pending(consent_t *c, consent_action_t *cancelled)
{
    if (c->pending.op == CONSENT_OP_NONE) return false;
    if (cancelled) {
        memset(cancelled, 0, sizeof *cancelled);
        cancelled->op = c->pending.op;
        copy_id(cancelled->id, c->pending.id);
    }
    clear_pending(c);
    return true;
}

bool consent_cancel_pending(consent_t *c, consent_action_t *cancelled) { return cancel_pending(c, cancelled); }

bool consent_lock(consent_t *c, consent_action_t *cancelled)
{
    bool had = cancel_pending(c, cancelled);
    c->locked = true;
    clear_elevate(c);
    clear_allowance(c);
    return had;
}

bool consent_duress(consent_t *c, consent_action_t *cancelled)
{
    bool had = cancel_pending(c, cancelled);
    if (!c->p.duress) c->dirty = true;
    c->p.duress = true;
    clear_elevate(c);
    clear_allowance(c);
    return had;
}

bool consent_allowance_revoke(consent_t *c)
{
    bool had = c->p.allowance.active;
    clear_allowance(c);
    return had;
}

/* Applies the state change of an approved pending op and fills the action. */
static void complete_pending(consent_t *c, uint32_t now_ms, consent_action_t *out)
{
    consent_op_t op = c->pending.op;
    memset(out, 0, sizeof *out);
    out->op = op;
    copy_id(out->id, c->pending.id);
    switch (op) {
    case CONSENT_OP_SIGN:
        out->slot = consent_active_slot(c);
        out->chain = c->pending.chain;
        out->lamports = c->pending.lamports;
        out->wei = c->pending.wei;
        break;
    case CONSENT_OP_ELEVATE:
        c->elevate.active = true;
        c->elevate.chain = c->pending.chain;
        c->elevate.limit = c->pending.limit;
        c->elevate.limit_wei = c->pending.limit_wei;
        c->elevate.until_ms = now_ms + CONSENT_ELEVATE_WINDOW_MS;
        break;
    case CONSENT_OP_UNLOCK:
        c->locked = false;
        if (c->p.duress) c->dirty = true;
        c->p.duress = false;
        break;
    case CONSENT_OP_POLICY:
        if (c->pending.policy.has_sol) c->p.daily_limit = c->pending.policy.sol;
        if (c->pending.policy.has_wei) c->p.daily_limit_wei = c->pending.policy.wei;
        if (c->pending.policy.has_sats) c->p.daily_limit_sats = c->pending.policy.sats;
        if (c->pending.policy.has_router_add) routers_add(&c->p.routers, c->pending.policy.router.chain, c->pending.policy.router.addr);
        if (c->pending.policy.has_router_remove) routers_remove(&c->p.routers, c->pending.policy.router.chain, c->pending.policy.router.addr);
        out->policy = c->pending.policy;
        c->dirty = true;
        break;
    case CONSENT_OP_ALLOWANCE:
        c->p.allowance = c->pending.allowance;
        c->dirty = true;
        break;
    case CONSENT_OP_SET_PIN:
    case CONSENT_OP_SET_DURESS_PIN:
        memcpy(out->pin, c->pending.pin, CONSENT_PIN_CODE_LEN);
        break;
    case CONSENT_OP_INHERITANCE_SET:
        out->slot = CONSENT_SLOT_REAL; /* gate() refused duress, so this is always the real key */
        out->inh = c->pending.inh;
        break;
    case CONSENT_OP_INHERITANCE_CLEAR:
        memset(&c->ih.inh, 0, sizeof c->ih.inh);
        c->ih_dirty = true;
        break;
    case CONSENT_OP_INIT:
    case CONSENT_OP_INIT_WORDS:
    default:
        break;
    }
    clear_pending(c);
}

consent_touch_result_t consent_touch_ex(consent_t *c, bool long_touch, uint32_t now_ms, consent_action_t *out)
{
    mark_touch(c); /* every touch is proof of life, approved or not */
    if (c->pending.op == CONSENT_OP_NONE) return CONSENT_TOUCH_NONE;
    if (time_reached(now_ms, c->pending.deadline_ms)) return CONSENT_TOUCH_NONE; /* tick() will report the timeout */
    if (c->pending.need == CONSENT_NEED_PIN) return CONSENT_TOUCH_NONE;          /* taps are rhythm input */
    if (c->pending.need == CONSENT_NEED_DISPLAY) return CONSENT_TOUCH_NONE;      /* nothing to consent to yet */
    if (c->pending.display_gated) {
        if (long_touch && c->pending.need == CONSENT_NEED_TOUCH) {
            /* hold 3 s = cancel, on any page */
            memset(out, 0, sizeof *out);
            out->op = c->pending.op;
            copy_id(out->id, c->pending.id);
            clear_pending(c);
            return CONSENT_TOUCH_CANCELLED;
        }
        if (!c->pending.final_shown) {
            if (long_touch) return CONSENT_TOUCH_NONE; /* long-touch request, not on the last page yet */
            memset(out, 0, sizeof *out);
            out->op = c->pending.op;
            copy_id(out->id, c->pending.id);
            out->page = (uint8_t)(c->pending.page + 1);
            return CONSENT_TOUCH_PAGE;
        }
    }
    if (c->pending.need == CONSENT_NEED_LONG_TOUCH && !long_touch) return CONSENT_TOUCH_NONE;
    complete_pending(c, now_ms, out);
    return CONSENT_TOUCH_APPROVED;
}

bool consent_touch(consent_t *c, bool long_touch, uint32_t now_ms, consent_action_t *out)
{
    return consent_touch_ex(c, long_touch, now_ms, out) == CONSENT_TOUCH_APPROVED;
}

bool consent_display_shown(consent_t *c, uint8_t page, uint8_t n_pages)
{
    if (c->pending.op == CONSENT_OP_NONE || !c->pending.display_gated) return false;
    if (n_pages == 0 || page >= n_pages) return false;
    c->pending.n_pages = n_pages;
    c->pending.page = page;
    if (c->pending.need == CONSENT_NEED_DISPLAY) c->pending.need = c->pending.approve_need;
    if (page + 1u == n_pages) c->pending.final_shown = true;
    return true;
}

bool consent_pending_display_gated(const consent_t *c) { return c->pending.op != CONSENT_OP_NONE && c->pending.display_gated; }
uint8_t consent_pending_page(const consent_t *c) { return c->pending.op != CONSENT_OP_NONE ? c->pending.page : 0; }
uint8_t consent_pending_pages(const consent_t *c) { return c->pending.op != CONSENT_OP_NONE ? c->pending.n_pages : 0; }

consent_rhythm_result_t consent_rhythm(consent_t *c, const uint8_t code[CONSENT_PIN_CODE_LEN], uint32_t now_ms,
                                       consent_action_t *out)
{
    if (c->pending.op == CONSENT_OP_NONE || c->pending.need != CONSENT_NEED_PIN) return CONSENT_RHYTHM_NONE;
    if (time_reached(now_ms, c->pending.deadline_ms)) return CONSENT_RHYTHM_NONE;
    if (c->pending.op == CONSENT_OP_SET_PIN) {
        if (c->pending.pin_stage == 0) {
            memcpy(c->pending.pin, code, CONSENT_PIN_CODE_LEN);
            c->pending.pin_stage = 1;
            c->pending.deadline_ms = now_ms + CONSENT_DEFAULT_TTL_MS; /* fresh window for the confirmation */
            return CONSENT_RHYTHM_CONFIRM;
        }
        if (ct_memcmp(c->pending.pin, code, CONSENT_PIN_CODE_LEN) != 0) {
            memset(out, 0, sizeof *out);
            out->op = CONSENT_OP_SET_PIN;
            copy_id(out->id, c->pending.id);
            clear_pending(c);
            return CONSENT_RHYTHM_MISMATCH;
        }
        complete_pending(c, now_ms, out);
        return CONSENT_RHYTHM_SET;
    }
    if (c->pending.op == CONSENT_OP_SET_DURESS_PIN && c->pending.pin_stage > 0) {
        if (c->pending.pin_stage == 1) {
            memcpy(c->pending.pin, code, CONSENT_PIN_CODE_LEN);
            c->pending.pin_stage = 2;
            c->pending.deadline_ms = now_ms + CONSENT_DEFAULT_TTL_MS;
            return CONSENT_RHYTHM_CONFIRM;
        }
        /* confirmation must repeat the first entry, and the pair must differ from the real rhythm */
        if (ct_memcmp(c->pending.pin, code, CONSENT_PIN_CODE_LEN) != 0 ||
            (c->pending.have_pin_real && ct_memcmp(c->pending.pin_real, code, CONSENT_PIN_CODE_LEN) == 0)) {
            memset(out, 0, sizeof *out);
            out->op = CONSENT_OP_SET_DURESS_PIN;
            copy_id(out->id, c->pending.id);
            clear_pending(c);
            return CONSENT_RHYTHM_MISMATCH;
        }
        complete_pending(c, now_ms, out);
        return CONSENT_RHYTHM_SET;
    }
    /* unlock / policy / decoy_addresses / set_duress_pin stage 0: hand the code out, keep the request pending
     * until the verdict */
    if (c->pending.op == CONSENT_OP_SET_DURESS_PIN) {
        memcpy(c->pending.pin_real, code, CONSENT_PIN_CODE_LEN); /* kept only if the signer says it is the real one */
        c->pending.have_pin_real = false;
    }
    memset(out, 0, sizeof *out);
    out->op = c->pending.op;
    copy_id(out->id, c->pending.id);
    memcpy(out->pin, code, CONSENT_PIN_CODE_LEN);
    return CONSENT_RHYTHM_VERIFY;
}

consent_pin_result_t consent_pin_verdict(consent_t *c, consent_verdict_t v, uint32_t now_ms, consent_action_t *out)
{
    if (c->pending.op == CONSENT_OP_NONE || c->pending.need != CONSENT_NEED_PIN) return CONSENT_PIN_NONE;
    if (v == CONSENT_VERDICT_DURESS && c->pending.op == CONSENT_OP_UNLOCK) {
        /* Looks exactly like a real unlock from the outside; inside, the decoy slot is active from now on.
         * The failure counter is left alone: this was a correct rhythm. */
        memset(out, 0, sizeof *out);
        out->op = CONSENT_OP_UNLOCK;
        copy_id(out->id, c->pending.id);
        clear_pending(c);
        c->locked = false;
        if (!c->p.duress) c->dirty = true;
        c->p.duress = true;
        clear_elevate(c);
        clear_allowance(c);
        return CONSENT_PIN_OK;
    }
    if (v == CONSENT_VERDICT_REAL) {
        if (c->p.pin_failures) {
            c->p.pin_failures = 0;
            c->dirty = true;
        }
        if (c->pending.op == CONSENT_OP_SET_DURESS_PIN && c->pending.pin_stage == 0) {
            /* the real rhythm is proven: remember it for the "must differ" rule and ask for the new one */
            c->pending.have_pin_real = true;
            c->pending.pin_stage = 1;
            c->pending.deadline_ms = now_ms + CONSENT_DEFAULT_TTL_MS;
            memset(out, 0, sizeof *out);
            out->op = c->pending.op;
            copy_id(out->id, c->pending.id);
            return CONSENT_PIN_CONTINUE;
        }
        complete_pending(c, now_ms, out);
        return CONSENT_PIN_OK;
    }
    /* wrong, or the duress rhythm used for anything but an unlock: a failure like any other */
    memset(out, 0, sizeof *out);
    out->op = c->pending.op;
    copy_id(out->id, c->pending.id);
    clear_pending(c);
    if (c->p.pin_failures < 255) c->p.pin_failures++;
    c->dirty = true;
    if (c->p.pin_failures >= CONSENT_PIN_MAX_FAILURES) {
        consent_wipe(c);
        return CONSENT_PIN_WIPE;
    }
    return CONSENT_PIN_WRONG;
}

consent_pin_result_t consent_pin_result(consent_t *c, bool verified, uint32_t now_ms, consent_action_t *out)
{
    return consent_pin_verdict(c, verified ? CONSENT_VERDICT_REAL : CONSENT_VERDICT_WRONG, now_ms, out);
}

void consent_commit_spend(consent_t *c, const consent_action_t *a)
{
    if (a->op != CONSENT_OP_SIGN) return;
    switch (a->chain) {
    case CONSENT_CHAIN_EVM: u256_add_sat(&c->p.spent_today_wei, &c->p.spent_today_wei, &a->wei); break;
    case CONSENT_CHAIN_BTC: c->p.spent_today_sats = u64_sat_add(c->p.spent_today_sats, a->lamports); break;
    default: c->p.spent_today = u64_sat_add(c->p.spent_today, a->lamports); break;
    }
    if (a->via_allowance && c->p.allowance.active)
        c->p.allowance.spent = u64_sat_add(c->p.allowance.spent, a->lamports);
    else if (c->elevate.active && c->elevate.chain == a->chain)
        clear_elevate(c); /* an elevate covers exactly one touched request on its chain */
    c->dirty = true;
}

void consent_mark_initialized(consent_t *c)
{
    c->p.initialized = true;
    c->dirty = true;
}

void consent_mark_pin_set(consent_t *c)
{
    c->p.pin_set = true;
    c->p.pin_failures = 0;
    c->dirty = true;
}

void consent_mark_duress_pin_set(consent_t *c)
{
    c->p.duress_pin_set = true;
    c->dirty = true;
}

void consent_wipe(consent_t *c)
{
    memset(&c->p, 0, sizeof c->p);
    clear_pending(c);
    clear_elevate(c);
    c->locked = false; /* a fresh device: nothing to protect, init allowed again */
    c->dirty = true;
    /* c->ih is untouched: the inheritance outlives the wipe (that is the point of it). */
}

void consent_commit_inheritance(consent_t *c, const consent_action_t *a)
{
    if (a->op != CONSENT_OP_INHERITANCE_SET) return;
    c->ih.inh = a->inh;
    c->ih.inh.armed = true;
    c->ih.inh.warned = 0;
    c->ih.last_touch_powered_seconds = c->ih.powered_seconds; /* the approving touch starts the clock */
    c->ih_dirty = true;
}

uint8_t consent_active_slot(const consent_t *c) { return c->p.duress ? CONSENT_SLOT_DECOY : CONSENT_SLOT_REAL; }

consent_need_t consent_pending_need(const consent_t *c)
{
    return c->pending.op == CONSENT_OP_NONE ? CONSENT_NEED_NONE : c->pending.need;
}

const char *consent_reason_str(consent_reason_t r)
{
    switch (r) {
    case CONSENT_DENY_TIMEOUT: return "timeout";
    case CONSENT_DENY_CLAIMS_MISMATCH: return "claims_mismatch";
    case CONSENT_DENY_OVER_DAILY_LIMIT: return "over_daily_limit";
    case CONSENT_DENY_LOCKED: return "locked";
    case CONSENT_DENY_POLICY: return "policy";
    case CONSENT_DENY_USER: return "user";
    case CONSENT_DENY_BUSY: return "busy";
    case CONSENT_DENY_NOT_INITIALIZED: return "not_initialized";
    case CONSENT_DENY_PIN_NOT_SET: return "pin_not_set";
    case CONSENT_DENY_WRONG_PIN: return "wrong_pin";
    case CONSENT_DENY_PIN_MISMATCH: return "pin_mismatch";
    case CONSENT_DENY_WIPED: return "wiped";
    case CONSENT_DENY_NOT_ARMED: return "not_armed";
    case CONSENT_DENY_NOT_EXPIRED: return "not_expired";
    case CONSENT_DENY_NOT_SUPPORTED: return "not_supported";
    case CONSENT_DENY_NONE:
    default: return "policy";
    }
}

const char *consent_need_str(consent_need_t n)
{
    switch (n) {
    case CONSENT_NEED_LONG_TOUCH: return "long_touch";
    case CONSENT_NEED_PIN: return "pin";
    case CONSENT_NEED_DISPLAY: return "display";
    default: return "touch";
    }
}
