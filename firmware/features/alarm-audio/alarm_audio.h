#ifndef HOME_CLOCK_ALARM_AUDIO_H
#define HOME_CLOCK_ALARM_AUDIO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "audio requests require lock-free int atomics");

enum { AA_RATE = 22050, AA_PERIOD = 88200, AA_BLOCK = 441, AA_FADE = 441 };
typedef struct { uint32_t phase, rise; bool preview; } aa_source;
void aa_source_reset(aa_source *s);
void aa_source_preview_reset(aa_source *s); /* 100ms rise to normal final platform */
/* Valid initialized source/output required. fade=true requires count=AA_FADE. */
void aa_source_render(aa_source *s, int16_t *out, size_t count, bool fade);
double aa_gain(uint32_t sample);
extern const int16_t aa_reference[AA_PERIOD];

/* start/stop/status are nonblocking task-context calls, not ISR APIs.
 * One owner calls aa_pump; it alone may call blocking sink operations.
 * cutoff is the sole exception: strictly nonblocking, thread-safe, no I2C/driver
 * wait, called synchronously by stop even while another sink call is blocked.
 * A claimed write can finish after cancellation. No new normal write is claimed
 * afterwards. Sink callbacks MUST return; a C callback cannot be preempted here. */
typedef enum { AA_IDLE, AA_REQUESTED, AA_OPENING, AA_PLAYING, AA_WRITING,
               AA_STOPPING, AA_FAILED, AA_RECOVERING } aa_state;
typedef enum { AA_OK, AA_BUSY, AA_INVALID_CONFIG, AA_OPEN_ERROR,
               AA_WRITE_ERROR, AA_SHORT_WRITE, AA_TIMEOUT, AA_QUIET_ERROR } aa_result;
typedef struct {
    int codec_volume;             /* engineering output level 1..100, not SPL */
} aa_output_config;
bool aa_output_config_valid(const aa_output_config *config);
typedef struct {
    void *ctx;
    aa_result (*prepare)(void *, const aa_output_config *); /* opens MUTED, clears old data */
    aa_result (*enable)(void *);  /* explicit unmute after cancellation check */
    aa_result (*write)(void *, int16_t *, size_t, size_t *); /* samples accepted */
    aa_result (*quiet)(void *);   /* drain/clear, mute/PA off, including partial open */
    aa_result (*cutoff)(void *);  /* optional immediate hardware silence; see above */
} aa_sink;
typedef struct {
    atomic_uint state, error;
    atomic_uint owner, disabled, cutoff_failed;
    atomic_uint level, start_guard, preview;
    float level_gain; /* worker only; interpolate live level changes over one block */
    aa_source source;
    aa_sink sink;
    aa_output_config output;     /* immutable after init */
    bool prepared, submitted_normal;
    int16_t block[AA_BLOCK];
} aa_player;
void aa_init(aa_player *p, aa_sink sink, aa_output_config config);
aa_result aa_start(aa_player *p);
aa_result aa_start_preview(aa_player *p); /* same worker/sink; no normal 30s wait */
bool aa_set_level(aa_player *p, unsigned level); /* 0..100, squared amplitude, lock-free */
unsigned aa_level(const aa_player *p);
void aa_stop(aa_player *p);
void aa_shutdown(aa_player *p); /* permanently refuses future start; owner drains */
aa_state aa_status(const aa_player *p, aa_result *error);
aa_result aa_pump(aa_player *p); /* one bounded block or transition; worker only */
/* Nonblocking request: worker rechecks quiet before IDLE; never replays. */
aa_result aa_ack_error(aa_player *p);
#endif
