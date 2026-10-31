#include "environment_runtime.h"
#include <stddef.h>

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#error "Environment validity requires non-finite floating-point support"
#endif

_Static_assert(BP_FRESH_MS == COMFORT_FRESH_MS, "Sensor freshness differs");

env_status_t env_runtime_init(env_runtime_t *r, uint64_t now, unsigned current) {
    if (!r) return ENV_INVALID_ARGUMENT;
    *r = (env_runtime_t){0};
    bp_init(&r->brightness, now, current);
    comfort_reset(&r->comfort);
    r->initialized = true;
    return ENV_OK;
}

env_status_t env_runtime_step(env_runtime_t *r, const env_snapshot_t *s,
                              uint64_t now, env_output_t *out) {
    if (!r || !s || !out) return ENV_INVALID_ARGUMENT;
    if (!r->initialized) return ENV_NOT_INITIALIZED;
    if (now < r->brightness.last_tick_ms) return ENV_CLOCK_ROLLBACK;

    /* Conversion deliberately does not pre-age samples or filter zero lux.
     * The reviewed cores own range, future/stale and hysteresis policy. */
    bp_input_t light = {.lux=s->lux, .acquired_ms=s->bh_acquired_ms,
                        .valid=s->bh_source_valid};
    comfort_sample_t temperature = {.value=s->temperature_c,
        .acquired_ms=s->sht_acquired_ms, .source_valid=s->sht_source_valid};
    comfort_sample_t humidity = {.value=s->humidity_pct,
        .acquired_ms=s->sht_acquired_ms, .source_valid=s->sht_source_valid};
    /* The precheck ensures bp_step succeeds; keep its result explicit. */
    bp_output_t brightness;
    if (!bp_step(&r->brightness, light, now, &brightness))
        return ENV_CLOCK_ROLLBACK;
    out->brightness = brightness;
    out->comfort = comfort_step(&r->comfort, temperature, humidity, now);
    return ENV_OK;
}
