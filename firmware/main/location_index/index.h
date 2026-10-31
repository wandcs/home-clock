#ifndef LOCATION_INDEX_H
#define LOCATION_INDEX_H
#include <stddef.h>
#include <stdint.h>

/* Immutable, validated mapping must outlive the view and every returned string.
 * Canonical UTF-8 input: R2 norm() is an adapter, not part of this C core.
 * No allocator, globals, unaligned casts, recursion, or firmware dependencies.
 */
#define LIX_VARIANTS 1400
typedef struct {
    const uint8_t *data, *records, *keys, *posts, *pool, *ranks, *dirs;
    uint32_t nr, nk, np, ns;
    int compact, accelerate;
} lix_index;
typedef struct { uint32_t record, key; uint8_t tier, kind; } lix_hit;
typedef struct {
    uint32_t total, count, variants;
    uint32_t comparisons, keys_scanned, posts_scanned, improvements;
} lix_result;
/* Open validates the whole untrusted file before exposing a view. 0=success. */
int lix_open(lix_index *, const void *, size_t);
size_t lix_workspace_size(const lix_index *);
/* Workspace is bytes, no alignment required. Output capacity is the requested
 * limit and may reach nr: no silent top-eight semantic restriction.
 * Invalid UTF-8, noncanonical ASCII, >64 scalars, short workspace or cap=0: -1.
 * Empty canonical query is valid and returns zero results.
 */
int lix_query(const lix_index *, const char *, size_t, int,
              void *, size_t, lix_hit *, uint32_t, lix_result *);
const char *lix_string(const lix_index *, uint32_t);
const char *lix_key(const lix_index *, uint32_t);
const uint8_t *lix_record(const lix_index *, uint32_t);
uint32_t lix_u32(const uint8_t *);
#endif
