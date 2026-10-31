#ifndef CLOCK_CALENDAR_RUNTIME_CORE_H
#define CLOCK_CALENDAR_RUNTIME_CORE_H
#include "calendar_runtime.h"
typedef enum { CR_READ_OK,CR_READ_MISSING,CR_READ_OVERSIZE,CR_READ_ERROR } cr_read_result;
typedef cr_read_result (*cr_read_fn)(void *,unsigned,uint8_t *,size_t,size_t *,int *);
typedef int (*cr_write_fn)(void *,unsigned,const uint8_t *,size_t);
typedef struct {void *ctx;cr_read_fn read;cr_write_fn write;} cr_store;
typedef struct {bool valid,blocked;af_year year;uint8_t raw[AF_YEAR_BYTES];} cr_slot;
typedef struct {
    cr_slot slots[2];cc_schedule schedule[2];uint16_t targets[2];
    cr_snapshot view;cr_store store;
} cr_core;
typedef struct {bool online,trusted;uint16_t year;uint64_t now_ms;} cr_input;
void cr_core_init(cr_core *,cr_store);
/* Explicit startup/recovery reads; only this worker owns the two keys. Known
 * bad AFY1 is disposable derived data; foreign/oversized/I/O blocks its slot. */
void cr_core_reload(cr_core *);
void cr_core_input(cr_core *,const cr_input *);
bool cr_core_take(cr_core *,uint64_t,uint16_t *);
/* Prepared wire must be exact136B and match requested year. Persist only on
 * CC_OK; independently read-back target activates. Old/third/unknown retain
 * last approved RAM; third/unknown blocks until explicit reload. */
cc_result cr_core_finish(cr_core *,uint16_t,cc_result,cc_result,const uint8_t *,uint64_t);
bool cr_input_from_utc(bool,bool,int64_t,uint64_t,cr_input *);
cr_store cr_nvs_store(void);
#endif
