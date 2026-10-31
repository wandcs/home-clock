#ifndef CLOCK_ALARM_STORE_NVS_H
#define CLOCK_ALARM_STORE_NVS_H
#include "alarm_store.h"
/* Uses ONLY nvs/home_clock/alarms; caller owns initialization and one writer.
 * No erases, retries or actual task creation. Constructing the port does no I/O. */
as_port as_nvs_port(void);
#endif
