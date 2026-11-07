#pragma once
#include "weather_data.h"
#include "weather_schedule.h"
#include "air_quality.h"
#include "weather_http_guard.h"
typedef struct {
    bool configured, development_test, fetching, injected;
    double latitude, longitude;
    char requested_timezone[WEATHER_ZONE_BYTES];
    uint32_t generation, attempts, successes, discarded;
    int http_status, transport_error, bytes;
    uint64_t attempted_ms, finished_ms;
    uint64_t next_due_ms;
    unsigned failures;
    uint32_t issued;
    bool manual_pending;
    uint32_t manual_requests;
    uint64_t manual_requested_ms;
    weather_http_progress_t http_progress;
    weather_error_t error;
    int primary_http, backup_http;
    weather_error_t primary_error, backup_error;
    bool backup_configured, backup_attempted, backup_cached, primary_injected;
    unsigned backup_days;
    uint32_t backup_daily_successes;
    air_quality_current_t air_quality;
    uint32_t air_quality_attempts,air_quality_successes;
    int air_quality_http,air_quality_transport,air_quality_bytes;
    weather_error_t air_quality_error;
    bool air_quality_fetching;
    uint64_t air_quality_attempted_ms,air_quality_finished_ms,air_quality_next_due_ms;
    weather_data_t data;
} weather_state_t;
typedef enum {
    WEATHER_REFRESH_ACCEPTED,
    WEATHER_REFRESH_NO_LOCATION,
    WEATHER_REFRESH_NOT_READY,
    WEATHER_REFRESH_THROTTLED
} weather_refresh_result_t;
/* Non-blocking intent only: ACCEPTED may coalesce with an in-flight refresh.
 * It does not mean an HTTP request or weather update has succeeded. */
weather_refresh_result_t weather_request_refresh(void);
void weather_start(void);
/* F3 position owner calls this only after durable readback, or on valid boot load. */
bool weather_set_location(double latitude,double longitude,const char *timezone);
void weather_clear_location(void);
weather_state_t weather_read(void);
void weather_status(void); /* JSON fragment, called inside the existing stdout lock. */
/* Internal-RAM TLS budget: weather, geocoding and the development network probe
 * share one connection slot. Never held under UI/state locks. */
bool weather_http_take(uint32_t wait_ms);
void weather_http_give(void);
/* Only implemented/referenced by the development build, like network_time_command. */
int weather_command(const char *line, uint32_t *rid);
