#ifndef CLOCK_ALARM_RUNTIME_H
#define CLOCK_ALARM_RUNTIME_H
#include "alarm_controller.h"
#include "alarm_store.h"
#include "alarm_trigger_audio.h"
#include <stdatomic.h>
typedef bool (*ar_clock_fn)(void *,at_clock *);
typedef struct {
    am_config config;
    am_summary next;
    uint32_t revision;
    at_phase phase;
    at_error error;
    aa_state audio;
    aa_result audio_error;
    as_status storage;
    bool known,blocked,audio_ready,calendar_notice,active;
    unsigned session_count;
} ar_snapshot;
typedef struct {
    as_store store;
    at_state trigger;
    ar_clock_fn read_clock;
    void *clock_ctx;
    atomic_bool stop_pending;
    as_status storage_status;
    uint32_t revision;
} ar_runtime;
void ar_init(ar_runtime *,as_port,ar_clock_fn,void *);
/* Owner only. UI jobs and checkpoints share this exact instance/port. */
am_reply ar_ui(ar_runtime *,const am_job *);
void ar_step(ar_runtime *,aa_player *,bool,const af_year *,size_t);
/* Caller serializes this short request-only operation against request_stop +
 * aa_stop; never hold that gate during store, clock reads, UI or aa_pump. */
void ar_dispatch(ar_runtime *,aa_player *,bool);
void ar_request_stop(ar_runtime *);
void ar_copy_into(ar_runtime *,aa_player *,bool,const af_year *,size_t,ar_snapshot *);
#endif
