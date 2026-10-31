#ifndef CLOCK_ALARM_TRIGGER_H
#define CLOCK_ALARM_TRIGGER_H
#include "alarm_config.h"
#include "alarm_audio.h"

enum { AT_POLL_MAX_MS=5000, AT_CHECKPOINT_MAX_MS=8000, AT_RING_MS=180000 };
/* Logical consistent image, NOT a wire format or authorization for a new NVS
 * key. Integration must atomically store config+8 revisions+ledger through the
 * existing sole writer. A confirmed ALM1 config alone cannot provide this. */
typedef struct {
    uint32_t revision;       /* nonzero; increment on any time/mode/enabled edit */
    uint32_t once_revision;  /* pending conditional closure; 0 = none */
    uint16_t day1;          /* current-plan high-water civil day since 2000-01-01, plus one */
} at_record;
typedef struct { am_config config; at_record records[AM_CAPACITY]; } at_image;
bool at_image_valid(const at_image *);
bool at_image_equal(const at_image *,const at_image *);
/* New IDs must follow config.next_id; deleted IDs are never resurrected.
 * Revision overflow rejects the whole edit; no silent wrap/reset. Explicit
 * changed enabled entries rearm (enable, time or repeat edits). Closing and
 * no-op saves keep consumption. Wire layout and CRC are unchanged. */
bool at_image_reconfigure(const at_image *,const am_config *,at_image *);
bool at_image_first(const am_config *,at_image *); /* explicit first migration only */

/* One fixed UTC+8 civil mapping per revision. wall_anchor_ms is milliseconds
 * since 2000-01-01 00:00:00 Beijing time. Derive current wall time solely from
 * monotonic delta. Caller reports each real sync event UTC+mono pair, including
 * zero adjustment. Consecutive revision and an event inside a <=5s poll gap
 * permit two real continuous segments; skipped revisions/stale events rebase.
 * Do not sample mutable gettimeofday without reporting its mapping change. */
typedef struct {
    bool trusted;
    uint32_t revision;
    int64_t wall_anchor_ms;
    uint64_t mono_anchor_ms,now_mono_ms;
} at_clock;
bool at_wall_ms(const af_time *,int64_t *);
/* Checked inverse for owner-side runtime summaries; same civil conversion as
 * scheduling, avoiding another date algorithm in the integration layer. */
bool at_civil_from_wall(int64_t,af_time *);
bool at_clock_wall(const at_clock *,int64_t *);
/* Pure adapter for existing network_state last_sync_utc_us/last_sync_ms/
 * sync_count/time_valid plus app_uptime. No system clock or timezone calls. */
bool at_clock_from_sync(bool,uint32_t,int64_t,uint64_t,uint64_t,at_clock *);

typedef enum { AT_SILENT, AT_START_COMMAND, AT_STARTING, AT_RINGING,
               AT_STOPPING, AT_AUDIO_FAILED } at_phase;
typedef enum { AT_ERROR_NONE, AT_BAD_INPUT, AT_CLOCK_CONTRACT, AT_POLL_GAP,
               AT_STORAGE_ERROR, AT_STORAGE_UNCERTAIN, AT_CHECKPOINT_EXPIRED,
               AT_SEQUENCE_LIMIT, AT_AUDIO_ERROR } at_error;
typedef enum { AT_MARK, AT_CLOSE_ONCE } at_job_kind;
typedef struct {
    uint32_t token;
    at_job_kind kind;
    at_image expected,proposed;
} at_job;
typedef enum { AT_AUDIO_START, AT_AUDIO_STOP } at_audio_kind;
typedef struct { uint32_t token; at_audio_kind kind; } at_audio_command;
typedef enum { AT_COMMITTED, AT_UNCHANGED, AT_DIFFERENT, AT_UNCERTAIN } at_store_result;

typedef struct {
    at_image image;
    at_record seen[AM_CAPACITY]; /* RAM high-water/pending overlay, not persisted */
    uint64_t effective_after[AM_CAPACITY]; /* strict edit-confirm monotonic point */
    at_phase phase;
    at_error error;
    aa_result audio_error;
    bool loaded,storage_blocked,baseline,have_mono,quiet_observed;
    at_clock mapping;
    int64_t previous_wall;
    uint64_t last_mono,poll_mono,start_ms,deadline_ms;
    uint32_t session_ids[AM_CAPACITY];
    uint8_t session_count;
    bool session_fallback;
    at_job job;
    bool job_pending,job_taken,batch_valid;
    uint8_t batch_count;
    uint32_t batch_ids[AM_CAPACITY],batch_revisions[AM_CAPACITY];
    bool batch_fallback;
    uint64_t job_started_ms;
    uint64_t batch_due_mono,session_due_mono; /* latest occurrence in each fact */
    uint32_t next_job_token,next_audio_token;
    at_audio_command audio_command;
    bool command_pending,command_taken;
    uint32_t skipped_busy;
} at_state;

void at_init(at_state *); /* blocked until consistent image independently loaded */
/* Initial/recovery readback is explicit, invalidates outstanding jobs/commands.
 * Recovery never replays a marked occurrence. Cannot be used while audio active. */
bool at_restore(at_state *,const at_image *);
/* Confirmed sole-writer edit notification. Caller must supply every committed
 * edit and its current clock confirmation point; changed IDs become eligible
 * strictly after that point without skipping an entire next poll interval.
 * Stale/revision-regressing snapshots are rejected. */
bool at_observe_image(at_state *,const at_image *,const at_clock *confirmed_at);
/* One owner; no I/O, allocation, locks or audio calls. Call <=1s normally.
 * Longer than 5s between valid clock samples rebases without replay. */
bool at_step(at_state *,const at_clock *,const af_year *,size_t,aa_state,aa_result);
void at_stop(at_state *);
bool at_take_job(at_state *,at_job *);
/* Must pass the current mapping at readback delivery; a proven sync AFTER the
 * batch's latest real occurrence preserves it. Unknown event history, changed
 * plan, stop, >8s delay or monotonic regression cannot produce a late start.
 * COMMITTED requires actual independent readback == proposed. */
bool at_accept_job(at_state *,uint32_t,at_store_result,const at_image *,const at_clock *);
bool at_take_audio(at_state *,at_audio_command *);
/* START result is aa_start's request result, not proof of audible playback.
 * STOP acknowledgement never marks quiet; step must observe AA_IDLE. */
bool at_accept_audio(at_state *,at_audio_command,aa_result);
#endif
