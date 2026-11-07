#ifndef HOME_CLOCK_ALARM_BSP_H
#define HOME_CLOCK_ALARM_BSP_H
#include "alarm_audio.h"
/* Codec control units: 100 maps to 0 dB on the locked default curve.
 * Not percent loudness or an acoustic/SPL calibration; source ramp is separate. */
enum { AA_ENGINEERING_VOLUME = 100, AA_WORKER_STACK = 4096, AA_WORKER_PRIORITY = 7,
       AA_DMA_DESCRIPTORS = 4, AA_DMA_FRAMES = 256, AA_DMA_DATA_BYTES = 2048 };
typedef enum { AA_PREP_NOT_STARTED, AA_PREP_RUNNING, AA_PREP_READY, AA_PREP_FAILED } aa_prepare_state;
typedef struct {
    aa_prepare_state preparation;
    bool ready, worker_active, pa_on, mute_readback, volume_readback;
    unsigned codec_volume, dac_register, dma_data_bytes, worker_stack_free_min;
    unsigned cutoff_count, cutoff_failures, normal_blocks, written_samples;
    /* feed gap is write-end to next write-start; >20ms is risk, not an IRQ underrun proof. */
    unsigned feed_gap_max_us, feed_gap_over_20ms, render_max_us, write_max_us, stream_frames;
    aa_result preparation_error;
} aa_bsp_diagnostic;
/* Startup once, after BSP I2C/display initialization, before network workers.
 * AA_OK accepts the worker, not hardware readiness. Preparation is asynchronous,
 * PA remains off and only zeros are submitted. No microphone/RX is created.
 * No implicit internal-stack fallback. Engineering level is NOT listening/SPL
 * calibration. Reserve I2S/codec/PA exclusively for this process lifetime. */
aa_result aa_bsp_startup(void);
bool aa_bsp_ready(void);
/* Stable after accepted startup, including preparation failure: stop/status
 * remain usable. Gate NEW starts on ready; never externally pump this player. */
aa_player *aa_bsp_player(void);
void aa_bsp_diagnostics(aa_bsp_diagnostic *out); /* snapshot only, no I2C */
void aa_bsp_shutdown(void);
#endif
