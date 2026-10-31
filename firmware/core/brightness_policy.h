#ifndef HOME_CLOCK_BRIGHTNESS_POLICY_H
#define HOME_CLOCK_BRIGHTNESS_POLICY_H
#include <stdbool.h>
#include <stdint.h>

/* Candidate calibration, not a product comfort guarantee. Units: ms, lux,
 * millipercent (1000 = one percentage point). No automatic screen off. */
#define BP_FRESH_MS UINT64_C(6000)
#define BP_FAULT_MS UINT64_C(30000)
#define BP_UP_CONFIRM_MS UINT64_C(2000)
#define BP_DOWN_CONFIRM_MS UINT64_C(6000)
#define BP_LOW_ENTER_MS UINT64_C(8000)
#define BP_LOW_EXIT_MS UINT64_C(4000)
#define BP_TICK_CAP_MS UINT64_C(250)
#define BP_MIN_MP 2000
#define BP_MAX_MP 80000
#define BP_FALLBACK_MP 20000
#define BP_DEADBAND_MP 2000
#define BP_UP_MP_PER_MS 8
#define BP_DOWN_MP_PER_MS 2
#define BP_LOW_ENTER_LUX 3.0f
#define BP_LOW_EXIT_LUX 8.0f

typedef enum { BP_WAITING, BP_TRACKING, BP_HOLD, BP_FALLBACK } bp_mode_t;
typedef struct { float lux; uint64_t acquired_ms; bool valid; } bp_input_t;
typedef struct {
    int32_t target_millipercent, demand_millipercent;
    uint8_t target_percent;
    bool low_light, low_light_known;
    bp_mode_t mode;
} bp_output_t;
typedef struct {
    uint64_t started_ms, last_tick_ms, sample_ms, demand_since_ms, low_since_ms;
    int32_t target_mp, demand_mp;
    int8_t demand_direction, low_candidate;
    bool have_sample, low_light, low_known;
    bp_mode_t mode;
} bp_state_t;

/* Valid finite nonnegative lux only; invalid argument returns -1. */
int32_t bp_curve(float lux);
void bp_init(bp_state_t *state, uint64_t now_ms, unsigned current_percent);
/* Non-null pointers required; false only on clock rollback, with no writes.
 * Fresh sample contents immutable. New samples alone advance confirmation.
 * Call about every 100ms; the state is caller-owned and not thread-safe. */
bool bp_step(bp_state_t *state, bp_input_t input, uint64_t now_ms, bp_output_t *out);
#endif
