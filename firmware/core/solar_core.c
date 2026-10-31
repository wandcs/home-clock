#include "solar_core.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(DBL_MANT_DIG >= 53, "solar core requires binary64 double precision");
#define DAY_MS INT64_C(86400000)
#define STEP_MS INT64_C(300000)
#define DEG (3.14159265358979323846 / 180.0)

static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int month_days(int y, int m) {
    static const int lengths[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return lengths[m - 1] + (m == 2 && leap(y));
}
static bool valid_date(solar_date_t d) {
    return d.year >= 1901 && d.year <= 2099 && d.month >= 1 && d.month <= 12 &&
           d.day >= 1 && d.day <= month_days(d.year, d.month);
}
static bool offset_ok(int m) { return m >= -14 * 60 && m <= 14 * 60; }
/* Bounded calendar conversion; no platform time_t or locale assumptions. */
static int64_t ordinal(solar_date_t d) {
    int64_t n = 0;
    if (d.year >= 1970) for (int y = 1970; y < d.year; ++y) n += 365 + leap(y);
    else for (int y = d.year; y < 1970; ++y) n -= 365 + leap(y);
    for (int m = 1; m < d.month; ++m) n += month_days(d.year, m);
    return n + d.day - 1;
}
static int64_t floor_day(int64_t t) {
    int64_t d = t / DAY_MS;
    return d - (t % DAY_MS < 0);
}
bool solar_civil_bounds(solar_civil_day_t c, int64_t *start, int64_t *end) {
    if (!start || !end || !valid_date(c.date) || !offset_ok(c.start_offset_minutes) ||
        !offset_ok(c.end_offset_minutes)) return false;
    int64_t base = ordinal(c.date) * DAY_MS;
    int64_t a = base - (int64_t)c.start_offset_minutes * 60000;
    int64_t b = base + DAY_MS - (int64_t)c.end_offset_minutes * 60000;
    /* Matches R8 bounded <=26h domain, rejects skipped/reversed civil dates. */
    if (b <= a || b - a > 26 * INT64_C(3600000)) return false;
    *start = a; *end = b;
    return true;
}
bool solar_fixed_civil(int64_t now, int offset, solar_civil_day_t *out) {
    if (!out || !offset_ok(offset)) return false;
    /* Check before adding, including INT64_MIN/MAX. Edge UTC is permitted when
     * it maps to a supported civil date. */
    const int64_t lower = -INT64_C(2177452800000), upper = INT64_C(4102444800000);
    int64_t shift = (int64_t)offset * 60000;
    if (now < lower - shift || now >= upper - shift) return false;
    int64_t n = floor_day(now + shift) - floor_day(lower);
    solar_date_t d = {1901,1,1};
    while (n >= 365 + leap(d.year)) { n -= 365 + leap(d.year); ++d.year; }
    while (n >= month_days(d.year,d.month)) { n -= month_days(d.year,d.month); ++d.month; }
    d.day = (int)n + 1;
    *out = (solar_civil_day_t){d,offset,offset};
    return true;
}
static bool location_ok(double lat, double lon) {
    return isfinite(lat) && isfinite(lon) && fabs(lat) <= 90 && fabs(lon) <= 180;
}
/* R8 solar.js ephemeris: public NOAA/Meeus equations, original C organization.
 * Provenance and accuracy limits: docs/solar-core.md, A3-R8/NOTICE.md. */
static double horizon(double ms, double latitude, double longitude) {
    double t = (ms / (double)DAY_MS + 2440587.5 - 2451545) / 36525;
    double L = fmod(280.46646 + t * (36000.76983 + .0003032 * t),360);
    if (L < 0) L += 360;
    double M = (357.52911 + t * (35999.05029 - .0001537 * t)) * DEG;
    double e = .016708634 - t * (.000042037 + .0000001267 * t);
    double centre = sin(M) * (1.914602 - t * (.004817 + .000014 * t)) +
        sin(2*M) * (.019993 - .000101*t) + sin(3*M) * .000289;
    double omega = (125.04 - 1934.136*t)*DEG;
    double apparent = (L + centre - .00569 - .00478*sin(omega))*DEG;
    double obliquity = (23 + (26 + (21.448 - t*(46.815 + t*(.00059 - .001813*t)))/60)/60 +
                        .00256*cos(omega))*DEG;
    double declination = asin(sin(obliquity)*sin(apparent));
    double y = tan(obliquity/2); y *= y;
    double l = L*DEG;
    double equation = 4/DEG*(y*sin(2*l) - 2*e*sin(M) + 4*e*y*sin(M)*cos(2*l) -
                             .5*y*y*sin(4*l) - 1.25*e*e*sin(2*M));
    double minutes = (ms - floor(ms/(double)DAY_MS)*(double)DAY_MS)/60000;
    double angle = ((minutes + 4*longitude + equation)/4 - 180)*DEG;
    double lat = latitude*DEG;
    return sin(lat)*sin(declination) + cos(lat)*cos(declination)*cos(angle) - sin(-.833*DEG);
}
typedef struct { int64_t t; double v; } point_t;
static point_t point(int64_t t, double lat, double lon) {
    return (point_t){t,horizon((double)t,lat,lon)};
}
static point_t extremum(int64_t t, int64_t start, int64_t last, int sign, double lat, double lon) {
    double lo = (double)(t-STEP_MS > start ? t-STEP_MS : start);
    double hi = (double)(t+STEP_MS < last ? t+STEP_MS : last);
    for (int i = 0; i < 42; ++i) {
        double a = lo + (hi-lo)/3, b = hi - (hi-lo)/3;
        if (horizon(a,lat,lon)*sign < horizon(b,lat,lon)*sign) lo = a;
        else hi = b;
    }
    /* JS Math.round, also for pre-1970 negative Unix times. */
    return point((int64_t)floor((lo+hi)/2 + .5),lat,lon);
}
static bool crossing(point_t a, point_t b, double lat, double lon, solar_day_t *out) {
    if ((a.v >= 0) == (b.v >= 0)) return true;
    int64_t lo = a.t, hi = b.t;
    while (hi-lo > 1) {
        int64_t mid = lo + (hi-lo)/2;
        if ((horizon((double)mid,lat,lon) >= 0) == (a.v >= 0)) lo = mid;
        else hi = mid;
    }
    if (out->count && hi-out->events[out->count-1].utc_ms < 2) return true;
    if (out->count == SOLAR_MAX_EVENTS) return false;
    out->events[out->count++] = (solar_event_t){a.v < 0 ? SOLAR_SUNRISE : SOLAR_SUNSET,hi};
    return true;
}
bool solar_calculate(double lat, double lon, solar_civil_day_t civil, solar_day_t *out) {
    if (!out) return false;
    memset(out,0,sizeof(*out)); out->reason = SOLAR_BAD_INPUT;
    if (!location_ok(lat,lon) || !solar_civil_bounds(civil,&out->start_utc_ms,&out->end_utc_ms))
        return false;
    int64_t start = out->start_utc_ms, last = out->end_utc_ms-1;
    point_t low = point(start,lat,lon), high = low;
    out->start_phase = low.v >= 0 ? SOLAR_DAY : SOLAR_NIGHT;
    /* Two streaming passes replace R8's points array, retain same brackets and
     * global-extremum refinement. <=313 coarse points at maximum 26h. */
    for (int64_t t = start;;) {
        point_t p = point(t,lat,lon);
        if (p.v < low.v) low = p;
        if (p.v > high.v) high = p;
        if (t == last) break;
        t = t+STEP_MS < last ? t+STEP_MS : last;
    }
    point_t extra[2] = {extremum(low.t,start,last,-1,lat,lon),extremum(high.t,start,last,1,lat,lon)};
    double minimum = fmin(low.v, fmin(extra[0].v,extra[1].v));
    double maximum = fmax(high.v, fmax(extra[0].v,extra[1].v));
    if (extra[0].t > extra[1].t) { point_t tmp = extra[0]; extra[0] = extra[1]; extra[1] = tmp; }
    point_t previous = point(start,lat,lon);
    unsigned index = 0;
    for (int64_t t = start;;) {
        t = t+STEP_MS < last ? t+STEP_MS : last;
        point_t next = point(t,lat,lon);
        while (index < 2 && extra[index].t <= t) {
            if (!crossing(previous,extra[index],lat,lon,out)) goto uncertain;
            previous = extra[index++];
        }
        if (!crossing(previous,next,lat,lon,out)) goto uncertain;
        previous = next;
        if (t == last) break;
    }
    out->status = out->count ? SOLAR_NORMAL : minimum > 1e-8 ? SOLAR_CONTINUOUS_DAY :
                  maximum < -1e-8 ? SOLAR_CONTINUOUS_NIGHT : SOLAR_INDETERMINATE;
    if (out->status == SOLAR_INDETERMINATE) goto uncertain;
    out->reason = SOLAR_OK;
    return true;
uncertain:
    out->status = SOLAR_INDETERMINATE; out->reason = SOLAR_NUMERIC_UNCERTAIN;
    out->count = 0; out->start_phase = SOLAR_UNKNOWN;
    return false;
}
solar_state_t solar_evaluate(const solar_day_t *day, int64_t now, bool trusted) {
    solar_state_t s = {0};
    if (!trusted) { s.reason = SOLAR_UNTRUSTED_TIME; return s; }
    if (!day) { s.reason = SOLAR_BAD_INPUT; return s; }
    s.reason = day->reason; s.status = day->status;
    if (day->reason != SOLAR_OK) return s;
    if (now < day->start_utc_ms || now >= day->end_utc_ms) {
        s.reason = SOLAR_STALE_WINDOW; return s;
    }
    s.phase = day->start_phase;
    s.has_next_check = true; s.next_check_utc_ms = day->end_utc_ms;
    for (unsigned i = 0; i < day->count; ++i) {
        solar_event_t event = day->events[i];
        if (now >= event.utc_ms) s.phase = event.type == SOLAR_SUNRISE ? SOLAR_DAY : SOLAR_NIGHT;
        else {
            s.has_next_event = true; s.next_event = event;
            s.next_check_utc_ms = event.utc_ms; break;
        }
    }
    return s;
}
static bool same_key(const solar_input_t *a, const solar_input_t *b) {
    return a->latitude == b->latitude && a->longitude == b->longitude &&
        a->civil.date.year == b->civil.date.year && a->civil.date.month == b->civil.date.month &&
        a->civil.date.day == b->civil.date.day &&
        a->civil.start_offset_minutes == b->civil.start_offset_minutes &&
        a->civil.end_offset_minutes == b->civil.end_offset_minutes &&
        a->location_revision == b->location_revision && a->clock_revision == b->clock_revision;
}
solar_state_t solar_update(solar_model_t *model, const solar_input_t *input) {
    solar_state_t error = {0}; error.reason = SOLAR_BAD_INPUT;
    if (!model) return error;
    if (!input) { model->cached = false; return error; }
    if (!input->trusted) {
        model->cached = false; error.reason = SOLAR_UNTRUSTED_TIME; return error;
    }
    int64_t start, end;
    if (!location_ok(input->latitude,input->longitude) || !solar_civil_bounds(input->civil,&start,&end)) {
        model->cached = false; return error;
    }
    if (input->now_utc_ms < start || input->now_utc_ms >= end) {
        model->cached = false; error.reason = SOLAR_STALE_WINDOW; return error;
    }
    if (!model->cached || !same_key(&model->key,input) || input->now_utc_ms < model->key.now_utc_ms) {
        model->cached = solar_calculate(input->latitude,input->longitude,input->civil,&model->day);
        ++model->calculations;
    }
    model->key = *input;
    return solar_evaluate(&model->day,input->now_utc_ms,true);
}
