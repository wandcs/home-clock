/* Fixed CAMS/Open-Meteo hourly contract; no I/O or global timezone changes. */
#include "air_quality.h"
#include "cJSON.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const cJSON *get(const cJSON *o,const char *key){return cJSON_GetObjectItemCaseSensitive(o,key);}
static bool unit(const cJSON *units,const char *key,const char *value){
    const cJSON *u=get(units,key);return cJSON_IsString(u)&&!strcmp(u->valuestring,value);
}
static bool value(const cJSON *v,float *out){
    if(!cJSON_IsNumber(v)||!isfinite(v->valuedouble)||v->valuedouble<0||v->valuedouble>FLT_MAX)return false;
    *out=(float)v->valuedouble;return true;
}
bool air_quality_url(double lat,double lon,char *out,size_t capacity){
    if(!out||!capacity||!isfinite(lat)||!isfinite(lon)||fabs(lat)>90||fabs(lon)>180)return false;
    int n=snprintf(out,capacity,"https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%.5f&longitude=%.5f&hourly=us_aqi,pm2_5&forecast_hours=6&timeformat=unixtime&domains=cams_global",lat,lon);
    return n>0&&(size_t)n<capacity;
}
bool air_quality_valid(const air_quality_current_t *c,int64_t utc,uint64_t ms,bool trusted){
    return trusted&&c&&c->present&&isfinite(c->us_aqi)&&c->us_aqi>=0&&c->source_utc>=1577836800LL
        &&c->source_utc%3600==0&&utc>=c->source_utc&&utc-c->source_utc<3600
        &&utc>=c->fetched_utc&&utc-c->fetched_utc<=AIR_QUALITY_MAX_AGE
        &&ms>=c->fetched_ms&&ms-c->fetched_ms<=AIR_QUALITY_MAX_AGE*1000ULL;
}
bool air_quality_poor(const air_quality_current_t *c){return c&&c->present&&isfinite(c->us_aqi)&&c->us_aqi>100.0f;}
bool air_quality_select(const air_quality_cache_t *cache,int64_t utc,uint64_t ms,bool trusted,air_quality_current_t *out){
    if(!out)return false;
    *out=(air_quality_current_t){0};
    if(!cache||cache->count>AIR_QUALITY_HOURS)return false;
    for(size_t i=0;i<cache->count;i++)if(air_quality_valid(&cache->hours[i],utc,ms,trusted)){
        *out=cache->hours[i];return true;
    }
    return false;
}
weather_error_t air_quality_parse(const char *json,size_t length,int64_t fetched_utc,uint64_t fetched_ms,air_quality_cache_t *out){
    if(!json||!length||length>AIR_QUALITY_BODY_LIMIT)return WX_TOO_LARGE;
    if(!out)return WX_SCHEMA;
    int depth=0,nodes=0;bool quoted=false,escaped=false;
    for(size_t i=0;i<length;i++){
        char c=json[i];if(!c)return WX_JSON;
        if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>8||++nodes>256)return WX_JSON;}
        else if(c=='}'||c==']'){if(--depth<0)return WX_JSON;}
        else if(c==','||c==':'){if(++nodes>256)return WX_JSON;}
    }
    if(depth||quoted)return WX_JSON;
    const char *end=NULL;cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,false);
    if(!root)return WX_JSON;
    while(end&&end<json+length&&strchr(" \t\r\n",*end))end++;
    weather_error_t result=WX_SCHEMA;air_quality_cache_t parsed={0};
    if(!end||end!=json+length||!cJSON_IsObject(root)){result=WX_JSON;goto done;}
    const cJSON *offset=get(root,"utc_offset_seconds"),*hours=get(root,"hourly"),*units=get(root,"hourly_units");
    const cJSON *times=get(hours,"time"),*aqi=get(hours,"us_aqi"),*pm=get(hours,"pm2_5");
    if(get(root,"error")||!cJSON_IsNumber(offset)||offset->valuedouble!=0
        ||!unit(units,"time","unixtime")||!unit(units,"us_aqi","USAQI")
        ||!cJSON_IsArray(times)||cJSON_GetArraySize(times)!=(int)AIR_QUALITY_HOURS
        ||!cJSON_IsArray(aqi)||cJSON_GetArraySize(aqi)!=(int)AIR_QUALITY_HOURS)goto done;
    bool pm_ok=unit(units,"pm2_5","μg/m³")&&cJSON_IsArray(pm)&&cJSON_GetArraySize(pm)==(int)AIR_QUALITY_HOURS;
    int64_t previous=0;unsigned valid=0;
    for(size_t i=0;i<AIR_QUALITY_HOURS;i++){
        const cJSON *time=cJSON_GetArrayItem(times,(int)i);
        if(!cJSON_IsNumber(time)||!isfinite(time->valuedouble)||floor(time->valuedouble)!=time->valuedouble
            ||time->valuedouble<1577836800.0||time->valuedouble>4133980800.0)goto done;
        int64_t source=(int64_t)time->valuedouble;
        if(source%3600||(i&&source!=previous+3600))goto done;
        previous=source;air_quality_current_t *c=&parsed.hours[i];
        c->source_utc=source;c->fetched_utc=fetched_utc;c->fetched_ms=fetched_ms;
        c->present=value(cJSON_GetArrayItem(aqi,(int)i),&c->us_aqi);if(c->present)valid++;
        if(pm_ok)c->pm25_present=value(cJSON_GetArrayItem(pm,(int)i),&c->pm25_ug_m3);
    }
    if(!valid)goto done;
    parsed.count=AIR_QUALITY_HOURS;*out=parsed;result=valid==AIR_QUALITY_HOURS?WX_OK:WX_PARTIAL;
done:cJSON_Delete(root);return result;
}
