#ifndef HOME_CLOCK_WEATHER_BACKGROUND_H
#define HOME_CLOCK_WEATHER_BACKGROUND_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Independent, caller-owned CPU renderer. No LVGL, IO, allocator or scheduler.
 * Assets are immutable flash; workspace and output must be in PSRAM on P4.
 * The caller supplies fresh CURRENT weather validity, never daily worst code.
 */
#define WB_WIDTH 1024u
#define WB_HEIGHT 600u
#define WB_OUTPUT_BYTES (WB_WIDTH*WB_HEIGHT*2u)
#define WB_GRID_WIDTH 256u
#define WB_GRID_HEIGHT 150u
typedef enum { WB_UNKNOWN, WB_SUNNY, WB_CLOUDY, WB_OVERCAST, WB_RAIN, WB_SNOW, WB_FOG, WB_THUNDER } wb_weather_t;
typedef enum { WB_OUTDOOR_UNKNOWN, WB_DAY, WB_NIGHT } wb_outdoor_t;
typedef enum { WB_NEUTRAL, WB_BLACK, WB_SUN, WB_CLOUD, WB_GREY, WB_WET, WB_WHITE, WB_MOON, WB_CLOUD_MOON } wb_scene_t;
typedef struct {
    wb_weather_t weather;
    wb_outdoor_t outdoor;
    bool current_valid;
    bool low_light_known, low_light;
    bool visible, motion_enabled;
} wb_input_t;
typedef struct {
    wb_scene_t scene;
    uint64_t animation_ms;
    bool moving, needs_redraw;
} wb_frame_t;
typedef struct {
    wb_frame_t frame;
    uint64_t last_ms;
    wb_weather_t phase_weather;
    bool low_light, initialized, visible;
} wb_state_t;
typedef struct { uint8_t rgb[WB_GRID_WIDTH*WB_GRID_HEIGHT*3u]; } wb_workspace_t;

/* Initialize with the actual existing display low-light state. Unknown light
 * thereafter retains this state. A new boot may choose a conservative black.
 * All arguments must be non-null. State is not concurrently shared. */
void wb_init(wb_state_t *state, uint64_t now_ms, bool current_low_light);
/* False on invalid enum/clock rollback, without changing state or output.
 * Pauses preserve phase; resuming never catches up hidden/night/low-light time.
 * Call at visibility exit/entry as well, even if no render happens while hidden.
 * Re-entry requests a repaint even when the new hidden scene was static.
 * Caller normally schedules a few daytime frames/s after measuring on P4.
 * No callbacks/timers, and a redraw never counts as user activity. */
bool wb_step(wb_state_t *state, wb_input_t input, uint64_t now_ms, wb_frame_t *out);
/* Pure same-source frame renderer; no per-frame allocation/decode or mutable
 * globals. Exact native 1024x600 tightly packed RGB565 LE output. Output,
 * workspace and frame must not overlap. Output must not be read by LVGL/DMA
 * during render: integration must serialize ownership, not mutate a live image.
 * False on bad pointers/capacity/scene; buffers untouched on those errors.
 * needs_redraw/moving are advisory to caller; phase is explicit for fixtures. */
bool wb_render(const wb_frame_t *frame, wb_workspace_t *workspace, size_t workspace_bytes,
               uint8_t *output, size_t output_bytes);
#endif
