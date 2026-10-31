/* LVGL-owned objects and strings belong in PSRAM. Keep internal/DMA memory
 * available for display transport and strict TLS. No global malloc override. */
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <assert.h>
#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM
void lv_mem_init(void) { assert(heap_caps_get_total_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)>0); }
void lv_mem_deinit(void) { }
void *lv_malloc_core(size_t size) { return heap_caps_malloc(size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
void *lv_realloc_core(void *p,size_t size) { return heap_caps_realloc(p,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); }
void lv_free_core(void *p) { heap_caps_free(p); }
/* Match the previous CLIB port: no private pool/monitor support. The app's
 * heap_caps health telemetry independently measures internal/DMA/PSRAM. */
lv_mem_pool_t lv_mem_add_pool(void *mem,size_t size) { (void)mem;(void)size;return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }
void lv_mem_monitor_core(lv_mem_monitor_t *monitor) { (void)monitor; }
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#else
#error "Application LVGL memory policy requires LV_USE_CUSTOM_MALLOC"
#endif
