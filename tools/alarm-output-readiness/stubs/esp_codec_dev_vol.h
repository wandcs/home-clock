#ifndef AA_FAKE_VOL_H
#define AA_FAKE_VOL_H
typedef struct { int vol; float db_value; } esp_codec_dev_vol_map_t;
typedef struct { esp_codec_dev_vol_map_t min_vol, max_vol; } esp_codec_dev_vol_range_t;
typedef struct { float pa_voltage, codec_dac_voltage, pa_gain; } esp_codec_dev_hw_gain_t;
int esp_codec_dev_vol_calc_reg(const esp_codec_dev_vol_range_t *, float);
float esp_codec_dev_col_calc_hw_gain(esp_codec_dev_hw_gain_t *);

#endif
