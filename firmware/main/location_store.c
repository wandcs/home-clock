#include "sdkconfig.h"
#include "location_store.h"
#include "nvs.h"
#include <string.h>
#if CONFIG_CLOCK_DIAGNOSTICS
static bool fail_readback_once;
void location_store_fail_readback_once(void) { fail_readback_once=true; } /* Location worker only. */
#endif
esp_err_t location_store_load(location_record_t *r) {
    memset(r,0,sizeof *r); nvs_handle_t h;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&h);
    if(err!=ESP_OK)return err;
    size_t size=sizeof *r;err=nvs_get_blob(h,"location",r,&size);nvs_close(h);
    if(err==ESP_OK&&size==sizeof(location_legacy_record_t)&&r->version==1) {
        /* Identical original prefix, but discard legacy tail padding. No
         * write-back, catalog lookup, ID/name matching or automatic migration. */
        size_t tail=offsetof(location_record_t,source_id);
        memset((char*)r+tail,0,sizeof *r-tail);
    }else if(err==ESP_OK&&(size!=sizeof *r||r->version!=2))err=ESP_ERR_INVALID_VERSION;
    if(err==ESP_OK&&!location_valid(r))err=ESP_ERR_INVALID_VERSION;
    if(err!=ESP_OK)memset(r,0,sizeof *r);
    return err;
}
esp_err_t location_store_save(const location_record_t *r) {
#if CONFIG_CLOCK_DIAGNOSTICS
    bool inject_readback=fail_readback_once;fail_readback_once=false;
#endif
    if(!location_valid(r))return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    size_t size=r->version==1?sizeof(location_legacy_record_t):sizeof *r;
    err=nvs_set_blob(h,"location",r,size);if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);
    location_record_t check;esp_err_t read;
#if CONFIG_CLOCK_DIAGNOSTICS
    if(inject_readback)read=ESP_FAIL;
    else
#endif
    read=location_store_load(&check);
    /* Commit errors are resolved by an independent handle before any retry. */
    if(read==ESP_OK&&location_equal(r,&check))return ESP_OK;
    return err==ESP_OK?(read==ESP_OK?ESP_ERR_INVALID_RESPONSE:read):err;
}
#if CONFIG_CLOCK_DIAGNOSTICS
esp_err_t location_store_clear(void) {
    nvs_handle_t h;esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(err!=ESP_OK)return err;
    err=nvs_erase_key(h,"location");if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);
    location_record_t check;esp_err_t read=location_store_load(&check);
    if(read==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    return err==ESP_OK?ESP_ERR_INVALID_RESPONSE:err;
}
#endif
