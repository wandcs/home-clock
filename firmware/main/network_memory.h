#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Boot-local diagnostic: only failures explicitly requesting DMA or INTERNAL.
 * A zero count means the first_* fields have not been populated. */
typedef struct {
    uint64_t first_us;
    size_t first_size;
    uint32_t first_caps, failures;
} network_memory_snapshot_t;

/* Register once from app_main before starting application/network workers. */
esp_err_t network_memory_start(void);
network_memory_snapshot_t network_memory_read(void);
