#include "network_memory.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static DRAM_ATTR portMUX_TYPE failure_lock=portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR network_memory_snapshot_t failure;

static void IRAM_ATTR allocation_failed(size_t size,uint32_t caps,const char *function_name){
    (void)function_name;
    if(!(caps&(MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL)))return;
    /* The hook may run in ISR context. This short diagnostic critical section
     * never allocates, formats output, queries the heap or takes a network lock. */
    portENTER_CRITICAL_SAFE(&failure_lock);
    if(!failure.failures){
        failure.first_size=size;
        failure.first_caps=caps;
        failure.first_us=(uint64_t)esp_timer_get_time();
    }
    if(failure.failures<UINT32_MAX)failure.failures++;
    portEXIT_CRITICAL_SAFE(&failure_lock);
}

esp_err_t network_memory_start(void){
    return heap_caps_register_failed_alloc_callback(allocation_failed);
}
network_memory_snapshot_t network_memory_read(void){
    portENTER_CRITICAL_SAFE(&failure_lock);
    network_memory_snapshot_t copy=failure;
    portEXIT_CRITICAL_SAFE(&failure_lock);
    return copy;
}
