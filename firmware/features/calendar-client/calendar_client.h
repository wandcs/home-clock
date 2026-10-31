#ifndef CLOCK_CALENDAR_CLIENT_H
#define CLOCK_CALENDAR_CLIENT_H
#include "alarm_core.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CC_BODY_LIMIT 16384u
#define CC_DAYS_LIMIT 100u
#define CC_PAPERS_LIMIT 8u
#define CC_PAPER_BYTES 192u
#define CC_ALL_FAMILIES 127u

typedef enum {
    CC_OK = 0, CC_ARGUMENT, CC_SIZE, CC_JSON, CC_SCHEMA, CC_DATE,
    CC_DUPLICATE, CC_EMPTY, CC_NOT_FOUND, CC_INCOMPLETE, CC_COVERAGE, CC_CONFLICT, CC_UNAPPROVED,
    CC_STORAGE, CC_UNCERTAIN, CC_TRANSPORT, CC_HTTP, CC_TIMEOUT,
    CC_OFFLINE, CC_CLOCK
} cc_result;

/* A parsed candidate is NEVER an af_year authorized for calculation.
 * years[0] is title_year-1, years[1] title_year. Previous-year entries
 * support the upstream's notice-title-year convention. No heap ownership. */
typedef struct {
    uint16_t title_year, day_count;
    uint8_t paper_count, families;
    af_year years[2];
    char papers[CC_PAPERS_LIMIT][CC_PAPER_BYTES];
} cc_candidate;

/* Strict, bounded, nonrecursive UTF-8 JSON grammar; output unchanged on error.
 * Empty future source is parsed as CC_EMPTY, also leaves output unchanged. */
cc_result cc_parse(const uint8_t *raw, size_t bytes, uint16_t title_year,
                   cc_candidate *out);

typedef enum {
    CC_NO_APPROVAL = 0,
    CC_INDEPENDENT_REVIEW = 1,
    /* Caller explicitly trusts the upstream's annual transcription, including
     * omitted dates. Structural coverage is not a proof of completeness.
     * This mode is a policy port, not automatically selected by this module. */
    CC_TRUSTED_UPSTREAM = 2
} cc_approval;

/* base must have all seven holiday families, nonempty official papers.
 * adjacent is a parsed *complete* next-title-year candidate, with status
 * CC_OK, or NULL with CC_EMPTY for an actually fetched empty future file,
 * or CC_NOT_FOUND for an explicit HTTP404 from that next-title-year URL.
 * Transport/HTTP/partial errors are propagated, never treated as empty.
 * Conflicting previous-December dates reject the entire update.
 * digest is caller's SHA256 over the source bundle, not computed/trusted here.
 * Output is AFY1 compatible; approval must cover BOTH sources if provided. */
cc_result cc_prepare(const cc_candidate *base, const cc_candidate *adjacent,
                      cc_result adjacent_status, cc_approval approval, const uint8_t digest[32],
                      uint8_t output[AF_YEAR_BYTES]);

/* Storage is owned/serialized by caller. put atomically replaces ONE bounded
 * record; get must report the actual independent committed read, <= cap.
 * No erase, keys, RTOS, clock, network or filesystem are hidden here. */
typedef bool (*cc_get_fn)(void *, uint16_t, uint8_t *, size_t, size_t *);
typedef bool (*cc_put_fn)(void *, uint16_t, const uint8_t *, size_t);
typedef struct { void *ctx; cc_get_fn get; cc_put_fn put; } cc_store;
typedef struct {
    bool valid, uncertain;
    af_year year;
    uint8_t bytes[AF_YEAR_BYTES];
} cc_cache;
/* Stored CRC is not provenance: caller must separately approve the retained
 * record before load; load is also the explicit uncertainty recovery path. */
cc_result cc_cache_load(cc_cache *, const cc_store *, uint16_t, cc_approval);
cc_result cc_cache_update(cc_cache *, const cc_store *, const uint8_t *, size_t,
                          cc_approval);

typedef struct { uint64_t due_ms; uint8_t failures; bool in_flight; } cc_schedule;
/* One low-frequency job at a time. Caller chooses current/next year bundle.
 * Normal recheck 7 days, failures 1h/6h/24h/72h; no catchup burst. */
void cc_schedule_init(cc_schedule *, uint64_t now_ms);
bool cc_schedule_take(cc_schedule *, uint64_t now_ms, bool online, bool clock);
void cc_schedule_finish(cc_schedule *, uint64_t now_ms, bool success);

/* Strict immutable provider URL (no arbitrary host/config/HTTP fallback). */
bool cc_url(uint16_t title_year, char *out, size_t cap);
#endif
