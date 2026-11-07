#ifndef AA_FAKE_CODEC_H
#define AA_FAKE_CODEC_H
#include <stdbool.h>
#include <stdint.h>
typedef void *esp_codec_dev_handle_t;
typedef struct { uint32_t sample_rate; uint8_t channel, bits_per_sample; } esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_OK 0
int esp_codec_dev_read_reg(esp_codec_dev_handle_t, int, int *);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t, bool);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t, int);
int esp_codec_dev_open(esp_codec_dev_handle_t, esp_codec_dev_sample_info_t *);
int esp_codec_dev_close(esp_codec_dev_handle_t);
int esp_codec_dev_write(esp_codec_dev_handle_t, void *, int);
#endif
