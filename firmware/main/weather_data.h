#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEATHER_BODY_LIMIT 8192
#define WEATHER_DAYS 7
#define WEATHER_ZONE_BYTES 80
#define WEATHER_URL_BYTES 640
/* Display validity limits, not a polling schedule or a provider guarantee. */
#define WEATHER_CURRENT_MAX_AGE 7200
#define WEATHER_DAILY_MAX_AGE 86400
typedef enum { WX_UNKNOWN, WX_CLEAR, WX_CLOUD, WX_OVERCAST, WX_FOG, WX_RAIN, WX_SNOW, WX_THUNDER } weather_kind_t;
typedef enum { WX_PROVIDER_UNKNOWN, WX_PROVIDER_OPEN_METEO, WX_PROVIDER_MET } weather_provider_t;
typedef struct {
    bool present, wind_present;
    int code, interval_s;
    float temperature_c, wind_mps;
    int64_t source_utc, fetched_utc;
    uint64_t fetched_ms;
    weather_provider_t provider;
    int64_t model_updated_utc; /* Optional model issue time, never the valid instant. */
} weather_current_t;
typedef struct {
    bool present;
    char date[11];
    int code;
    float high_c, low_c;
    bool precipitation_present, wind_present, uv_present;
    float precipitation_pct, wind_mps; /* Daily maximum 10m model wind, not gusts. */
    float uv_index; /* Optional daily maximum index; dimensionless, not current UV. */
    int64_t fetched_utc;
    uint64_t fetched_ms;
    weather_provider_t provider;
} weather_day_t;
typedef struct {
    char timezone[64], abbreviation[24];
    int offset_s;
    float grid_latitude, grid_longitude;
    bool grid_present; /* Only the provider's explicit grid, not requested coordinates. */
    weather_current_t current;
    weather_day_t days[WEATHER_DAYS];
} weather_data_t;
typedef enum { WEATHER_SLOT_UNKNOWN, WEATHER_SLOT_MISSING, WEATHER_SLOT_EXPIRED,
    WEATHER_SLOT_USABLE } weather_slot_state_t;
typedef struct {
    char date[11];
    weather_slot_state_t state;
    int data_index; /* Index into the supplied snapshot, or -1. */
} weather_forecast_slot_t;
typedef struct {
    weather_forecast_slot_t slots[WEATHER_DAYS];
    bool time_known, mixed;
    unsigned usable_count, missing_count, expired_count;
    uint64_t oldest_fetched_ms, newest_fetched_ms;
    int64_t oldest_fetched_utc, newest_fetched_utc;
} weather_forecast_t;
typedef enum { WX_OK, WX_JSON, WX_SCHEMA, WX_PARTIAL, WX_HTTP, WX_TRANSPORT,
    WX_TOO_LARGE, WX_TRUNCATED, WX_TIMEOUT, WX_OFFLINE, WX_CLOCK, WX_UNCONFIGURED } weather_error_t;
weather_kind_t weather_kind(int code);
const char *weather_kind_name(int code);
const char *weather_error_name(weather_error_t error);
const char *weather_provider_name(weather_provider_t provider);
bool weather_url(double latitude,double longitude,const char *timezone,char *out,size_t capacity);
/* Whole document validation is atomic; missing current/day groups are independent. */
weather_error_t weather_parse(const char *json, size_t length, int64_t utc, uint64_t ms, weather_data_t *out);
void weather_merge(weather_data_t *cache, const weather_data_t *fresh);
bool weather_current_valid(const weather_current_t *c, int64_t utc, uint64_t ms, bool trusted);
bool weather_day_valid(const weather_day_t *d, int64_t utc, uint64_t ms, bool trusted);
/* Display precision: canonical float half-step ties round upward to 0.1m/s.
 * -1 for invalid/nonfinite/outside engineering range 0..150m/s; else 0..17. */
int weather_wind_grade(float mps);
/* Product's compact "UV strong" cue starts at the international high category.
 * Missing, invalid or negative values never establish a normal/strong claim. */
bool weather_uv_strong(const weather_day_t *day);
/* Derived only; no extra cache or request state. Only usable days contribute
 * to batch/range facts. UTC values correspond to monotonic oldest/newest. */
void weather_forecast_summary(const weather_data_t *data,int64_t utc,uint64_t ms,
    bool trusted,weather_forecast_t *out);
bool weather_local_date(int64_t utc, int offset_s, char date[11]);
/* Visible slot is relative to the location's today, never the array position. */
const weather_day_t *weather_day_at(const weather_data_t *data, int slot, int64_t utc, uint64_t ms, bool trusted, char date[11]);
