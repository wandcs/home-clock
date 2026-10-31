/* Provider contract only: no UI, I/O, global TZ changes or persistence. */
#include "weather_data.h"
#include "cJSON.h"
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

bool weather_url(double lat,double lon,const char *timezone,char *out,size_t capacity) {
    if(!timezone||!isfinite(lat)||!isfinite(lon)||fabs(lat)>90||fabs(lon)>180)return false;
    size_t n=strnlen(timezone,WEATHER_ZONE_BYTES),used=0;
    if(!n||n==WEATHER_ZONE_BYTES)return false;
    char encoded[WEATHER_ZONE_BYTES*3];const char *hex="0123456789ABCDEF";
    for(size_t i=0;i<n;i++){
        unsigned c=(unsigned char)timezone[i];
        if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-'||c=='+'))return false;
        if(c=='/'||c=='+'){encoded[used++]='%';encoded[used++]=hex[c>>4];encoded[used++]=hex[c&15];}
        else encoded[used++]=(char)c;
    }
    encoded[used]=0;
    int written=snprintf(out,capacity,"https://api.open-meteo.com/v1/forecast?latitude=%.5f&longitude=%.5f&current=temperature_2m,weather_code,wind_speed_10m&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,wind_speed_10m_max,uv_index_max&forecast_days=7&temperature_unit=celsius&wind_speed_unit=ms&timezone=%s&timeformat=iso8601",lat,lon,encoded);
    return written>0&&(size_t)written<capacity;
}

weather_kind_t weather_kind(int c) {
    switch(c) {
    case 0: case 1: return WX_CLEAR;
    case 2: return WX_CLOUD;
    case 3: return WX_OVERCAST;
    case 45: case 48: return WX_FOG;
    case 51: case 53: case 55: case 56: case 57: case 61: case 63: case 65:
    case 66: case 67: case 80: case 81: case 82: return WX_RAIN;
    case 71: case 73: case 75: case 77: case 85: case 86: return WX_SNOW;
    case 95: case 96: case 97: case 99: return WX_THUNDER;
    default: return WX_UNKNOWN;
    }
}
const char *weather_kind_name(int c) {
    static const char *names[]={"unknown","clear","cloud","overcast","fog","rain","snow","thunder"};
    return names[weather_kind(c)];
}
const char *weather_error_name(weather_error_t e) {
    static const char *names[]={"ok","json","schema","partial","http","transport","too_large","truncated","timeout","offline","clock_unknown","unconfigured"};
    return e>=WX_OK && e<=WX_UNCONFIGURED?names[e]:"unknown";
}
const char *weather_provider_name(weather_provider_t p) {
    return p==WX_PROVIDER_OPEN_METEO?"Open-Meteo":p==WX_PROVIDER_MET?"MET Norway":"unknown";
}
static cJSON *get(const cJSON *o,const char *k) { return cJSON_GetObjectItemCaseSensitive(o,k); }
static bool number(const cJSON *v,double min,double max,double *n) {
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<min || v->valuedouble>max) return false;
    *n=v->valuedouble; return true;
}
static bool integer(const cJSON *v,int min,int max,int *n) {
    double d; if(!number(v,min,max,&d) || floor(d)!=d) return false;
    *n=(int)d; return true;
}
static bool text(const cJSON *v,char *out,size_t cap) {
    if(!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring)>=cap) return false;
    /* Metadata goes into development JSON: accept only safe timezone tokens. */
    for(const char *p=v->valuestring;*p;p++) if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("/_+-:",*p))) return false;
    strcpy(out,v->valuestring); return true;
}
static bool unit(const cJSON *u,const char *k,const char *expected) {
    cJSON *v=get(u,k); return cJSON_IsString(v) && strcmp(v->valuestring,expected)==0;
}
static bool date_parts(const char *s,int *y,int *m,int *d) {
    if(!s || strlen(s)<10 || s[4]!='-' || s[7]!='-') return false;
    for(int i=0;i<10;i++) if(i!=4 && i!=7 && (s[i]<'0'||s[i]>'9')) return false;
    *y=(s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
    *m=(s[5]-'0')*10+s[6]-'0'; *d=(s[8]-'0')*10+s[9]-'0';
    static const int month[]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(*y<2020 || *y>2100 || *m<1 || *m>12) return false;
    int max=month[*m-1]+(*m==2 && *y%4==0 && (*y%100!=0 || *y%400==0));
    return *d>=1 && *d<=max;
}
/* Gregorian civil date to UTC seconds, independent of device TZ. */
static int64_t civil(int y,int m,int d) {
    y-=m<=2; int era=y/400; unsigned yo=(unsigned)(y-era*400);
    unsigned doy=(153*(m+(m>2?-3:9))+2)/5+d-1;
    return ((int64_t)era*146097+yo*365+yo/4-yo/100+doy-719468)*86400;
}
static bool source_time(const cJSON *v,int offset,int64_t *out) {
    if(!cJSON_IsString(v)) return false;
    const char *s=v->valuestring; int y,m,d;
    if(strlen(s)!=16 || !date_parts(s,&y,&m,&d) || s[10]!='T' || s[13]!=':') return false;
    for(int i=11;i<16;i++) if(i!=13 && (s[i]<'0'||s[i]>'9')) return false;
    int h=(s[11]-'0')*10+s[12]-'0',min=(s[14]-'0')*10+s[15]-'0';
    if(h>23 || min>59) return false;
    *out=civil(y,m,d)+h*3600+min*60-offset; return true;
}
bool weather_local_date(int64_t utc,int offset,char date[11]) {
    time_t t=utc+offset; struct tm tm;
    return gmtime_r(&t,&tm) && strftime(date,11,"%Y-%m-%d",&tm)==10;
}
bool weather_current_valid(const weather_current_t *c,int64_t utc,uint64_t ms,bool trusted) {
    return trusted && c->present && utc>=c->source_utc-300 && utc-c->source_utc<=WEATHER_CURRENT_MAX_AGE
        && (c->provider!=WX_PROVIDER_MET || (utc>=c->source_utc && c->interval_s==3600 && utc-c->source_utc<3600))
        && utc>=c->fetched_utc-300 && utc-c->fetched_utc<=WEATHER_CURRENT_MAX_AGE
        && ms>=c->fetched_ms && ms-c->fetched_ms<=WEATHER_CURRENT_MAX_AGE*1000ULL;
}
bool weather_day_valid(const weather_day_t *d,int64_t utc,uint64_t ms,bool trusted) {
    return trusted && d->present && utc>=d->fetched_utc-300 && utc-d->fetched_utc<=WEATHER_DAILY_MAX_AGE
        && ms>=d->fetched_ms && ms-d->fetched_ms<=WEATHER_DAILY_MAX_AGE*1000ULL;
}
int weather_wind_grade(float mps) {
    static const uint16_t lower_deci[]={0,3,16,34,55,80,108,139,172,208,245,285,327,370,415,462,510,561};
    if(!isfinite(mps)||mps<0||mps>150)return -1;
    /* Quantize each decimal half-step once into the input float domain.
     * That canonical float and its upper neighbour enter the new grade;
     * nextafterf toward -infinity stays below. No epsilon/double rounding. */
    for(int grade=17;grade>0;grade--)
        if(mps>=(float)(((double)lower_deci[grade]-0.5)/10.0))return grade;
    return 0;
}
bool weather_uv_strong(const weather_day_t *day) {
    return day && day->present && day->uv_present && isfinite(day->uv_index)
        && day->uv_index>=6.0f;
}
void weather_forecast_summary(const weather_data_t *data,int64_t utc,uint64_t ms,
    bool trusted,weather_forecast_t *out) {
    if(!out)return;
    memset(out,0,sizeof *out);
    for(int i=0;i<WEATHER_DAYS;i++)out->slots[i].data_index=-1;
    if(!data||!trusted||!data->timezone[0])return;
    out->time_known=true;
    for(int i=0;i<WEATHER_DAYS;i++) {
        weather_forecast_slot_t *slot=&out->slots[i];
        if(!weather_local_date(utc+i*86400LL,data->offset_s,slot->date)){
            /* Do not claim a known date or weekday when conversion fails. */
            memset(out,0,sizeof *out);
            for(int j=0;j<WEATHER_DAYS;j++)out->slots[j].data_index=-1;
            return;
        }
        slot->state=WEATHER_SLOT_MISSING;
        for(int j=0;j<WEATHER_DAYS;j++)if(data->days[j].present&&!strcmp(data->days[j].date,slot->date)){
            const weather_day_t *day=&data->days[j];slot->data_index=j;
            slot->state=weather_day_valid(day,utc,ms,true)?WEATHER_SLOT_USABLE:WEATHER_SLOT_EXPIRED;
            break;
        }
        if(slot->state==WEATHER_SLOT_MISSING)out->missing_count++;
        else if(slot->state==WEATHER_SLOT_EXPIRED)out->expired_count++;
        else {
            const weather_day_t *day=&data->days[slot->data_index];
            if(!out->usable_count){
                out->oldest_fetched_ms=out->newest_fetched_ms=day->fetched_ms;
                out->oldest_fetched_utc=out->newest_fetched_utc=day->fetched_utc;
            }else {
                if(day->fetched_ms!=out->oldest_fetched_ms)out->mixed=true;
                if(day->fetched_ms<out->oldest_fetched_ms){out->oldest_fetched_ms=day->fetched_ms;out->oldest_fetched_utc=day->fetched_utc;}
                if(day->fetched_ms>out->newest_fetched_ms){out->newest_fetched_ms=day->fetched_ms;out->newest_fetched_utc=day->fetched_utc;}
            }
            out->usable_count++;
        }
    }
}
const weather_day_t *weather_day_at(const weather_data_t *data,int slot,int64_t utc,uint64_t ms,bool trusted,char date[11]) {
    date[0]=0;
    if(!trusted||!data->timezone[0]||slot<0||slot>=7||!weather_local_date(utc+slot*86400LL,data->offset_s,date))return NULL;
    for(int i=0;i<7;i++)if(!strcmp(data->days[i].date,date)&&weather_day_valid(&data->days[i],utc,ms,trusted))return &data->days[i];
    return NULL;
}
weather_error_t weather_parse(const char *json,size_t length,int64_t utc,uint64_t ms,weather_data_t *out) {
    if(!json || !length || length>WEATHER_BODY_LIMIT) return WX_TOO_LARGE;
    /* Keep cJSON recursion and allocations bounded even for hostile documents. */
    int depth=0,nodes=0; bool quoted=false,escaped=false;
    for(size_t i=0;i<length;i++) {
        char c=json[i]; if(!c) return WX_JSON;
        if(quoted) { if(escaped) escaped=false; else if(c=='\\') escaped=true; else if(c=='"') quoted=false; }
        else if(c=='"') quoted=true;
        else if(c=='{'||c=='[') { if(++depth>8 || ++nodes>512) return WX_JSON; }
        else if(c=='}'||c==']') { if(--depth<0) return WX_JSON; }
        else if(c==',' || c==':') { if(++nodes>512) return WX_JSON; }
    }
    if(depth || quoted) return WX_JSON;
    const char *end=NULL;
    cJSON *root=cJSON_ParseWithLengthOpts(json,length,&end,false);
    if(!root) return WX_JSON;
    while(end<json+length && (*end==' '||*end=='\n'||*end=='\r'||*end=='\t')) end++;
    weather_error_t result=WX_SCHEMA; weather_data_t p={0}; double lat,lon;
    if(end!=json+length || !cJSON_IsObject(root)) { result=WX_JSON; goto done; }
    if(get(root,"error") || !text(get(root,"timezone"),p.timezone,sizeof p.timezone)
        || !text(get(root,"timezone_abbreviation"),p.abbreviation,sizeof p.abbreviation)
        || !integer(get(root,"utc_offset_seconds"),-43200,50400,&p.offset_s)
        || !number(get(root,"latitude"),-90,90,&lat) || !number(get(root,"longitude"),-180,180,&lon)) goto done;
    p.grid_latitude=lat; p.grid_longitude=lon; p.grid_present=true;
    cJSON *c=get(root,"current"),*cu=get(root,"current_units"); double temp;
    if(unit(cu,"temperature_2m","°C") && unit(cu,"weather_code","wmo code") && unit(cu,"time","iso8601") && unit(cu,"interval","seconds")
        && source_time(get(c,"time"),p.offset_s,&p.current.source_utc)
        && integer(get(c,"interval"),1,86400,&p.current.interval_s)
        && integer(get(c,"weather_code"),0,9999,&p.current.code) && number(get(c,"temperature_2m"),-100,100,&temp)) {
        p.current.temperature_c=temp; p.current.present=true; p.current.fetched_utc=utc; p.current.fetched_ms=ms;
        p.current.provider=WX_PROVIDER_OPEN_METEO;
        double wind;
        if(unit(cu,"wind_speed_10m","m/s")&&number(get(c,"wind_speed_10m"),0,150,&wind)){
            p.current.wind_present=true;p.current.wind_mps=(float)wind;
        }
    }
    cJSON *daily=get(root,"daily"),*du=get(root,"daily_units");
    const char *keys[]={"time","weather_code","temperature_2m_max","temperature_2m_min"}; cJSON *a[4]; bool arrays=true;
    for(int i=0;i<4;i++) { a[i]=get(daily,keys[i]); if(!cJSON_IsArray(a[i])||cJSON_GetArraySize(a[i])!=7) arrays=false; }
    int valid=0; int64_t previous=0;
    if(arrays) for(int i=0;i<7;i++) {
        cJSON *dt=cJSON_GetArrayItem(a[0],i); int y,m,d;
        if(!cJSON_IsString(dt) || strlen(dt->valuestring)!=10 || !date_parts(dt->valuestring,&y,&m,&d)) { arrays=false; break; }
        int64_t day=civil(y,m,d);
        if(i && day!=previous+86400) { arrays=false; break; }
        previous=day;
    }
    if(arrays && unit(du,"time","iso8601") && unit(du,"weather_code","wmo code") && unit(du,"temperature_2m_max","°C") && unit(du,"temperature_2m_min","°C")) {
        for(int i=0;i<7;i++) {
            cJSON *dt=cJSON_GetArrayItem(a[0],i); int y,m,d; double hi,lo;
            if(!cJSON_IsString(dt) || strlen(dt->valuestring)!=10 || !date_parts(dt->valuestring,&y,&m,&d)) continue;
            strcpy(p.days[i].date,dt->valuestring);
            double probability;
            cJSON *pa=get(daily,"precipitation_probability_max");
            if(unit(du,"precipitation_probability_max","%") && cJSON_IsArray(pa) && cJSON_GetArraySize(pa)==7
                && number(cJSON_GetArrayItem(pa,i),0,100,&probability)) {
                p.days[i].precipitation_present=true; p.days[i].precipitation_pct=probability;
            }
            if(integer(cJSON_GetArrayItem(a[1],i),0,9999,&p.days[i].code)
                && number(cJSON_GetArrayItem(a[2],i),-100,100,&hi) && number(cJSON_GetArrayItem(a[3],i),-100,100,&lo) && hi>=lo) {
                p.days[i].high_c=hi; p.days[i].low_c=lo; p.days[i].present=true;
                p.days[i].fetched_utc=utc; p.days[i].fetched_ms=ms; p.days[i].provider=WX_PROVIDER_OPEN_METEO; valid++;
                cJSON *wa=get(daily,"wind_speed_10m_max");double wind;
                if(unit(du,"wind_speed_10m_max","m/s")&&cJSON_IsArray(wa)&&cJSON_GetArraySize(wa)==7
                    &&number(cJSON_GetArrayItem(wa,i),0,150,&wind)){
                    p.days[i].wind_present=true;p.days[i].wind_mps=(float)wind;
                }
                /* The provider serializes a dimensionless index with an empty
                 * unit. Optional UV never invalidates otherwise usable weather.
                 * FLT_MAX limits representation, not the physical UV scale. */
                cJSON *ua=get(daily,"uv_index_max");double uv;
                if(unit(du,"uv_index_max","")&&cJSON_IsArray(ua)&&cJSON_GetArraySize(ua)==7
                    &&number(cJSON_GetArrayItem(ua,i),0,FLT_MAX,&uv)){
                    p.days[i].uv_present=true;p.days[i].uv_index=(float)uv;
                }
            }
        }
    }
    if(!p.current.present && !valid) goto done;
    *out=p; result=p.current.present && valid==7?WX_OK:WX_PARTIAL;
done:
    cJSON_Delete(root); return result;
}
void weather_merge(weather_data_t *cache,const weather_data_t *fresh) {
    /* A usable fresh day replaces its complete optional-field snapshot. A
     * missing UV value must not revive an older strong-UV cue. */
    weather_data_t result=*fresh;
    if(!fresh->timezone[0]) {
        memcpy(result.timezone,cache->timezone,sizeof result.timezone);
        memcpy(result.abbreviation,cache->abbreviation,sizeof result.abbreviation);
        result.offset_s=cache->offset_s;
        result.grid_latitude=cache->grid_latitude; result.grid_longitude=cache->grid_longitude;
        result.grid_present=cache->grid_present;
    }
    if(!fresh->current.present || (cache->current.present && fresh->current.source_utc<cache->current.source_utc)) result.current=cache->current;
    for(int i=0;i<7;i++) if(!fresh->days[i].present) {
        for(int j=0;j<7;j++) if(cache->days[j].present && strcmp(cache->days[j].date,fresh->days[i].date)==0) result.days[i]=cache->days[j];
    }
    /* Entire daily group missing: preserve old dated entries for independent age/date checks. */
    if(!fresh->days[0].date[0]) memcpy(result.days,cache->days,sizeof result.days);
    *cache=result;
}
