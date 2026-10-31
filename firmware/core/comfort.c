#include "comfort.h"
#include <math.h>

#if defined(__FAST_MATH__) || (defined(__FINITE_MATH_ONLY__) && __FINITE_MATH_ONLY__)
#error "Comfort validity checks require non-finite floating-point support"
#endif

void comfort_reset(comfort_context_t *context) {
    if (context) *context = (comfort_context_t){0};
}

static comfort_validity_t validity(comfort_sample_t sample, uint64_t now,
                                   float minimum, float maximum) {
    if (!sample.source_valid) return COMFORT_UNAVAILABLE;
    if (!isfinite(sample.value)) return COMFORT_NONFINITE;
    if (sample.value < minimum || sample.value > maximum)
        return COMFORT_OUT_OF_RANGE;
    if (sample.acquired_ms > now) return COMFORT_FUTURE;
    if (now - sample.acquired_ms > COMFORT_FRESH_MS) return COMFORT_STALE;
    return COMFORT_VALUE_OK;
}

static comfort_band_t band(float value, comfort_band_t previous,
                           float low, float high, float hysteresis) {
    /* Opposite-side crossings take precedence over holding the old band. */
    if (value < low) return COMFORT_BAND_LOW;
    if (value > high) return COMFORT_BAND_HIGH;
    if (previous == COMFORT_BAND_LOW && value < low + hysteresis)
        return COMFORT_BAND_LOW;
    if (previous == COMFORT_BAND_HIGH && value > high - hysteresis)
        return COMFORT_BAND_HIGH;
    return COMFORT_BAND_OK;
}

comfort_result_t comfort_step(comfort_context_t *context,
                             comfort_sample_t temperature,
                             comfort_sample_t humidity, uint64_t now_ms) {
    comfort_result_t result = {
        .state = COMFORT_PENDING,
        .temperature_validity = COMFORT_UNAVAILABLE,
        .humidity_validity = COMFORT_UNAVAILABLE
    };
    if (!context) return result;

    if (context->has_step &&
        (now_ms < context->last_step_ms ||
         now_ms - context->last_step_ms > COMFORT_FRESH_MS))
        comfort_reset(context);
    context->has_step = true;
    context->last_step_ms = now_ms;

    result.temperature_validity = validity(temperature, now_ms,
        COMFORT_TEMP_MIN_C, COMFORT_TEMP_MAX_C);
    result.humidity_validity = validity(humidity, now_ms,
        COMFORT_RH_MIN_PCT, COMFORT_RH_MAX_PCT);
    context->temperature = result.temperature_validity == COMFORT_VALUE_OK
        ? band(temperature.value, context->temperature, COMFORT_TEMP_LOW_C,
               COMFORT_TEMP_HIGH_C, COMFORT_TEMP_HYST_C)
        : COMFORT_BAND_UNKNOWN;
    context->humidity = result.humidity_validity == COMFORT_VALUE_OK
        ? band(humidity.value, context->humidity, COMFORT_RH_LOW_PCT,
               COMFORT_RH_HIGH_PCT, COMFORT_RH_HYST_PCT)
        : COMFORT_BAND_UNKNOWN;

    if (context->temperature == COMFORT_BAND_LOW)
        result.reasons[result.reason_count++] = COMFORT_REASON_COLD;
    else if (context->temperature == COMFORT_BAND_HIGH)
        result.reasons[result.reason_count++] = COMFORT_REASON_HOT;
    if (context->humidity == COMFORT_BAND_LOW)
        result.reasons[result.reason_count++] = COMFORT_REASON_DRY;
    else if (context->humidity == COMFORT_BAND_HIGH)
        result.reasons[result.reason_count++] = COMFORT_REASON_WET;

    if (result.temperature_validity != COMFORT_VALUE_OK ||
        result.humidity_validity != COMFORT_VALUE_OK)
        return result;
    if (context->temperature == COMFORT_BAND_HIGH) result.state = COMFORT_HOT;
    else if (context->temperature == COMFORT_BAND_LOW) result.state = COMFORT_COLD;
    else if (context->humidity == COMFORT_BAND_LOW) result.state = COMFORT_DRY;
    else if (context->humidity == COMFORT_BAND_HIGH) result.state = COMFORT_WET;
    else result.state = COMFORT_OK;
    return result;
}
