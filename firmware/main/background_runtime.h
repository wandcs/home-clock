#pragma once
#include "weather_background.h"
#include "lvgl.h"
/* Start outside display lock. Submit only under it. Two buffers, one worker. */
bool background_runtime_start(void);
void background_runtime_attach(lv_obj_t *parent);
void background_runtime_submit(const wb_frame_t *frame,bool visible);
uint32_t background_runtime_frames(void);
