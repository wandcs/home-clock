#pragma once
#include "sdkconfig.h"
#include "location_data.h"
typedef enum { LOC_IDLE, LOC_SEARCHING, LOC_RESULTS, LOC_EMPTY, LOC_OFFLINE, LOC_CLOCK,
    LOC_TRANSPORT, LOC_TIMEOUT, LOC_HTTP, LOC_OVERSIZE, LOC_PARSE, LOC_SAVING, LOC_SAVED, LOC_SAVE_FAILED } location_phase_t;
typedef struct {
    bool configured, boot_loaded, initialized, saving, first_fetch_pending, save_unconfirmed;
    bool catalog_ready;
    location_record_t saved;
    uint32_t generation, revision, discarded, searches, saves, failures;
    unsigned count,rejected;
    location_phase_t phase;
    int storage_error,http,transport,bytes;
    uint32_t total,query_us,init_us,workspace_bytes;
} location_state_t;
void location_start(void);
location_state_t location_read(void);
uint32_t location_search(const char *query);
void location_cancel(void);
bool location_result(unsigned index,uint32_t generation,location_record_t *out);
bool location_save(unsigned index,uint32_t generation);
const char *location_phase_name(location_phase_t phase);
void location_status(void);
#if CONFIG_CLOCK_DIAGNOSTICS
bool location_public_test(void);
bool location_command(const char *line);
#endif
