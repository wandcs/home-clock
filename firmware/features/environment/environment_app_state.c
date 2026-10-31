#include "environment_app_state.h"
#include <stddef.h>

_Static_assert(SAMPLE_FRESH_MS == BP_FRESH_MS, "BH freshness contract changed");
_Static_assert(SAMPLE_FRESH_MS == COMFORT_FRESH_MS, "SHT freshness contract changed");

bool env_snapshot_from_app_state(const sensor_state_t *s, env_snapshot_t *out) {
    if (!s || !out) return false;
    *out = (env_snapshot_t){
        .temperature_c=s->sht.first, .humidity_pct=s->sht.second,
        .lux=s->bh.first, .sht_acquired_ms=s->sht.acquired_ms,
        .bh_acquired_ms=s->bh.acquired_ms,
        .sht_source_valid=s->sht.has_value && s->sht.error == ESP_OK,
        .bh_source_valid=s->bh.has_value && s->bh.error == ESP_OK
    };
    return true;
}
