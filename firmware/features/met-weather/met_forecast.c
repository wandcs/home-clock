/* MET compact contract only: bounded JSON validation and short hourly cache. */
#include "met_forecast.h"
#include "cJSON.h"
#include <math.h>
#include <string.h>

#define JSON_DEPTH 20u
#define JSON_VALUES 16384u
#define OBJECT_VALUES 256u

typedef enum { PATH_OTHER, PATH_ROOT, PATH_PROPERTIES, PATH_SERIES } path_t;
typedef struct {
    const char *json;
    size_t length, pos, values, series_items;
    unsigned root_seen, properties_seen;
    int64_t utc, previous_time;
    bool temperature_unit, wind_unit;
    weather_error_t error;
    met_forecast_t forecast;
} reader_t;

static void space(reader_t *r) {
    while (r->pos < r->length && r->json[r->pos] && strchr(" \r\n\t", r->json[r->pos])) r->pos++;
}
static int hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static bool unicode_escape(reader_t *r, unsigned *code) {
    if (r->length - r->pos < 4) return false;
    unsigned n = 0;
    for (unsigned i = 0; i < 4; i++) {
        int h = hex((unsigned char)r->json[r->pos++]);
        if (h < 0) return false;
        n = n * 16u + (unsigned)h;
    }
    *code = n; return true;
}
/* Validate UTF-8 and JSON escapes, decoding only short ASCII field names. */
static bool string(reader_t *r, char *key, size_t capacity) {
    if (r->pos >= r->length || r->json[r->pos++] != '"') return false;
    size_t used = 0; bool short_ascii = true;
    while (r->pos < r->length) {
        unsigned c = (unsigned char)r->json[r->pos++];
        if (c == '"') {
            if (key) key[short_ascii ? used : 0] = 0;
            return true;
        }
        if (c < 0x20) return false;
        if (c == '\\') {
            if (r->pos == r->length) return false;
            c = (unsigned char)r->json[r->pos++];
            if (c == 'u') {
                if (!unicode_escape(r, &c)) return false;
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (r->length - r->pos < 6 || r->json[r->pos] != '\\' || r->json[r->pos + 1] != 'u') return false;
                    r->pos += 2; unsigned low;
                    if (!unicode_escape(r, &low) || low < 0xdc00 || low > 0xdfff) return false;
                    c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff) return false;
            } else {
                const char *codes = "\"\\/bfnrt", *at = strchr(codes, (int)c);
                static const unsigned decoded[] = {'"', '\\', '/', '\b', '\f', '\n', '\r', '\t'};
                if (!c || !at) return false;
                c = decoded[at - codes];
            }
        } else if (c >= 0x80) {
            unsigned n, min;
            if (c >= 0xc2 && c <= 0xdf) { n = 1; min = 0x80; c &= 0x1f; }
            else if (c >= 0xe0 && c <= 0xef) { n = 2; min = 0x800; c &= 0x0f; }
            else if (c >= 0xf0 && c <= 0xf4) { n = 3; min = 0x10000; c &= 7; }
            else return false;
            if (r->length - r->pos < n) return false;
            for (unsigned i = 0; i < n; i++) {
                unsigned next = (unsigned char)r->json[r->pos++];
                if ((next & 0xc0) != 0x80) return false;
                c = (c << 6) | (next & 0x3f);
            }
            if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
        }
        /* cJSON uses NUL-terminated decoded strings: reject embedded NUL rather
         * than accepting a truncated contract name or timestamp. */
        if (!c) return false;
        if (c >= 0x80 || !capacity || used + 1 >= capacity) short_ascii = false;
        if (key && short_ascii) key[used++] = (char)c;
    }
    return false;
}
static bool numeric(reader_t *r) {
    size_t p = r->pos;
    if (p < r->length && r->json[p] == '-') p++;
    if (p == r->length) return false;
    if (r->json[p] == '0') p++;
    else {
        if (r->json[p] < '1' || r->json[p] > '9') return false;
        do { p++; } while (p < r->length && r->json[p] >= '0' && r->json[p] <= '9');
    }
    if (p < r->length && r->json[p] == '.') {
        p++; size_t begin = p;
        while (p < r->length && r->json[p] >= '0' && r->json[p] <= '9') p++;
        if (p == begin) return false;
    }
    if (p < r->length && (r->json[p] == 'e' || r->json[p] == 'E')) {
        p++; if (p < r->length && (r->json[p] == '+' || r->json[p] == '-')) p++;
        size_t begin = p;
        while (p < r->length && r->json[p] >= '0' && r->json[p] <= '9') p++;
        if (p == begin) return false;
    }
    r->pos = p; return true;
}
static bool date_utc(const char *s, int64_t *out) {
    if (!s || strlen(s) != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
    for (unsigned i = 0; i < 19; i++)
        if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && (s[i] < '0' || s[i] > '9')) return false;
    int year = (s[0]-'0')*1000+(s[1]-'0')*100+(s[2]-'0')*10+s[3]-'0';
    unsigned month = (unsigned)((s[5]-'0')*10+s[6]-'0'), day = (unsigned)((s[8]-'0')*10+s[9]-'0');
    unsigned hour = (unsigned)((s[11]-'0')*10+s[12]-'0'), minute = (unsigned)((s[14]-'0')*10+s[15]-'0'), second = (unsigned)((s[17]-'0')*10+s[18]-'0');
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    bool leap = year%4 == 0 && (year%100 != 0 || year%400 == 0);
    if (year < 2020 || year > 2100 || month < 1 || month > 12 || day < 1 || day > days[month-1] + (month == 2 && leap) || hour > 23 || minute > 59 || second > 59) return false;
    year -= month <= 2; int era = year/400; unsigned y = (unsigned)(year-era*400);
    unsigned doy = (153*(month+(month>2?-3:9))+2)/5+day-1;
    *out = ((int64_t)era*146097+y*365+y/4-y/100+doy-719468)*86400+hour*3600+minute*60+second;
    return true;
}
static cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static bool unit(const cJSON *units, const char *key, const char *expected) {
    const cJSON *v = get(units, key);
    return cJSON_IsString(v) && strcmp(v->valuestring, expected) == 0;
}
static bool number(const cJSON *v, double min, double max, float *out) {
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < min || v->valuedouble > max) return false;
    *out = (float)v->valuedouble; return true;
}
static bool unique_tree(const cJSON *v) {
    if (cJSON_IsObject(v)) for (const cJSON *a = v->child; a; a = a->next)
        for (const cJSON *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string)) return false;
    for (const cJSON *c = v->child; c; c = c->next) if (!unique_tree(c)) return false;
    return true;
}
/* Representative codes for the existing eight display categories, not an
 * exact conversion of MET phenomena to WMO observations. Sleet stays in the
 * rain display class rather than being mislabeled as freezing rain (66/67).
 * Day/night/polar geometry stays owned by the clock's local solar calculation.
 * Official MET symbol IDs:
 * https://github.com/metno/weathericons/blob/main/weather/README.md */
static bool symbol(const cJSON *v, int *code) {
    if (!cJSON_IsString(v)) return false;
    const char *s = v->valuestring; size_t len = strlen(s);
    const char *underscore = strrchr(s, '_');
    if (underscore) {
        if (strcmp(underscore,"_day") && strcmp(underscore,"_night") && strcmp(underscore,"_polartwilight")) return false;
        len = (size_t)(underscore-s);
    }
    static const struct { const char *name; int code; } map[] = {
        {"clearsky",0},{"fair",1},{"partlycloudy",2},{"cloudy",3},{"fog",45},
        {"lightrain",61},{"rain",63},{"heavyrain",65},
        {"lightrainshowers",80},{"rainshowers",81},{"heavyrainshowers",82},
        {"lightsleet",63},{"sleet",63},{"heavysleet",65},
        {"lightsleetshowers",63},{"sleetshowers",63},{"heavysleetshowers",65},
        {"lightsnow",71},{"snow",73},{"heavysnow",75},
        {"lightsnowshowers",85},{"snowshowers",85},{"heavysnowshowers",86},
        {"lightrainandthunder",95},{"rainandthunder",95},{"heavyrainandthunder",95},
        {"lightrainshowersandthunder",95},{"rainshowersandthunder",95},{"heavyrainshowersandthunder",95},
        {"lightsleetandthunder",95},{"sleetandthunder",95},{"heavysleetandthunder",95},
        {"lightssleetshowersandthunder",95},{"sleetshowersandthunder",95},{"heavysleetshowersandthunder",95},
        {"lightsnowandthunder",95},{"snowandthunder",95},{"heavysnowandthunder",95},
        {"lightssnowshowersandthunder",95},{"snowshowersandthunder",95},{"heavysnowshowersandthunder",95}
    };
    for (unsigned i=0; i<sizeof map/sizeof map[0]; i++)
        if (strlen(map[i].name)==len && !memcmp(s,map[i].name,len)) { *code=map[i].code; return true; }
    return false;
}
static bool small_object(reader_t *r, size_t start, size_t values, bool meta) {
    if (r->pos-start > MET_FORECAST_OBJECT_LIMIT || r->values-values > OBJECT_VALUES) { r->error=WX_TOO_LARGE; return false; }
    const char *end = NULL;
    cJSON *object = cJSON_ParseWithLengthOpts(r->json+start, r->pos-start, &end, false);
    if (!object || end != r->json+r->pos || !cJSON_IsObject(object) || !unique_tree(object)) {
        cJSON_Delete(object); r->error=WX_SCHEMA; return false;
    }
    bool ok = true;
    if (meta) {
        const cJSON *updated = get(object,"updated_at"), *units = get(object,"units");
        ok = cJSON_IsString(updated) && date_utc(updated->valuestring,&r->forecast.model_updated_utc)
            && r->forecast.model_updated_utc <= r->utc+300 && cJSON_IsObject(units);
        r->temperature_unit = unit(units,"air_temperature","celsius");
        r->wind_unit = unit(units,"wind_speed","m/s");
    } else {
        const cJSON *time = get(object,"time"), *data = get(object,"data"); int64_t at;
        ok = cJSON_IsString(time) && date_utc(time->valuestring,&at) && at>r->previous_time && cJSON_IsObject(data);
        if (ok) {
            r->previous_time=at;
            const cJSON *details=get(get(data,"instant"),"details");
            const cJSON *summary=get(get(data,"next_1_hours"),"summary");
            met_point_t point={.source_utc=at,.code=999};
            const cJSON *symbol_value=get(summary,"symbol_code");
            bool usable = at<=r->utc+(int64_t)MET_FORECAST_POINTS*3600-1 && at+3600>r->utc
                && number(get(details,"air_temperature"),-100,100,&point.temperature_c)
                && cJSON_IsString(symbol_value) && symbol_value->valuestring[0];
            /* Unknown future symbols retain a valid temperature with unknown
             * weather; a missing hourly period never borrows a 6-hour symbol. */
            if (usable) (void)symbol(symbol_value,&point.code);
            if (usable && r->forecast.count<MET_FORECAST_POINTS) {
                point.wind_present=number(get(details,"wind_speed"),0,150,&point.wind_mps);
                if (r->forecast.count && at<r->forecast.points[r->forecast.count-1].source_utc+3600) ok=false;
                else r->forecast.points[r->forecast.count++]=point;
            }
        }
    }
    cJSON_Delete(object);
    if (!ok) r->error=WX_SCHEMA;
    return ok;
}
static bool value(reader_t *r, unsigned depth, path_t path);
static bool object(reader_t *r, unsigned depth, path_t path) {
    r->pos++; space(r);
    if (r->pos<r->length && r->json[r->pos]=='}') { r->pos++; return true; }
    for (;;) {
        char key[32]; if (!string(r,key,sizeof key)) return false;
        space(r); if (r->pos==r->length || r->json[r->pos++]!=':') return false;
        space(r); size_t begin=r->pos, before=r->values;
        path_t next=PATH_OTHER; unsigned bit=0; bool meta=false;
        if (path==PATH_ROOT && !strcmp(key,"properties")) { next=PATH_PROPERTIES; bit=1; }
        if (path==PATH_ROOT && !strcmp(key,"type")) bit=2;
        if (path==PATH_PROPERTIES && !strcmp(key,"meta")) { bit=1; meta=true; }
        if (path==PATH_PROPERTIES && !strcmp(key,"timeseries")) { next=PATH_SERIES; bit=2; }
        unsigned *seen=path==PATH_ROOT?&r->root_seen:&r->properties_seen;
        if (bit && (*seen&bit)) { r->error=WX_SCHEMA; return false; }
        *seen|=bit;
        if (r->pos==r->length) return false;
        if ((next==PATH_PROPERTIES || meta) && r->json[r->pos]!='{') { r->error=WX_SCHEMA; return false; }
        if (next==PATH_SERIES && r->json[r->pos]!='[') { r->error=WX_SCHEMA; return false; }
        if (!value(r,depth+1,next)) return false;
        if (meta && !small_object(r,begin,before,true)) return false;
        if (path==PATH_ROOT && bit==2) {
            size_t saved=r->pos; char type[16]; r->pos=begin;
            bool feature=string(r,type,sizeof type) && !strcmp(type,"Feature") && r->pos==saved;
            r->pos=saved;
            if (!feature) { r->error=WX_SCHEMA; return false; }
        }
        space(r); if (r->pos==r->length) return false;
        char c=r->json[r->pos++];
        if (c=='}') return true;
        if (c!=',') return false;
        space(r);
    }
}
static bool array(reader_t *r, unsigned depth, path_t path) {
    r->pos++; space(r);
    if (r->pos<r->length && r->json[r->pos]==']') { r->pos++; return true; }
    for (;;) {
        size_t begin=r->pos, before=r->values;
        if (path==PATH_SERIES) {
            if (++r->series_items>1024) { r->error=WX_TOO_LARGE; return false; }
            if (r->pos==r->length || r->json[r->pos]!='{') { r->error=WX_SCHEMA; return false; }
        }
        if (!value(r,depth+1,PATH_OTHER)) return false;
        if (path==PATH_SERIES && !small_object(r,begin,before,false)) return false;
        space(r); if (r->pos==r->length) return false;
        char c=r->json[r->pos++];
        if (c==']') return true;
        if (c!=',') return false;
        space(r);
    }
}
static bool value(reader_t *r, unsigned depth, path_t path) {
    if (depth>JSON_DEPTH || ++r->values>JSON_VALUES) { r->error=WX_TOO_LARGE; return false; }
    if (r->pos==r->length) return false;
    char c=r->json[r->pos];
    if (c=='{') return object(r,depth,path);
    if (c=='[') return array(r,depth,path);
    if (c=='"') return string(r,NULL,0);
    if (c=='-' || (c>='0' && c<='9')) return numeric(r);
    const char *literal = c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
    if (!literal || r->length-r->pos<strlen(literal) || memcmp(r->json+r->pos,literal,strlen(literal))) return false;
    r->pos+=strlen(literal); return true;
}
weather_error_t met_forecast_parse(const char *json, size_t length, int64_t utc, met_forecast_t *out) {
    if (!json || !out || !length || utc<1577836800LL || utc>4133980800LL) return WX_SCHEMA;
    if (length>MET_FORECAST_BODY_LIMIT) return WX_TOO_LARGE;
    reader_t r={.json=json,.length=length,.utc=utc,.error=WX_JSON};
    space(&r);
    if (r.pos==r.length || r.json[r.pos]!='{' || !value(&r,0,PATH_ROOT)) return r.error;
    space(&r); if (r.pos!=length) return WX_JSON;
    if (r.root_seen!=3 || r.properties_seen!=3 || !r.temperature_unit || !r.forecast.count) return WX_SCHEMA;
    if (!r.wind_unit) for (size_t i=0;i<r.forecast.count;i++) { r.forecast.points[i].wind_present=false; r.forecast.points[i].wind_mps=0; }
    *out=r.forecast; return WX_OK;
}
bool met_forecast_select(const met_forecast_t *forecast, int64_t utc, int64_t fetched_utc,
                         uint64_t fetched_ms, weather_current_t *out) {
    if (!forecast || !out || !forecast->count || forecast->count>MET_FORECAST_POINTS || utc<1577836800LL || utc>4133980800LL
        || fetched_utc<=0 || fetched_utc>utc+300 || utc-fetched_utc>WEATHER_CURRENT_MAX_AGE
        || forecast->model_updated_utc<=0 || forecast->model_updated_utc>utc+300) return false;
    for (size_t i=0;i<forecast->count;i++) {
        const met_point_t *p=&forecast->points[i];
        if (p->source_utc<1577836800LL || p->source_utc>4133980800LL || p->source_utc>utc || utc>=p->source_utc+3600) continue;
        if (!isfinite(p->temperature_c) || p->temperature_c < -100 || p->temperature_c>100 || p->code<0 || p->code>9999) return false;
        weather_current_t c={.present=true,.source_utc=p->source_utc,.interval_s=3600,
            .code=p->code,.temperature_c=p->temperature_c,.fetched_utc=fetched_utc,.fetched_ms=fetched_ms};
        c.wind_present=p->wind_present && isfinite(p->wind_mps) && p->wind_mps>=0 && p->wind_mps<=150;
        if (c.wind_present) c.wind_mps=p->wind_mps;
        *out=c; return true;
    }
    return false;
}
