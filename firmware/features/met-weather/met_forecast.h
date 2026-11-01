#pragma once
#include "weather_data.h"

#define MET_FORECAST_POINTS 24u
/* Today plus ten dates: bounded retained calendar coverage for 304 reuse. */
#define MET_FORECAST_DAYS 11u
#define MET_OFFSET_UNKNOWN INT32_MAX
#define MET_FORECAST_BODY_LIMIT (128u * 1024u)
#define MET_FORECAST_OBJECT_LIMIT 4096u

typedef struct {
    int64_t source_utc;
    int code;
    float temperature_c, wind_mps;
    bool wind_present;
} met_point_t;

typedef struct {
    int64_t model_updated_utc;
    size_t count;
    met_point_t points[MET_FORECAST_POINTS];
    int offset_s;
    int64_t series_first_utc, series_last_utc;
    int64_t days_begin_utc, days_end_utc; /* Retained [begin,end), not a completeness claim. */
    weather_day_t days[MET_FORECAST_DAYS]; /* Fetched times attached only by selection. */
} met_forecast_t;

/* Complete bounded JSON, parsed atomically without a whole-document cJSON tree.
 * Keeps current hourly candidates and fully covered future reference dates.
 * Explicit fixed offset; MET_OFFSET_UNKNOWN retains current data only.
 * WX_OK means complete valid JSON with usable current OR daily candidates. */
weather_error_t met_forecast_parse(const char *json, size_t length, int64_t utc,
                                 int offset_s, met_forecast_t *out);
/* No source/model time is invented on a cache hit or 304. The caller supplies
 * the actual successful fetch/revalidation times and attaches provenance. */
bool met_forecast_select(const met_forecast_t *forecast, int64_t utc,
                         int64_t fetched_utc, uint64_t fetched_ms,
                         weather_current_t *out);
/* Clears all seven outputs, fills their local dates when possible, and returns
 * usable count. Cached full future dates may become today's reference on 304.
 * No probability, gust or actual UV is manufactured. Date/offset/cache window
 * coverage and actual requested group needs remain service responsibilities. */
unsigned met_forecast_select_days(const met_forecast_t *forecast, int64_t utc,
    int64_t fetched_utc, uint64_t fetched_ms, weather_day_t out[WEATHER_DAYS]);
