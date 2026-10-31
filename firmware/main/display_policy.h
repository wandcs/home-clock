#pragma once
#include "environment_app_state.h"
#include "solar_core.h"
#include "weather_background.h"
#include "weather_service.h"
#include "location_service.h"
typedef struct { env_runtime_t environment; env_output_t env; solar_model_t solar;
    solar_state_t sun; wb_state_t background; wb_frame_t frame; bool low_light; } display_policy_t;
void display_policy_init(display_policy_t *p,uint64_t now,unsigned brightness);
void display_policy_step(display_policy_t *p,const sensor_state_t *s,const network_state_t *n,
    const weather_state_t *w,const location_state_t *l,int64_t utc_ms,uint64_t now,bool visible);
