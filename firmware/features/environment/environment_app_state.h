#ifndef HOME_CLOCK_ENVIRONMENT_APP_STATE_H
#define HOME_CLOCK_ENVIRONMENT_APP_STATE_H

#include "environment_runtime.h"
#include "app_state.h"

/* Pass one app_state_read() result. No locks, clocks or app_state access here.
 * NULL fails without changing output. Error/injection decisions belong to
 * the authoritative measurement; counters and sequence are not new samples. */
bool env_snapshot_from_app_state(const sensor_state_t *source,
                                  env_snapshot_t *output);

#endif
