#pragma once
#include "weather_data.h"

#define MET_FORECAST_POINTS 24u
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
} met_forecast_t;

/* Complete bounded JSON, parsed atomically without a whole-document cJSON tree.
 * Retains only one-hour forecasts from the current interval through 24 hours. */
weather_error_t met_forecast_parse(const char *json, size_t length, int64_t utc,
                                 met_forecast_t *out);
/* No source/model time is invented on a cache hit or 304. The caller supplies
 * the actual successful fetch/revalidation times and attaches provenance. */
bool met_forecast_select(const met_forecast_t *forecast, int64_t utc,
                         int64_t fetched_utc, uint64_t fetched_ms,
                         weather_current_t *out);
