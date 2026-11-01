#pragma once
#include "weather_data.h"
#include "air_quality.h"

/* One optional cue, never an official warning or a safety assessment. */
typedef enum {
    WEATHER_HINT_NONE,
    WEATHER_HINT_WIND,
    WEATHER_HINT_GUST,
    WEATHER_HINT_RAIN_STRONG,
    WEATHER_HINT_FREEZING_RAIN,
    WEATHER_HINT_SNOW_STRONG,
    WEATHER_HINT_UV,
    WEATHER_HINT_PRECIPITATION,
    WEATHER_HINT_UNKNOWN,
    WEATHER_HINT_AIR_POOR,
    WEATHER_HINT_THUNDER, /* Appended: preserve the original kind IDs. */
    WEATHER_HINT_HAIL
} weather_hint_kind_t;

typedef struct {
    weather_hint_kind_t kind;
    int grade; /* WIND/GUST only; otherwise -1. */
    bool probability_present; /* PRECIPITATION: false means unknown, not 0%. */
    float probability_pct; /* Meaningful only when probability_present. */
} weather_hint_t;

/* Current weather and air quality have independent validity. Neither daily
 * maxima nor an expired weather group establish current conditions. */
weather_hint_t weather_hint_current(const weather_current_t *current,
    const air_quality_current_t *air_quality, int64_t utc, uint64_t ms, bool trusted);
/* Caller selects the location's date slot using weather_day_at/forecast_summary.
 * Invalid daily data yields UNKNOWN. Ordinary wind fallback never means gust. */
weather_hint_t weather_hint_day(const weather_day_t *day,
    int64_t utc, uint64_t ms, bool trusted);
