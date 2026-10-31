#include "display_policy.h"
#include <string.h>
void display_policy_init(display_policy_t *p,uint64_t now,unsigned brightness){
    memset(p,0,sizeof *p);env_runtime_init(&p->environment,now,brightness);
    /* Unknown light on a new boot is conservative black until measured. */
    p->low_light=true;wb_init(&p->background,now,true);
}
void display_policy_step(display_policy_t *p,const sensor_state_t *s,const network_state_t *n,
    const weather_state_t *w,const location_state_t *l,int64_t utc_ms,uint64_t now,bool visible){
    env_snapshot_t snapshot;
    if(env_snapshot_from_app_state(s,&snapshot)&&env_runtime_step(&p->environment,&snapshot,now,&p->env)==ENV_OK)
        if(p->env.brightness.low_light_known)p->low_light=p->env.brightness.low_light;
    /* Physical daylight is independent of civil timezone. UTC midnight provides
     * a complete astronomical window without fabricating overseas DST rules. */
    solar_input_t input={.latitude=l->saved.latitude,.longitude=l->saved.longitude,
        .now_utc_ms=utc_ms,.trusted=n->time_valid&&l->configured,
        .location_revision=l->revision,.clock_revision=n->sync_count};
    if(!solar_fixed_civil(utc_ms,0,&input.civil))input.trusted=false;
    p->sun=solar_update(&p->solar,&input);
    static const wb_weather_t kinds[]={WB_UNKNOWN,WB_SUNNY,WB_CLOUDY,WB_OVERCAST,WB_FOG,WB_RAIN,WB_SNOW,WB_THUNDER};
    wb_input_t background={.weather=kinds[weather_kind(w->data.current.code)],
        .outdoor=p->sun.phase==SOLAR_DAY?WB_DAY:p->sun.phase==SOLAR_NIGHT?WB_NIGHT:WB_OUTDOOR_UNKNOWN,
        .current_valid=weather_current_valid(&w->data.current,utc_ms/1000,now,n->time_valid),
        .low_light_known=p->env.brightness.low_light_known,.low_light=p->low_light,
        .visible=visible,.motion_enabled=true};
    (void)wb_step(&p->background,background,now,&p->frame);
}
