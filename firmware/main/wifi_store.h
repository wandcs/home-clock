#pragma once
#include "esp_wifi.h"
/* Network task only. No diagnostics expose the loaded credential. */
esp_err_t wifi_store_init(void);
esp_err_t wifi_store_load(wifi_config_t *config);
esp_err_t wifi_store_save(const wifi_config_t *config);
#if CONFIG_CLOCK_DIAGNOSTICS
/* Development cleanup, network owner only. RAM remains unchanged until reload. */
esp_err_t wifi_store_clear(void);
#endif
