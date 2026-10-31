#pragma once
#include <stdbool.h>
#include <stdint.h>
/* One monotonic deadline, no catch-up queue. Called only by the weather owner. */
#define WEATHER_REFRESH_MS (30ULL*60*1000)
typedef struct {
    uint32_t generation, flight_generation;
    uint64_t due_ms;
    unsigned failures;
    bool configured, in_flight;
} weather_schedule_t;
void weather_schedule_location(weather_schedule_t *s, bool configured, uint32_t generation, uint64_t now);
bool weather_schedule_claim(weather_schedule_t *s, uint64_t now, bool online, bool trusted);
void weather_schedule_finish(weather_schedule_t *s, uint32_t generation, uint64_t now, bool success);
