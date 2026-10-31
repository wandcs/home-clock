#ifndef ALARM_FOUNDATION_H
#define ALARM_FOUNDATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AF_YEAR_BYTES 136u
#define AF_BITMAP_BYTES 46u
typedef struct { uint16_t year; uint8_t off[46], work[46]; } af_year;
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second; } af_time;
typedef enum { AF_ONCE = 0, AF_WORKDAY = 1, AF_DAILY = 2 } af_mode;
typedef struct {
    bool enabled;
    bool once_pending; /* External lifecycle: false after this occurrence is consumed. */
    uint8_t hour, minute;
    af_mode mode;
} af_alarm;
typedef enum {
    AF_OK = 0, AF_WAIT_TIME, AF_DISABLED, AF_CONSUMED,
    AF_INVALID, AF_RANGE_END, AF_NO_CANDIDATE
} af_status;
typedef struct {
    af_status status;
    af_time at; /* meaningful only on AF_OK */
    bool calendar_fallback; /* missing year encountered during WORKDAY search */
} af_candidate;

/* No heap, system clock, timezone, networking, persistence or audio. */
bool af_time_valid(const af_time *t);
bool af_day_index(const af_time *t, uint16_t *index);
bool af_year_decode(const uint8_t *bytes, size_t size, af_year *out);
bool af_year_valid(const af_year *year);
/* Invalid catalogs/dates return false without changing either output. */
bool af_is_workday(const af_time *date, const af_year *years, size_t count,
                   bool *workday, bool *fallback);
/* include_reference applies to exact second, never the whole reference minute.
 * At most today + 366 dates. Does not choose a real triggering policy. */
af_candidate af_next(const af_alarm *alarm, const af_time *reference,
                     bool trusted, bool include_reference,
                     const af_year *years, size_t count);
#endif
