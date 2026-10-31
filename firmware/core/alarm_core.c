#include "alarm_core.h"
#include <string.h>
_Static_assert(sizeof(af_year) == 94u, "unexpected year layout");

static bool leap(unsigned year) { return year % 4u == 0u; } /* 2000..2099 only */
static unsigned month_days(unsigned year, unsigned month)
{
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return (unsigned)days[month-1u] + ((month == 2u && leap(year)) ? 1u : 0u);
}
bool af_time_valid(const af_time *t)
{
    return t && t->year >= 2000u && t->year <= 2099u &&
        t->month >= 1u && t->month <= 12u && t->day >= 1u &&
        t->day <= month_days(t->year, t->month) && t->hour < 24u &&
        t->minute < 60u && t->second < 60u;
}
bool af_day_index(const af_time *t, uint16_t *index)
{
    if (!index || !af_time_valid(t)) return false;
    unsigned d = t->day - 1u;
    for (unsigned m = 1u; m < t->month; ++m) d += month_days(t->year, m);
    *index = (uint16_t)d;
    return true;
}
static bool bit(const uint8_t *map, unsigned d)
{ return (((unsigned)map[d/8u] >> (d%8u)) & 1u) != 0u; }
bool af_year_valid(const af_year *y)
{
    if (!y || y->year < 2000u || y->year > 2099u) return false;
    const unsigned length = leap(y->year) ? 366u : 365u;
    for (unsigned i = 0; i < 368u; ++i) {
        bool a = bit(y->off, i), b = bit(y->work, i);
        if ((a && b) || (i >= length && (a || b))) return false;
    }
    return true;
}
static uint32_t crc32(const uint8_t *bytes, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned j = 0; j < 8u; ++j)
            crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    }
    return ~crc;
}
static uint32_t le32(const uint8_t *p)
{ return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24; }
bool af_year_decode(const uint8_t *bytes, size_t size, af_year *out)
{
    if (!bytes || !out || size != AF_YEAR_BYTES ||
        memcmp(bytes, "AFY1\x01\x00", 6u) != 0 || crc32(bytes,132u) != le32(bytes+132u))
        return false;
    af_year temp;
    temp.year = (uint16_t)((unsigned)bytes[6] | (unsigned)bytes[7]<<8);
    memcpy(temp.off, bytes+8u, 46u);
    memcpy(temp.work, bytes+54u, 46u);
    if (!af_year_valid(&temp)) return false;
    *out = temp;
    return true;
}
static bool catalog_valid(const af_year *years, size_t count)
{
    if (count > 100u || (count && !years)) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!af_year_valid(&years[i])) return false;
        /* Sorted unique years, bounded lookup. Product alarm count is unrelated. */
        if (i && years[i-1u].year >= years[i].year) return false;
    }
    return true;
}
static bool workday_unchecked(const af_time *date, const af_year *years, size_t count,
                              bool *fallback)
{
    uint16_t index = 0;
    (void)af_day_index(date, &index);
    *fallback = true;
    for (size_t i = 0; i < count; ++i) {
        if (years[i].year == date->year) {
            *fallback = false;
            if (bit(years[i].work, index)) return true;
            if (bit(years[i].off, index)) return false;
            break;
        }
    }
    unsigned elapsed = index;
    for (unsigned y = 2000u; y < date->year; ++y) elapsed += leap(y) ? 366u : 365u;
    return (elapsed + 5u) % 7u < 5u; /* 2000-01-01 Saturday; Monday=0. */
}
bool af_is_workday(const af_time *date, const af_year *years, size_t count,
                   bool *workday, bool *fallback)
{
    if (!workday || !fallback || !af_time_valid(date) || !catalog_valid(years,count)) return false;
    *workday = workday_unchecked(date,years,count,fallback);
    return true;
}
static bool advance(af_time *t)
{
    if (t->day < month_days(t->year,t->month)) { ++t->day; return true; }
    if (t->month < 12u) { t->day = 1u; ++t->month; return true; }
    if (t->year == 2099u) return false;
    t->day = 1u; t->month = 1u; ++t->year; return true;
}
af_candidate af_next(const af_alarm *a, const af_time *r, bool trusted, bool inclusive,
                     const af_year *years, size_t count)
{
    af_candidate result = {.status = AF_INVALID};
    if (!a || a->hour >= 24u || a->minute >= 60u ||
        a->mode < AF_ONCE || a->mode > AF_DAILY) return result;
    if (!a->enabled) { result.status = AF_DISABLED; return result; }
    if (a->mode == AF_ONCE && !a->once_pending) { result.status = AF_CONSUMED; return result; }
    if (!trusted) { result.status = AF_WAIT_TIME; return result; }
    if (!af_time_valid(r)) return result;
    /* ONCE/DAILY deliberately do not depend on calendar validity. */
    if (a->mode == AF_WORKDAY && !catalog_valid(years,count)) return result;
    af_time at = *r;
    at.hour = a->hour; at.minute = a->minute; at.second = 0;
    const unsigned target = (unsigned)a->hour*3600u + (unsigned)a->minute*60u;
    const unsigned now = (unsigned)r->hour*3600u + (unsigned)r->minute*60u + r->second;
    for (unsigned day = 0; day <= 366u; ++day) {
        bool eligible = day != 0u || target > now || (inclusive && target == now);
        if (eligible && a->mode == AF_WORKDAY) {
            bool fallback;
            eligible = workday_unchecked(&at,years,count,&fallback);
            result.calendar_fallback |= fallback;
        }
        if (eligible) { result.status = AF_OK; result.at = at; return result; }
        if (day == 366u) break;
        if (!advance(&at)) { result.status = AF_RANGE_END; return result; }
    }
    result.status = AF_NO_CANDIDATE;
    return result;
}
