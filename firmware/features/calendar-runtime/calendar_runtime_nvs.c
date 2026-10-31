#include "calendar_runtime_core.h"
#include "nvs.h"
static const char *key(unsigned slot){return slot==0?"calendar0":slot==1?"calendar1":NULL;}
static cr_read_result get(void *ctx,unsigned slot,uint8_t *raw,size_t cap,size_t *size,int *error) {
    (void)ctx;if(!key(slot)||!raw||!size||!error)return CR_READ_ERROR;
    nvs_handle_t h;esp_err_t e=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&h);*error=e;
    if(e==ESP_ERR_NVS_NOT_FOUND)return CR_READ_MISSING;
    if(e!=ESP_OK)return CR_READ_ERROR;
    size_t n=0;e=nvs_get_blob(h,key(slot),NULL,&n);*error=e;
    if(e!=ESP_OK){nvs_close(h);return e==ESP_ERR_NVS_NOT_FOUND?CR_READ_MISSING:CR_READ_ERROR;}
    if(n>cap){*size=n;nvs_close(h);return CR_READ_OVERSIZE;}
    size_t actual=n;e=nvs_get_blob(h,key(slot),raw,&actual);nvs_close(h);*error=e;
    if(e!=ESP_OK)return CR_READ_ERROR;
    if(n!=actual){*error=ESP_ERR_INVALID_SIZE;return CR_READ_ERROR;}
    *size=n;return CR_READ_OK;
}
static int put(void *ctx,unsigned slot,const uint8_t *raw,size_t n) {
    (void)ctx;if(!key(slot)||!raw||n!=AF_YEAR_BYTES)return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t e=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(e==ESP_OK){
        e=slot==0?nvs_set_blob(h,"calendar0",raw,n):nvs_set_blob(h,"calendar1",raw,n);
        if(e==ESP_OK)e=nvs_commit(h);
        nvs_close(h);
    }
    return e;
}
cr_store cr_nvs_store(void){return(cr_store){NULL,get,put};}
