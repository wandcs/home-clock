#pragma once
#include <stdint.h>
#include "sdkconfig.h"
#ifndef CONFIG_CLOCK_DIAGNOSTICS
#define CONFIG_CLOCK_DIAGNOSTICS 0
#endif
void app_ui_start(void);
void app_ui_status(const char *event,uint32_t request_id); /* Requires BSP display lock. */
void app_ui_command(char *line);
