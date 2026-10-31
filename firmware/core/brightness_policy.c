#include "brightness_policy.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

int32_t bp_curve(float lux) {
    static const struct { float lux; int32_t mp; } points[] = {
        {0, BP_MIN_MP}, {3, 4000}, {10, 8000}, {50, 20000},
        {200, 40000}, {500, 65000}, {1000, BP_MAX_MP}
    };
    if (!isfinite(lux) || lux < 0) return -1;
    for (size_t i = 1; i < sizeof points / sizeof points[0]; ++i) {
        if (lux <= points[i].lux) {
            float fraction = (lux - points[i-1].lux) / (points[i].lux - points[i-1].lux);
            return points[i-1].mp + (int32_t)(fraction * (points[i].mp - points[i-1].mp) + 0.5f);
        }
    }
    return BP_MAX_MP;
}

void bp_init(bp_state_t *s, uint64_t now, unsigned percent) {
    memset(s, 0, sizeof *s);
    if (percent < BP_MIN_MP / 1000u) percent = BP_MIN_MP / 1000u;
    if (percent > BP_MAX_MP / 1000u) percent = BP_MAX_MP / 1000u;
    s->started_ms = s->last_tick_ms = now;
    s->target_mp = s->demand_mp = (int32_t)percent * 1000;
    s->low_candidate = -1;
    s->mode = BP_WAITING;
}

static void reset_pending(bp_state_t *s) {
    s->demand_direction = 0;
    s->low_candidate = -1;
}

static void accept_sample(bp_state_t *s, bp_input_t in) {
    int32_t delta = bp_curve(in.lux) - s->demand_mp;
    int direction = delta > BP_DEADBAND_MP ? 1 : delta < -BP_DEADBAND_MP ? -1 : 0;
    if (direction != s->demand_direction) {
        s->demand_direction = (int8_t)direction;
        s->demand_since_ms = in.acquired_ms;
    }
    if (direction && in.acquired_ms - s->demand_since_ms >=
        (direction > 0 ? BP_UP_CONFIRM_MS : BP_DOWN_CONFIRM_MS)) {
        s->demand_mp = bp_curve(in.lux);
        s->demand_direction = 0;
    }

    int candidate = -1;
    if (in.lux <= BP_LOW_ENTER_LUX) candidate = 1;
    else if (in.lux >= BP_LOW_EXIT_LUX || !s->low_light) candidate = 0;
    if (s->low_known && candidate == (int)s->low_light) candidate = -1;
    if (candidate != s->low_candidate) {
        s->low_candidate = (int8_t)candidate;
        s->low_since_ms = in.acquired_ms;
    }
    if (candidate >= 0 && in.acquired_ms - s->low_since_ms >=
        (candidate ? BP_LOW_ENTER_MS : BP_LOW_EXIT_MS)) {
        s->low_light = candidate != 0;
        s->low_known = true;
        s->low_candidate = -1;
    }
}

bool bp_step(bp_state_t *s, bp_input_t in, uint64_t now, bp_output_t *out) {
    if (now < s->last_tick_ms) return false;
    uint64_t dt = now - s->last_tick_ms;
    if (dt > BP_TICK_CAP_MS) dt = BP_TICK_CAP_MS;
    s->last_tick_ms = now;
    bool valid = in.valid && isfinite(in.lux) && in.lux >= 0 &&
        in.acquired_ms <= now && now - in.acquired_ms <= BP_FRESH_MS &&
        (!s->have_sample || in.acquired_ms >= s->sample_ms);
    if (valid) {
        if (!s->have_sample || in.acquired_ms > s->sample_ms) {
            if (s->have_sample && in.acquired_ms - s->sample_ms > BP_FRESH_MS) {
                reset_pending(s);
                s->low_known = false;
            }
            accept_sample(s, in);
            s->sample_ms = in.acquired_ms;
            s->have_sample = true;
        }
        s->mode = BP_TRACKING;
    } else {
        reset_pending(s);
        s->low_known = false;
        s->demand_mp = s->target_mp;
        uint64_t reference = s->have_sample ? s->sample_ms : s->started_ms;
        if (now - reference >= BP_FAULT_MS) {
            s->mode = BP_FALLBACK;
            if (s->demand_mp > BP_FALLBACK_MP) s->demand_mp = BP_FALLBACK_MP;
        } else s->mode = s->have_sample ? BP_HOLD : BP_WAITING;
    }
    int32_t distance = s->demand_mp - s->target_mp;
    int32_t limit = (int32_t)dt * (distance > 0 ? BP_UP_MP_PER_MS : BP_DOWN_MP_PER_MS);
    if (distance > limit) distance = limit;
    if (distance < -limit) distance = -limit;
    s->target_mp += distance;
    *out = (bp_output_t){.target_millipercent = s->target_mp,
        .demand_millipercent = s->demand_mp,
        .target_percent = (uint8_t)((s->target_mp + 500) / 1000),
        .low_light = s->low_light, .low_light_known = s->low_known, .mode = s->mode};
    return true;
}
