#ifndef AA_HOST_H
#define AA_HOST_H
/* Contract shim only. Real locked SDK headers are compiled separately. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#define CONFIG_SPIRAM_FETCH_INSTRUCTIONS 1
#define CONFIG_SPIRAM_RODATA 1
#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1
#define CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY 1
#define CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM 1
#define CONFIG_BSP_I2S_NUM 0
#define BSP_I2C_NUM 0
#define BSP_POWER_AMP_IO 53
#define BSP_I2S_MCLK 13
#define BSP_I2S_SCLK 12
#define BSP_I2S_LCLK 10
#define BSP_I2S_DOUT 9
#define ESP_OK 0
#define ESP_ERR_TIMEOUT 0x107
#define GPIO_MODE_INPUT_OUTPUT 3
#define I2S_GPIO_UNUSED -1
#define I2S_ROLE_MASTER 1
#define I2S_DATA_BIT_WIDTH_16BIT 16
#define I2S_SLOT_MODE_MONO 1
#define ESP_CODEC_DEV_WORK_MODE_DAC 1
#define ESP_CODEC_DEV_TYPE_OUT 1
#define ES8311_CODEC_DEFAULT_ADDR 0x30
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdPASS 1
#define ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY 6
typedef int esp_err_t;
typedef void *i2s_chan_handle_t;
typedef struct { size_t total_dma_buf_size; } i2s_chan_info_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
typedef struct { uint64_t pin_bit_mask; int mode; } gpio_config_t;
typedef struct { int dma_desc_num, dma_frame_num; bool auto_clear; } i2s_chan_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(port,role) {0}
typedef struct { int mclk,bclk,ws,dout,din; } host_gpio;
typedef struct { int clk_cfg,slot_cfg; host_gpio gpio_cfg; } i2s_std_config_t;
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) rate
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits,mode) bits
typedef struct { int port; void *rx_handle,*tx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int port,addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct { int unused; } audio_codec_data_if_t;
typedef struct { int unused; } audio_codec_ctrl_if_t;
typedef struct { int unused; } audio_codec_if_t;
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; int codec_mode,pa_pin;
    bool use_mclk; esp_codec_dev_hw_gain_t hw_gain; } es8311_codec_cfg_t;
typedef struct { int dev_type; const audio_codec_if_t *codec_if;
    const audio_codec_data_if_t *data_if; } esp_codec_dev_cfg_t;
int gpio_config(const gpio_config_t *);
int gpio_set_level(int,int);
int gpio_get_level(int);
int i2s_new_channel(const i2s_chan_config_t *,void **,void **);
int i2s_channel_init_std_mode(void *,const i2s_std_config_t *);
int i2s_channel_enable(void *);
int i2s_channel_get_info(void *,i2s_chan_info_t *);
int i2s_channel_write(void *,const void *,size_t,size_t *,unsigned);
void *bsp_i2c_get_handle(void);
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *);
const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *);
const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *);
void *esp_codec_dev_new(esp_codec_dev_cfg_t *);
int xTaskCreateWithCaps(void (*)(void *),const char *,unsigned,void *,unsigned,void *,unsigned);
void vTaskDeleteWithCaps(void *);
void vTaskDelay(unsigned);
unsigned uxTaskGetStackHighWaterMark(void *);
#endif
