#ifndef CLOCK_CALENDAR_RUNTIME_H
#define CLOCK_CALENDAR_RUNTIME_H
#include "calendar_client.h"

/* Caller owns this buffer. Years are approved, independently read-back AFY1
 * cache values sorted ascending; count<=2. Missing applicable year means the
 * existing Monday-Friday fallback, never a fabricated empty holiday year. */
typedef struct {
    af_year years[2];
    uint32_t revision,attempts,successes;
    uint16_t current_year,last_year;
    uint8_t count,blocked_mask;
    bool online,trusted,busy;
    cc_result last_result,adjacent_status;
    int storage_error;
    uint64_t next_due_ms[2]; /* current target and next target, not NVS slots */
} cr_snapshot;
typedef bool (*cr_audio_busy_fn)(void);
/* One dedicated PSRAM-stack worker; one lifetime start. Optional callback
 * defers NEW calendar HTTPS while audio active; not hard cancellation of an
 * existing SDK call. Shares weather_http_take/give with all existing HTTPS.
 * No UI lock/store-worker ownership or returned heap object is required. */
bool calendar_runtime_start(cr_audio_busy_fn audio_busy);
/* Short coherent copy, no allocation/I/O; failure leaves caller unchanged.
 * Alarm owner should take ONCE and use the same years/count for summary and
 * trigger. Do not return this struct by value through LVGL callbacks. */
bool calendar_runtime_copy_into(cr_snapshot *out);
/* Redacted device observation only: years/results/HTTP/counts/resource flags,
 * no JSON body, credential or user location. Does not initiate a refresh. */
void calendar_runtime_status(void);
#endif
