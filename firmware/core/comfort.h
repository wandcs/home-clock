#ifndef HOME_CLOCK_INDOOR_COMFORT_H
#define HOME_CLOCK_INDOOR_COMFORT_H

#include <stdbool.h>
#include <stdint.h>

/* Project candidates, not a thermal-comfort or health standard. */
#define COMFORT_TEMP_LOW_C 20.0f
#define COMFORT_TEMP_HIGH_C 26.0f
#define COMFORT_TEMP_HYST_C 0.5f
#define COMFORT_RH_LOW_PCT 30.0f
#define COMFORT_RH_HIGH_PCT 60.0f
#define COMFORT_RH_HYST_PCT 3.0f
#define COMFORT_FRESH_MS UINT64_C(6000)
#define COMFORT_TEMP_MIN_C (-40.0f)
#define COMFORT_TEMP_MAX_C 125.0f
#define COMFORT_RH_MIN_PCT 0.0f
#define COMFORT_RH_MAX_PCT 100.0f

typedef enum {
    COMFORT_PENDING, COMFORT_OK, COMFORT_HOT, COMFORT_COLD,
    COMFORT_DRY, COMFORT_WET
} comfort_state_t;

typedef enum {
    COMFORT_REASON_NONE, COMFORT_REASON_HOT, COMFORT_REASON_COLD,
    COMFORT_REASON_DRY, COMFORT_REASON_WET
} comfort_reason_t;

typedef enum {
    COMFORT_VALUE_OK, COMFORT_UNAVAILABLE, COMFORT_NONFINITE,
    COMFORT_OUT_OF_RANGE, COMFORT_FUTURE, COMFORT_STALE
} comfort_validity_t;

typedef struct {
    float value;
    bool source_valid; /* Latest authoritative snapshot: present and no error. */
    uint64_t acquired_ms;
} comfort_sample_t;

/* Internal bands; callers initialize/reset, then leave these fields alone. */
typedef enum {
    COMFORT_BAND_UNKNOWN, COMFORT_BAND_OK, COMFORT_BAND_LOW, COMFORT_BAND_HIGH
} comfort_band_t;

typedef struct {
    comfort_band_t temperature, humidity;
    uint64_t last_step_ms;
    bool has_step;
} comfort_context_t;

typedef struct {
    comfort_state_t state; /* Validity > temperature > humidity > OK. */
    comfort_reason_t reasons[2]; /* Temperature first; unused entries NONE. */
    uint8_t reason_count;
    comfort_validity_t temperature_validity, humidity_validity;
} comfort_result_t;

/* Zero initialization is equivalent. NULL is safe. No persistent state. */
void comfort_reset(comfort_context_t *context);

/* Same-epoch monotonic ms; latest snapshots passed by value. Serialized caller.
 * Age == FRESH_MS is valid. Invalid dimensions lose hysteresis immediately.
 * A backwards clock or call gap > FRESH_MS resets both bands before evaluation.
 * NULL context fails closed. Compile without fast-math/finite-math-only.
 */
comfort_result_t comfort_step(comfort_context_t *context,
                             comfort_sample_t temperature,
                             comfort_sample_t humidity, uint64_t now_ms);

#endif
