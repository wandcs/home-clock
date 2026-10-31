#ifndef CLOCK_ALARM_STORE_H
#define CLOCK_ALARM_STORE_H
#include "alarm_trigger.h"

#define AS_WIRE_MAX (20u+20u*AM_CAPACITY)
typedef enum { AS_DECODE_CURRENT,AS_DECODE_LEGACY,AS_DECODE_BAD,AS_DECODE_FOREIGN } as_decode_status;
size_t as_encode(const at_image *,uint8_t *,size_t);
/* Legal v1 produces a revision=1/empty-ledger migration candidate only.
 * Failure/foreign leaves output untouched. No C struct is a wire record. */
as_decode_status as_decode(const uint8_t *,size_t,at_image *);

typedef enum { AS_RAW_OK,AS_RAW_MISSING,AS_RAW_OVERSIZE,AS_RAW_ERROR } as_raw_status;
typedef as_raw_status (*as_read_fn)(void *,uint8_t *,size_t,size_t *,int *);
typedef int (*as_replace_fn)(void *,const uint8_t *,size_t);
typedef struct {void *ctx;as_read_fn read;as_replace_fn replace;} as_port;
typedef enum { AS_CURRENT,AS_LEGACY,AS_MISSING,AS_BAD,AS_FOREIGN,AS_READ_ERROR,
               AS_SAVED,AS_UNCHANGED,AS_OLD_VALUE,AS_DIFFERENT,AS_UNCERTAIN,AS_BLOCKED,AS_INVALID } as_status;
typedef enum { AS_ORIGIN_NONE,AS_ORIGIN_CURRENT,AS_ORIGIN_LEGACY,AS_ORIGIN_MISSING } as_origin;
typedef struct {
    as_status status;
    as_origin origin;
    at_image image; /* meaningful only for CURRENT/LEGACY/MISSING origin */
    int write_error,read_error;
} as_result;
typedef struct {as_port port;bool blocked;} as_store;

/* Exactly ONE owner serializes ALL reads/edits/trigger/close requests for this
 * record. This is application CAS, not atomic NVS CAS across multiple writers.
 * No task, queue, retry, erase, key initialization or driver init is hidden. */
void as_init(as_store *,as_port);
as_result as_reload(as_store *); /* sole uncertainty-unlock path; read only */
/* UI compares config only, then reconfigures the latest full image preserving
 * ledger. Read-only startup never commits a legacy/missing candidate. */
as_result as_save_ui(as_store *,const am_config *expected,const am_config *proposed);
/* Trigger compares whole image and permits only mark/conditional-close changes.
 * Config editing/new IDs is owned by save_ui, not this path. */
as_result as_save_trigger(as_store *,const at_image *expected,const at_image *proposed);
#endif
