#pragma once
#include "location_data.h"
#include "location_index/index.h"

typedef struct { uint32_t cp, offset; } location_norm_map_t;
typedef struct { uint32_t first,last; } location_norm_range_t;
extern const location_norm_map_t location_norm_map[];
extern const location_norm_range_t location_norm_ranges[];
extern const unsigned location_norm_map_count,location_norm_range_count;
extern const unsigned char location_norm_pool[];
extern const char location_catalog_version[];

/* No allocator or global mutable query data: the caller owns one view and
 * workspace, reused by the serialized location worker. Strings borrow Flash. */
typedef struct {
    lix_index index;
    const uint8_t *metadata,*pool;
    size_t metadata_bytes,pool_bytes;
} location_catalog_t;
bool location_normalize(const char *raw,char out[LOCATION_QUERY_BYTES]);
bool location_catalog_open(location_catalog_t *,const void *,size_t,const void *,size_t);
bool location_catalog_record(const location_catalog_t *,uint32_t,location_record_t *);
bool location_catalog_query(const location_catalog_t *,const char *,void *,size_t,
                            location_results_t *,lix_result *);
