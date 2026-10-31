#include "alarm_store_nvs.h"
#include "nvs.h"

static as_raw_status read_record(void *ctx,uint8_t *out,size_t cap,size_t *size,int *error) {
    (void)ctx;nvs_handle_t handle;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&handle);
    *error=err;
    if(err==ESP_ERR_NVS_NOT_FOUND)return AS_RAW_MISSING;
    if(err!=ESP_OK)return AS_RAW_ERROR;
    size_t expected=0;err=nvs_get_blob(handle,"alarms",NULL,&expected);*error=err;
    if(err!=ESP_OK) {nvs_close(handle);return err==ESP_ERR_NVS_NOT_FOUND?AS_RAW_MISSING:AS_RAW_ERROR;}
    if(expected>cap) {nvs_close(handle);*size=expected;return AS_RAW_OVERSIZE;}
    size_t actual=expected;err=nvs_get_blob(handle,"alarms",out,&actual);nvs_close(handle);*error=err;
    if(err!=ESP_OK)return AS_RAW_ERROR;
    if(actual!=expected) {*error=ESP_ERR_INVALID_SIZE;return AS_RAW_ERROR;}
    *size=actual;return AS_RAW_OK;
}
static int replace_record(void *ctx,const uint8_t *bytes,size_t n) {
    (void)ctx;
    if(!bytes||n<20u||n>AS_WIRE_MAX)return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&handle);
    if(err==ESP_OK) {
        err=nvs_set_blob(handle,"alarms",bytes,n);
        if(err==ESP_OK)err=nvs_commit(handle);
        nvs_close(handle);
    }
    return err;
}
as_port as_nvs_port(void) {return (as_port){NULL,read_record,replace_record};}
