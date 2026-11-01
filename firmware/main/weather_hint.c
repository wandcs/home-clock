/* Pure selection only: no UI, clock reads, requests, cache or mutable state. */
#include "weather_hint.h"
#include <math.h>

static weather_hint_t hint(weather_hint_kind_t kind) {
    return (weather_hint_t){.kind=kind,.grade=-1};
}

static weather_hint_t wind_hint(int wind,int gust,int wind_min,int gust_min) {
    /* Each value must pass its own threshold before comparing grades. */
    bool windy=wind>=wind_min,gusty=gust>=gust_min;
    if(windy && (!gusty || wind>=gust)) {
        weather_hint_t out=hint(WEATHER_HINT_WIND);out.grade=wind;return out;
    }
    if(gusty) {
        weather_hint_t out=hint(WEATHER_HINT_GUST);out.grade=gust;return out;
    }
    return hint(WEATHER_HINT_NONE);
}

/* Fixed product order shared by current and daily data, not a risk score or
 * official warning. Source codes are used directly; no inferred phenomena. */
static weather_hint_t weather_exception(int code,bool wind_present,float wind_mps,
    bool gust_present,float gust_mps) {
    int wind=wind_present?weather_wind_grade(wind_mps):-1;
    int gust=gust_present?weather_wind_grade(gust_mps):-1;
    weather_hint_t out=wind_hint(wind,gust,8,9);
    if(out.kind!=WEATHER_HINT_NONE)return out;
    switch(code) {
    case 56: case 57: case 66: case 67:
        return hint(WEATHER_HINT_FREEZING_RAIN);
    case 96: case 99: return hint(WEATHER_HINT_HAIL);
    case 95: case 97: return hint(WEATHER_HINT_THUNDER);
    case 65: case 82: return hint(WEATHER_HINT_RAIN_STRONG);
    case 75: case 86: return hint(WEATHER_HINT_SNOW_STRONG);
    default: return wind_hint(wind,gust,6,7);
    }
}

weather_hint_t weather_hint_current(const weather_current_t *current,
    const air_quality_current_t *air_quality,int64_t utc,uint64_t ms,bool trusted) {
    if(current && weather_current_valid(current,utc,ms,trusted)) {
        weather_hint_t out=weather_exception(current->code,current->wind_present,
            current->wind_mps,current->gust_present,current->gust_mps);
        if(out.kind!=WEATHER_HINT_NONE)return out;
    }
    /* An unavailable weather group must not suppress independently valid AQ. */
    if(air_quality_valid(air_quality,utc,ms,trusted) && air_quality_poor(air_quality))
        return hint(WEATHER_HINT_AIR_POOR);
    return hint(WEATHER_HINT_NONE);
}

weather_hint_t weather_hint_day(const weather_day_t *day,
    int64_t utc,uint64_t ms,bool trusted) {
    if(!day || !weather_day_valid(day,utc,ms,trusted))
        return hint(WEATHER_HINT_UNKNOWN);
    weather_hint_t out=weather_exception(day->code,day->wind_present,day->wind_mps,
        day->gust_present,day->gust_mps);
    if(out.kind!=WEATHER_HINT_NONE)return out;
    if(weather_uv_strong(day))return hint(WEATHER_HINT_UV);
    weather_kind_t kind=weather_kind(day->code);
    bool wet=kind==WX_RAIN || kind==WX_SNOW || kind==WX_THUNDER;
    bool probability=day->precipitation_present && isfinite(day->precipitation_pct)
        && day->precipitation_pct>=0 && day->precipitation_pct<=100;
    if(wet || (probability && day->precipitation_pct>0)) {
        out=hint(WEATHER_HINT_PRECIPITATION);
        out.probability_present=probability;
        if(probability)out.probability_pct=day->precipitation_pct;
        return out;
    }
    int wind=day->wind_present?weather_wind_grade(day->wind_mps):-1;
    if(wind>=0) {
        out=hint(WEATHER_HINT_WIND);out.grade=wind;return out;
    }
    return hint(WEATHER_HINT_UNKNOWN);
}
