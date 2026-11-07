#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Exactly one active request under the existing shared HTTP budget. */
typedef enum {
    WHG_OWNER_NONE, WHG_OWNER_PRIMARY, WHG_OWNER_MET, WHG_OWNER_AIR,
    WHG_OWNER_CALENDAR
} weather_http_owner_t;
typedef enum {
    WHG_IDLE, WHG_DNS, WHG_CONNECT, WHG_HEADERS, WHG_BODY, WHG_CLEANUP, WHG_DONE
} weather_http_phase_t;
typedef struct {
    uint32_t attempt_id, generation;
    weather_http_owner_t owner;
    weather_http_phase_t phase;
    uint64_t started_ms, progress_ms, deadline_ms, finished_ms;
    uint32_t socket_aborts, dns_resets, stale_callbacks;
    uint32_t guard_task_stack_min_bytes;
    bool active, fd_present, interrupted, recovery_requested, dns_requested;
} weather_http_progress_t;
bool weather_http_guard_start(void);
uint32_t weather_http_guard_begin(weather_http_owner_t owner, uint32_t generation,
                                  uint32_t budget_ms);
bool weather_http_guard_dns(uint32_t attempt, const char *host, int *error);
void weather_http_guard_phase(uint32_t attempt, weather_http_phase_t phase);
void weather_http_guard_connected(uint32_t attempt, int fd);
void weather_http_guard_detach(uint32_t attempt);
void weather_http_guard_end(uint32_t attempt);
bool weather_http_guard_expired(uint32_t attempt);
/* Only a short metadata critical section: never a lifetime/DNS mutex or I/O. */
void weather_http_guard_request_recovery(void);
weather_http_progress_t weather_http_guard_read(void);
const char *weather_http_owner_name(weather_http_owner_t owner);
const char *weather_http_phase_name(weather_http_phase_t phase);
