#include "alarm_audio.h"
#include <math.h>

double aa_gain(uint32_t n)
{
    double u = n >= 30u * AA_RATE ? 1.0 : (double)n / (30u * AA_RATE);
    return pow(10.0, (-24.0 + 18.0 * u) / 20.0);
}
void aa_source_reset(aa_source *s) { *s = (aa_source){0}; }
void aa_source_preview_reset(aa_source *s) { *s = (aa_source){ .preview = true }; }
void aa_source_render(aa_source *s, int16_t *out, size_t count, bool fade)
{
    /* Fade is exactly one full 441-sample block, never independently chunked. */
    uint32_t anchor = UINT32_MAX;
    float low = 0, step = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bucket = s->rise / AA_BLOCK;
        if (bucket != anchor) {
            anchor = bucket;
            low = (float)aa_gain(bucket * AA_BLOCK);
            step = ((float)aa_gain((bucket + 1) * AA_BLOCK) - low) / AA_BLOCK;
        }
        /* Global 20ms grid, so arbitrary caller chunk boundaries are identical.
         * Two pow evaluations per grid block, single precision sample math on
         * P4 FPU; avoids 22050 software-double pow calls per second. */
        float gain = low + step * (s->rise % AA_BLOCK);
        if (s->preview) {
            float u = s->rise >= AA_RATE / 10u ? 1.0f : (float)s->rise / (AA_RATE / 10u);
            gain = 0.5011872336f * u; /* same -6 dB platform, gentle 100ms entrance */
        }
        float envelope = fade ? 0.5f + 0.5f * cosf(3.14159265358979323846f *
                                   (float)i / (AA_FADE - 1)) : 1.0f;
        float value = aa_reference[s->phase] * gain * envelope;
        out[i] = (int16_t)lroundf(value);
        if (++s->phase == aa_reference_count) s->phase = 0;
        if (s->rise < (s->preview ? AA_RATE / 10u : 30u * AA_RATE)) ++s->rise;
    }
}
