#ifndef ALARM_SERVICE_H
#define ALARM_SERVICE_H
#include "alarm_controller.h"
#include "alarm_runtime.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
typedef struct { QueueHandle_t jobs,replies;TaskHandle_t worker; } am_service;
/* App lifetime. Call once outside LVGL construction; no device initialization. */
bool am_service_start(am_service *service);
bool am_service_submit(void *service,const am_job *job);
bool am_service_take(void *service,am_reply *reply);
bool am_service_copy_into(void *service,ar_snapshot *out);
void am_service_stop(void *service);
bool am_service_audio_busy(void);
unsigned am_service_stack_free_min(void);
typedef struct {
    unsigned level, saved_level;
    bool loaded, saving, previewing, available;
    int load_error, save_error;
} am_volume_snapshot;
void am_service_volume_read(am_volume_snapshot *out);
/* Task-context, nonblocking: only an explicit gesture starts preview. Returns
 * false for invalid level, unavailable output or a real alarm. 0 is silent. */
bool am_service_volume_preview(unsigned level);
/* Release/leave/sleep: immediate preview cutoff plus one asynchronous save.
 * Idempotent until another edit; never stops an actual scheduled alarm. */
void am_service_volume_finish(void);
#endif
