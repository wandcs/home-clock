#include "weather_schedule.h"
void weather_schedule_location(weather_schedule_t *s,bool configured,uint32_t generation,uint64_t now) {
    s->configured=configured; s->generation=generation; s->due_ms=now; s->failures=0;
    /* Keep the old flight until it exits, even when its location is invalidated. */
}
bool weather_schedule_claim(weather_schedule_t *s,uint64_t now,bool online,bool trusted) {
    if(!s->configured || s->in_flight || !online || !trusted || now<s->due_ms) return false;
    s->in_flight=true; s->flight_generation=s->generation; return true;
}
void weather_schedule_finish(weather_schedule_t *s,uint32_t generation,uint64_t now,bool success) {
    if(!s->in_flight || s->flight_generation!=generation) return;
    s->in_flight=false;
    if(generation!=s->generation) return;
    if(success) { s->failures=0; s->due_ms=now+WEATHER_REFRESH_MS; }
    else {
        if(s->failures<3) s->failures++;
        static const uint64_t delay[]={0,300000,900000,1800000};
        s->due_ms=now+delay[s->failures];
    }
}
