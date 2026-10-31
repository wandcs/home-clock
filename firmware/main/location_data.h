#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define LOCATION_RESULTS 8
#define LOCATION_QUERY_BYTES 257
#define LOCATION_BODY_LIMIT 16384
#define LOCATION_LEGACY_FIELDS \
    uint32_t version, id; \
    double latitude, longitude; \
    char name[160], country[128], admin[4][160], timezone[80], country_code[3]
typedef struct { LOCATION_LEGACY_FIELDS; } location_legacy_record_t;
typedef struct {
    LOCATION_LEGACY_FIELDS;
    /* v2 is self-contained. Source identity is snapshot-bound, never looked
     * up by name or automatically migrated when the catalog changes. */
    char source_id[32], source_timezone[80], source[160], source_version[48];
    char catalog_version[80], point_purpose[96], source_region[3];
    uint8_t scope; /* 1 mainland, 2 HMT, 3 overseas; v1 leaves the tail empty. */
} location_record_t;
#undef LOCATION_LEGACY_FIELDS
/* Original F3 persisted ABI; checked on the native and target compilers. */
_Static_assert(sizeof(location_legacy_record_t)==1040,"legacy location ABI changed");
typedef struct { location_record_t items[LOCATION_RESULTS]; unsigned count, rejected; } location_results_t;
bool location_valid(const location_record_t *r);
bool location_equal(const location_record_t *a,const location_record_t *b);
bool location_query_valid(const char *q);
bool location_parse(const char *body,size_t bytes,location_results_t *out);
bool location_url(const char *query,char *out,size_t size);
/* Presentation only: exact complete context tokens, never persisted identity. */
bool location_display_context(const location_record_t *record,char *out,size_t size);
/* Pass the actual weather response timezone for weather dates; NULL if unknown. */
const char *location_weather_date_basis(const char *timezone);
