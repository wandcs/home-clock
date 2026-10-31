#pragma once
#include "sdkconfig.h"
#include "location_data.h"
#include "esp_err.h"
esp_err_t location_store_load(location_record_t *record);
esp_err_t location_store_save(const location_record_t *record);
#if CONFIG_CLOCK_DIAGNOSTICS
void location_store_fail_readback_once(void); /* Development-only post-commit read failure. */
esp_err_t location_store_clear(void); /* Development cleanup: this key only. */
#endif
