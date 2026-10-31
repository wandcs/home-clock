#ifndef HOME_CLOCK_SOLAR_CORE_H
#define HOME_CLOCK_SOLAR_CORE_H
#include <stdbool.h>
#include <stdint.h>

/* Unix UTC milliseconds, proleptic Gregorian civil dates 1901..2099.
 * East/north positive. No libc time zone, allocation, IO or device access. */
#define SOLAR_MAX_EVENTS 4
typedef enum { SOLAR_UNKNOWN, SOLAR_DAY, SOLAR_NIGHT } solar_phase_t;
typedef enum {
    SOLAR_UNAVAILABLE, SOLAR_NORMAL, SOLAR_CONTINUOUS_DAY,
    SOLAR_CONTINUOUS_NIGHT, SOLAR_INDETERMINATE
} solar_status_t;
typedef enum {
    SOLAR_OK, SOLAR_BAD_INPUT, SOLAR_UNTRUSTED_TIME,
    SOLAR_STALE_WINDOW, SOLAR_NUMERIC_UNCERTAIN
} solar_reason_t;
typedef enum { SOLAR_SUNRISE, SOLAR_SUNSET } solar_event_type_t;
typedef struct { int year, month, day; } solar_date_t;
/* Offsets apply at the two civil midnights separately. Caller supplies rules;
 * this does not certify an IANA zone. Never reuse one API offset as DST data. */
typedef struct {
    solar_date_t date;
    int start_offset_minutes, end_offset_minutes;
} solar_civil_day_t;
typedef struct { solar_event_type_t type; int64_t utc_ms; } solar_event_t;
typedef struct {
    solar_status_t status;
    solar_reason_t reason;
    int64_t start_utc_ms, end_utc_ms; /* half-open validity interval */
    solar_phase_t start_phase;
    unsigned count;
    solar_event_t events[SOLAR_MAX_EVENTS];
} solar_day_t;
typedef struct {
    double latitude, longitude;
    solar_civil_day_t civil;
    int64_t now_utc_ms;
    bool trusted;
    uint32_t location_revision, clock_revision;
} solar_input_t;
typedef struct {
    solar_phase_t phase;
    solar_reason_t reason;
    solar_status_t status;
    bool has_next_event, has_next_check;
    solar_event_t next_event;
    int64_t next_check_utc_ms;
} solar_state_t;
typedef struct {
    bool cached;
    solar_input_t key;
    solar_day_t day;
    uint64_t calculations;
} solar_model_t;

bool solar_civil_bounds(solar_civil_day_t civil, int64_t *start, int64_t *end);
/* Convenience only for a caller-established fixed offset (e.g. Beijing +480).
 * Does not implement IANA/DST; changing-offset callers supply civil explicitly. */
bool solar_fixed_civil(int64_t now_utc_ms, int offset_minutes, solar_civil_day_t *out);
bool solar_calculate(double latitude, double longitude, solar_civil_day_t civil,
                     solar_day_t *out);
solar_state_t solar_evaluate(const solar_day_t *day, int64_t now_utc_ms, bool trusted);
/* Initialize with {0}. update reuses only location/civil/revision matching data;
 * trust loss, invalid/stale inputs discard cache, backwards time recomputes.
 * Call after every clock sync (increment clock_revision, even small corrections)
 * and on wake/resume; pass CURRENT civil day, including new midnight offsets. */
solar_state_t solar_update(solar_model_t *model, const solar_input_t *input);
#endif
