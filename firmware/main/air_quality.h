#pragma once
#include "weather_data.h"

#define AIR_QUALITY_HOURS 6u
#define AIR_QUALITY_BODY_LIMIT 8192u
#define AIR_QUALITY_URL_BYTES 320u
#define AIR_QUALITY_REFRESH_MS (3ULL * 60 * 60 * 1000)
#define AIR_QUALITY_MAX_AGE (4 * 60 * 60)
/* Fixed source: Open-Meteo CAMS global model; US AQI, not Chinese station AQI.
 * Source time is the UTC start of a one-hour model slot, not fetch/model issue time. */
typedef struct {
    bool present, pm25_present;
    float us_aqi, pm25_ug_m3;
    int64_t source_utc, fetched_utc;
    uint64_t fetched_ms;
} air_quality_current_t;
typedef struct {
    size_t count;
    air_quality_current_t hours[AIR_QUALITY_HOURS];
} air_quality_cache_t;

bool air_quality_url(double latitude,double longitude,char *out,size_t capacity);
weather_error_t air_quality_parse(const char *json,size_t length,int64_t fetched_utc,
    uint64_t fetched_ms,air_quality_cache_t *out);
/* Trusted time, exact source-hour coverage and <=4h UTC/monotonic fetch age. */
bool air_quality_valid(const air_quality_current_t *current,int64_t utc,uint64_t ms,bool trusted);
bool air_quality_poor(const air_quality_current_t *current); /* present/finite US AQI >100 */
bool air_quality_select(const air_quality_cache_t *cache,int64_t utc,uint64_t ms,
    bool trusted,air_quality_current_t *out);
