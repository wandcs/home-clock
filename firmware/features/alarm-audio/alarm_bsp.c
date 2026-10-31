#include "alarm_bsp.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_vol.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include <string.h>

#if !CONFIG_SPIRAM_FETCH_INSTRUCTIONS || !CONFIG_SPIRAM_RODATA || \
    !CONFIG_SPIRAM_XIP_FROM_PSRAM || !CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY || \
    !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#error "Audio external stack requires the reviewed IDF6.1 PSRAM XiP configuration"
#endif

/* One permanent owner: no RX/microphone and no BSP hidden duplex channels.
 * Handles survive until reboot, even partial setup failure. No unbounded
 * i2s_channel_disable/codec close on the UI stop path. Locked IDF6.1/codec1.6.2. */
static aa_player player;
static esp_codec_dev_handle_t codec;
static i2s_chan_handle_t tx;
static bool opened, poisoned, io_failed; /* worker only */
static atomic_uint configured, worker_active, exit_requested, ready;
static atomic_uint preparation, preparation_error, mute_readback, volume_readback;
static atomic_uint dac_register, dma_data_bytes;
static atomic_uint worker_stack_free_min, normal_blocks, written_samples;
static portMUX_TYPE amp_lock = portMUX_INITIALIZER_UNLOCKED;
static bool pa_on; /* amp_lock only */
static unsigned cutoff_count, cutoff_failures; /* amp_lock only */

static int expected_dac(int volume)
{
    const esp_codec_dev_vol_range_t range = {
        .min_vol = { .vol = 0, .db_value = -95.5f },
        .max_vol = { .vol = 255, .db_value = 32.0f }
    };
    esp_codec_dev_hw_gain_t hw = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f };
    return esp_codec_dev_vol_calc_reg(&range,
           -50.0f + volume * 0.5f - esp_codec_dev_col_calc_hw_gain(&hw));
}
static bool reg_is(int reg, int mask, int expected)
{
    int value = 0;
    bool read = esp_codec_dev_read_reg(codec, reg, &value) == ESP_CODEC_DEV_OK;
    if (read && reg == 0x32) atomic_store(&dac_register, (unsigned)value);
    return read && (value & mask) == expected;
}
static bool mute(bool muted)
{
    bool ok = esp_codec_dev_set_out_mute(codec, muted) == ESP_CODEC_DEV_OK &&
              reg_is(0x31, 0x60, muted ? 0x60 : 0);
    atomic_store(&mute_readback, ok && muted);
    return ok;
}
static aa_result cutoff(void *ctx)
{
    (void)ctx;
    /* Only bounded GPIO HAL access under this lock; never I2C or a task wait.
     * enable uses the SAME lock and rechecks AA_WRITING before PA on. The codec
     * has pa_pin=-1, so blocked initialization cannot later turn PA back on. */
    portENTER_CRITICAL(&amp_lock);
    bool ok = gpio_set_level(BSP_POWER_AMP_IO, 0) == ESP_OK &&
              gpio_get_level(BSP_POWER_AMP_IO) == 0;
    pa_on = !ok;
    ++cutoff_count;
    if (!ok) ++cutoff_failures;
    portEXIT_CRITICAL(&amp_lock);
    if (!ok) atomic_store(&ready, false);
    return ok ? AA_OK : AA_QUIET_ERROR;
}
static aa_result put(void *ctx, int16_t *data, size_t count, size_t *accepted)
{
    (void)ctx;
    *accepted = 0;
    if (!opened || count > AA_BLOCK) { io_failed = true; return AA_WRITE_ERROR; }
    size_t bytes = 0;
    /* IDF timeout is per driver wait, not a hard total-call deadline. PA cutoff
     * is independent of it. Actual byte count is checked, never replayed. */
    esp_err_t err = i2s_channel_write(tx, data, count * sizeof(*data), &bytes, 40);
    *accepted = bytes / sizeof(*data);
    atomic_fetch_add(&written_samples, (unsigned)*accepted);
    if (err != ESP_OK) {
        io_failed = true;
        return err == ESP_ERR_TIMEOUT ? AA_TIMEOUT : AA_WRITE_ERROR;
    }
    if (bytes != count * sizeof(*data)) { io_failed = true; return AA_SHORT_WRITE; }
    for (size_t n = 0; n < count; ++n) {
        if (data[n]) { atomic_fetch_add(&normal_blocks, 1); break; }
    }
    return AA_OK;
}
static aa_result zero_ring(void)
{
    /* 3*882=2646B exceeds the explicit 2048B TX ring. The PA is already off.
     * Reuse the owner's PCM block, no second 882B internal zero buffer. */
    memset(player.block, 0, sizeof(player.block));
    for (unsigned n = 0; n < 3; ++n) {
        size_t accepted;
        aa_result result = put(NULL, player.block, AA_BLOCK, &accepted);
        if (result != AA_OK) return result;
    }
    return AA_OK;
}
static aa_result quiet(void *ctx)
{
    (void)ctx;
    bool ok = cutoff(NULL) == AA_OK;
    if (opened && !io_failed && zero_ring() != AA_OK) ok = false;
    if (codec && !mute(true)) ok = false;
    if (!ok) atomic_store(&ready, false);
    return ok ? AA_OK : AA_QUIET_ERROR;
}
static bool hardware_create(void)
{
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channel.dma_desc_num = AA_DMA_DESCRIPTORS;
    channel.dma_frame_num = AA_DMA_FRAMES;
    channel.auto_clear = true;
    if (i2s_new_channel(&channel, &tx, NULL) != ESP_OK) return false;
    i2s_std_config_t standard = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AA_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = { .mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK,
                      .ws = BSP_I2S_LCLK, .dout = BSP_I2S_DOUT, .din = I2S_GPIO_UNUSED }
    };
    if (i2s_channel_init_std_mode(tx, &standard) != ESP_OK ||
        i2s_channel_enable(tx) != ESP_OK) return false;
    audio_codec_i2s_cfg_t data_cfg = { .port = CONFIG_BSP_I2S_NUM, .tx_handle = tx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&data_cfg);
    audio_codec_i2c_cfg_t ctrl_cfg = { .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bsp_i2c_get_handle() };
    if (!data || !ctrl_cfg.bus_handle) return false;
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    if (!ctrl) return false;
    es8311_codec_cfg_t es_cfg = { .ctrl_if = ctrl, .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1, .use_mclk = true,
        .hw_gain = { .pa_voltage = 5.0f, .codec_dac_voltage = 3.3f } };
    const audio_codec_if_t *device = es8311_codec_new(&es_cfg);
    if (!device) return false;
    esp_codec_dev_cfg_t cfg = { .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = device, .data_if = data };
    codec = esp_codec_dev_new(&cfg);
    return codec != NULL;
}
static aa_result prepare(void *ctx, const aa_output_config *config)
{
    (void)ctx;
    if (!aa_output_config_valid(config)) return AA_INVALID_CONFIG;
    if (poisoned || cutoff(NULL) != AA_OK) return AA_OPEN_ERROR;
    io_failed = false;
    if (!codec && !hardware_create()) { poisoned = true; return AA_OPEN_ERROR; }
    if (!mute(true) || esp_codec_dev_set_out_vol(codec, config->codec_volume) != ESP_CODEC_DEV_OK)
        return AA_OPEN_ERROR;
    esp_codec_dev_sample_info_t fs = { .sample_rate = AA_RATE, .channel = 1, .bits_per_sample = 16 };
    if (!opened) {
        if (esp_codec_dev_open(codec, &fs) != ESP_CODEC_DEV_OK) { poisoned = true; return AA_OPEN_ERROR; }
        opened = true;
    }
    i2s_chan_info_t info;
    if (i2s_channel_get_info(tx, &info) != ESP_OK) return AA_OPEN_ERROR;
    atomic_store(&dma_data_bytes, info.total_dma_buf_size);
    if (info.total_dma_buf_size != AA_DMA_DATA_BYTES) return AA_OPEN_ERROR;
    /* Codec open may restore/unmute internally, but cannot own PA. Its volume
     * setter can swallow a driver error: actual register 0x32 is the proof. */
    bool vol_ok = reg_is(0x32, 0xff, expected_dac(config->codec_volume));
    atomic_store(&volume_readback, vol_ok);
    if (!mute(true) || !vol_ok || cutoff(NULL) != AA_OK) return AA_OPEN_ERROR;
    return zero_ring();
}
static aa_result enable(void *ctx)
{
    (void)ctx;
    bool silent = aa_level(&player) == 0;
    if (!opened || !atomic_load(&ready) ||
        !reg_is(0x32, 0xff, expected_dac(player.output.codec_volume)) || !mute(silent)) return AA_OPEN_ERROR;
    bool ok = true;
    portENTER_CRITICAL(&amp_lock);
    if (!silent && aa_status(&player, NULL) == AA_WRITING && !atomic_load(&exit_requested)) {
        ok = gpio_set_level(BSP_POWER_AMP_IO, 1) == ESP_OK && gpio_get_level(BSP_POWER_AMP_IO) == 1;
        pa_on = ok;
    }
    portEXIT_CRITICAL(&amp_lock);
    return ok ? AA_OK : AA_OPEN_ERROR;
}
static void worker(void *unused)
{
    (void)unused;
    atomic_store(&worker_active, true);
    aa_result result = prepare(NULL, &player.output);
    if (result == AA_OK) {
        atomic_store(&preparation, AA_PREP_READY);
        atomic_store(&ready, true);
    } else {
        if (quiet(NULL) != AA_OK) result = AA_QUIET_ERROR;
        atomic_store(&preparation_error, result);
        atomic_store(&preparation, AA_PREP_FAILED);
        atomic_store(&player.error, result);
        atomic_store(&player.state, AA_FAILED);
    }
    for (;;) {
        if (atomic_load(&exit_requested)) aa_stop(&player);
        aa_pump(&player);
        unsigned state = aa_status(&player, NULL);
        if (state == AA_FAILED) atomic_store(&ready, false);
        atomic_store(&worker_stack_free_min, uxTaskGetStackHighWaterMark(NULL));
        if (atomic_load(&exit_requested) && (state == AA_IDLE || state == AA_FAILED)) break;
        vTaskDelay(1);
    }
    aa_stop(&player);
    atomic_store(&ready, false);
    atomic_store(&worker_active, false);
    vTaskDeleteWithCaps(NULL);
}
aa_result aa_bsp_startup(void)
{
    if (atomic_load(&configured)) return AA_BUSY;
    aa_sink sink = { .prepare = prepare, .enable = enable, .write = put,
                     .quiet = quiet, .cutoff = cutoff };
    aa_init(&player, sink, (aa_output_config){ .codec_volume = AA_ENGINEERING_VOLUME });
    atomic_store(&configured, true); /* stable player even on hardware/task failure */
    atomic_store(&preparation, AA_PREP_RUNNING);
    gpio_config_t gpio = { .pin_bit_mask = UINT64_C(1) << BSP_POWER_AMP_IO,
                          .mode = GPIO_MODE_INPUT_OUTPUT };
    if (gpio_config(&gpio) != ESP_OK || cutoff(NULL) != AA_OK ||
        xTaskCreateWithCaps(worker, "alarm-audio", AA_WORKER_STACK, NULL, 3, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&preparation_error, AA_OPEN_ERROR);
        atomic_store(&preparation, AA_PREP_FAILED);
        atomic_store(&player.error, AA_OPEN_ERROR);
        atomic_store(&player.state, AA_FAILED);
        return AA_OPEN_ERROR;
    }
    return AA_OK;
}
bool aa_bsp_ready(void) { return atomic_load(&ready); }
aa_player *aa_bsp_player(void) { return atomic_load(&configured) ? &player : NULL; }
void aa_bsp_shutdown(void)
{
    if (atomic_load(&configured)) { atomic_store(&exit_requested, true); aa_shutdown(&player); }
}
void aa_bsp_diagnostics(aa_bsp_diagnostic *out)
{
    if (!out) return;
    *out = (aa_bsp_diagnostic){ .preparation = atomic_load(&preparation),
        .ready = aa_bsp_ready(), .worker_active = atomic_load(&worker_active),
        .mute_readback = atomic_load(&mute_readback), .volume_readback = atomic_load(&volume_readback),
        .codec_volume = AA_ENGINEERING_VOLUME, .dac_register = atomic_load(&dac_register),
        .dma_data_bytes = atomic_load(&dma_data_bytes), .worker_stack_free_min = atomic_load(&worker_stack_free_min),
        .normal_blocks = atomic_load(&normal_blocks), .written_samples = atomic_load(&written_samples),
        .preparation_error = atomic_load(&preparation_error) };
    portENTER_CRITICAL(&amp_lock);
    out->pa_on = pa_on; out->cutoff_count = cutoff_count; out->cutoff_failures = cutoff_failures;
    portEXIT_CRITICAL(&amp_lock);
}
