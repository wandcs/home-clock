#ifndef ALARM_NVS_H
#define ALARM_NVS_H
#include "alarm_controller.h"
/* Worker-only. No nvs_flash_init/erase, no namespace/key cleanup. */
am_io_result am_nvs_load(void);
am_io_result am_nvs_save(const am_config *expected,const am_config *proposed);
#endif
