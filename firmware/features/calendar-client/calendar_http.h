#ifndef CLOCK_CALENDAR_HTTP_H
#define CLOCK_CALENDAR_HTTP_H
#include "calendar_client.h"
typedef struct {
    int status, transport, tls_error, tls_flags;
    size_t bytes;
    uint64_t elapsed_ms;
} cc_http_info;
/* Run in caller's background task, serialized with the app's existing HTTP
 * budget. Caller supplies >=16384 bytes scratch, releases/reuses after parse.
 * No hidden task, retries, heap-owned result, credentials or body logging.
 * 25s cooperative deadline; blocking SDK DNS/cleanup are not a hard deadline.
 * Successful transport alone does not activate calendar data. */
cc_result cc_http_candidate(uint16_t year, bool online, bool trusted_clock,
                            uint8_t *scratch, size_t cap,
                            cc_candidate *out, cc_http_info *info);
#endif
