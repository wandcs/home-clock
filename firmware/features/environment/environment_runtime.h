#ifndef HOME_CLOCK_ENVIRONMENT_RUNTIME_H
#define HOME_CLOCK_ENVIRONMENT_RUNTIME_H

#include "brightness_policy.h"
#include "comfort.h"

/* Latest atomic sensor snapshot, same boot / monotonic millisecond epoch.
 * source_valid means present and last attempted read succeeded, not freshness.
 * Never construct separate success flags for the two fields of one SHT read. */
typedef struct {
    float temperature_c, humidity_pct, lux;
    uint64_t sht_acquired_ms, bh_acquired_ms;
    bool sht_source_valid, bh_source_valid;
} env_snapshot_t;

typedef struct {
    bp_output_t brightness;
    comfort_result_t comfort;
} env_output_t;

/* Caller owned; initialize once, then do not modify these fields. */
typedef struct {
    bp_state_t brightness;
    comfort_context_t comfort;
    bool initialized;
} env_runtime_t;

typedef enum {
    ENV_OK, ENV_INVALID_ARGUMENT, ENV_NOT_INITIALIZED, ENV_CLOCK_ROLLBACK
} env_status_t;

/* Current applied backlight is an input, clamped by the existing core to
 * 2..80%. This does not apply hardware brightness or produce an output yet. */
env_status_t env_runtime_init(env_runtime_t *runtime, uint64_t now_ms,
                              unsigned current_percent);

/* Serialized caller, about 100ms even when a page is hidden; sensors ~2s.
 * Repeat the latest immutable snapshot. No I/O, task, allocation or UTC.
 * On failure context and output remain untouched. Clock rollback requires an
 * explicit init in the new epoch, with that epoch's sensor snapshots.
 * low_light_known=false does NOT mean ordinary/day display: retain the last
 * selected low-light display treatment; no geographic-night inference.
 * Outputs are requested targets, not evidence of successful hardware writes. */
env_status_t env_runtime_step(env_runtime_t *runtime,
                              const env_snapshot_t *snapshot,
                              uint64_t now_ms, env_output_t *output);

#endif
