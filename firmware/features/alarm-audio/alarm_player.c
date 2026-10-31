#include "alarm_audio.h"
#include <string.h>
#include <math.h>

bool aa_output_config_valid(const aa_output_config *k)
{ return k && k->codec_volume >= 1 && k->codec_volume <= 100; }
void aa_init(aa_player *p, aa_sink sink, aa_output_config k)
{
    memset(p, 0, sizeof(*p));
    p->sink = sink; p->output = k;
    atomic_init(&p->state, AA_IDLE); atomic_init(&p->error, AA_OK);
    atomic_init(&p->owner, false);
    atomic_init(&p->disabled, false);
    atomic_init(&p->cutoff_failed, false);
    atomic_init(&p->level, 100); atomic_init(&p->start_guard, false);
    atomic_init(&p->preview, false); p->level_gain = 1.0f;
}
aa_state aa_status(const aa_player *p, aa_result *error)
{
    unsigned state = atomic_load(&p->state);
    if (error) *error = atomic_load(&p->cutoff_failed) ? AA_QUIET_ERROR : (aa_result)atomic_load(&p->error);
    return (aa_state)state;
}
bool aa_set_level(aa_player *p, unsigned level)
{
    if (!p || level > 100) return false;
    atomic_store(&p->level, level); return true;
}
unsigned aa_level(const aa_player *p) { return atomic_load(&p->level); }
static float level_gain(const aa_player *p)
{
    unsigned level = aa_level(p);
    return (float)(level * level) / 10000.0f;
}
static aa_result start_mode(aa_player *p, bool preview)
{
    unsigned unlocked = false;
    if (!atomic_compare_exchange_strong(&p->start_guard, &unlocked, true)) return AA_BUSY;
    aa_result result = AA_BUSY;
    if (atomic_load(&p->disabled) || atomic_load(&p->cutoff_failed)) goto done;
    if (!aa_output_config_valid(&p->output)) { result = AA_INVALID_CONFIG; goto done; }
    if (atomic_load(&p->state) != AA_IDLE) goto done;
    /* Publish mode before REQUESTED; the worker can observe it immediately.
     * Serialize start producers so a rejected normal start cannot alter preview. */
    atomic_store(&p->preview, preview);
    unsigned expected = AA_IDLE;
    if (!atomic_compare_exchange_strong(&p->state, &expected, AA_REQUESTED)) goto done;
    if (atomic_load(&p->disabled)) { aa_stop(p); goto done; }
    result = AA_OK;
done:
    atomic_store(&p->start_guard, false);
    return result;
}
aa_result aa_start(aa_player *p) { return start_mode(p, false); }
aa_result aa_start_preview(aa_player *p) { return start_mode(p, true); }
void aa_stop(aa_player *p)
{
    unsigned s = atomic_load(&p->state);
    while (s != AA_IDLE && s != AA_FAILED && s != AA_STOPPING) {
        if (atomic_compare_exchange_weak(&p->state, &s, AA_STOPPING)) break;
    }
    /* A blocked prepare/enable/write cannot delay this hardware cutoff. */
    if (p->sink.cutoff && p->sink.cutoff(p->sink.ctx) != AA_OK)
        atomic_store(&p->cutoff_failed, true);
}
void aa_shutdown(aa_player *p)
{
    atomic_store(&p->disabled, true);
    aa_stop(p);
}
static aa_result finish(aa_player *p, aa_result error)
{
    /* Always attempt cleanup, even after partial prepare. Sink is idempotent. */
    aa_result quiet = p->sink.quiet(p->sink.ctx);
    if (quiet != AA_OK || atomic_exchange(&p->cutoff_failed, false)) error = AA_QUIET_ERROR;
    p->prepared = false;
    p->submitted_normal = false;
    aa_source_reset(&p->source);
    atomic_store(&p->preview, false);
    atomic_store(&p->error, error);
    atomic_store(&p->state, error == AA_OK ? AA_IDLE : AA_FAILED);
    return error;
}
static aa_result write_block(aa_player *p)
{
    size_t accepted = 0;
    aa_result r = p->sink.write(p->sink.ctx, p->block, AA_BLOCK, &accepted);
    if (r != AA_OK) return r; /* unknown partial transfer: never replay */
    return accepted == AA_BLOCK ? AA_OK : AA_SHORT_WRITE;
}
static void apply_level(aa_player *p)
{
    float target = level_gain(p), initial = p->level_gain;
    for (size_t i = 0; i < AA_BLOCK; ++i) {
        float gain = initial + (target - initial) * ((float)(i + 1) / AA_BLOCK);
        p->block[i] = (int16_t)lroundf(p->block[i] * gain);
    }
    p->level_gain = target;
}
aa_result aa_pump(aa_player *p)
{
    if (atomic_exchange(&p->owner, true)) return AA_BUSY;
    aa_result r = AA_OK;
    unsigned s = atomic_load(&p->state), expected;
    if (s == AA_REQUESTED) {
        expected = AA_REQUESTED;
        if (!atomic_compare_exchange_strong(&p->state, &expected, AA_OPENING)) goto done;
        if (atomic_load(&p->preview)) aa_source_preview_reset(&p->source);
        else aa_source_reset(&p->source);
        p->level_gain = level_gain(p);
        p->submitted_normal = false;
        r = p->sink.prepare(p->sink.ctx, &p->output);
        if (r != AA_OK) { r = finish(p, r); goto done; }
        p->prepared = true;
        /* Claim enable. A concurrent stop can cancel its completion, but cannot
         * cause another prepare/unmute/normal block after STOPPING. */
        expected = AA_OPENING;
        if (!atomic_compare_exchange_strong(&p->state, &expected, AA_WRITING)) {
            r = finish(p, AA_OK); goto done;
        }
        r = p->sink.enable(p->sink.ctx);
        if (r != AA_OK) { r = finish(p, r); goto done; }
        expected = AA_WRITING;
        atomic_compare_exchange_strong(&p->state, &expected, AA_PLAYING);
    } else if (s == AA_PLAYING) {
        aa_source next = p->source;
        aa_source_render(&next, p->block, AA_BLOCK, false);
        apply_level(p);
        expected = AA_PLAYING;
        if (!atomic_compare_exchange_strong(&p->state, &expected, AA_WRITING)) goto done;
        p->source = next;
        p->submitted_normal = true;
        r = write_block(p);
        if (r != AA_OK) { r = finish(p, r); goto done; }
        expected = AA_WRITING;
        atomic_compare_exchange_strong(&p->state, &expected, AA_PLAYING);
    } else if (s == AA_STOPPING) {
        if (p->prepared && p->submitted_normal) {
            aa_source_render(&p->source, p->block, AA_FADE, true);
            apply_level(p);
            r = write_block(p);
        }
        r = finish(p, r);
    } else if (s == AA_RECOVERING) {
        r = finish(p, AA_OK);
    }
done:
    atomic_store(&p->owner, false);
    return r;
}
aa_result aa_ack_error(aa_player *p)
{
    unsigned expected = AA_FAILED;
    return atomic_compare_exchange_strong(&p->state, &expected, AA_RECOVERING) ? AA_OK : AA_BUSY;
}
