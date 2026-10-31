#include "alarm_nvs.h"
#include "nvs.h"
#include <string.h>

am_io_result am_nvs_load(void){
    am_io_result r={.status=AM_IO_ERROR};nvs_handle_t h;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND){am_config_empty(&r.config);r.status=AM_IO_MISSING;r.read_error=err;return r;}
    if(err!=ESP_OK){r.read_error=err;return r;}
    size_t size=0;err=nvs_get_blob(h,"alarms",NULL,&size);
    if(err==ESP_ERR_NVS_NOT_FOUND){nvs_close(h);am_config_empty(&r.config);r.status=AM_IO_MISSING;r.read_error=err;return r;}
    if(err!=ESP_OK){nvs_close(h);r.read_error=err;return r;}
    if(size>AM_WIRE_MAX){nvs_close(h);r.status=AM_IO_FOREIGN;return r;}
    if(size<20u){nvs_close(h);r.status=AM_IO_BAD;return r;}
    uint8_t bytes[AM_WIRE_MAX];size_t actual=size;
    err=nvs_get_blob(h,"alarms",bytes,&actual);nvs_close(h);r.read_error=err;
    if(err!=ESP_OK||actual!=size)return r;
    am_decode_status decoded=am_decode(bytes,actual,&r.config);
    r.status=decoded==AM_DECODE_OK?AM_IO_OK:decoded==AM_DECODE_FOREIGN?AM_IO_FOREIGN:AM_IO_BAD;
    return r;
}
am_io_result am_nvs_save(const am_config *expected,const am_config *proposed){
    am_io_result before=am_nvs_load();
    if(!am_config_valid(expected)||!am_config_valid(proposed)){before.status=AM_IO_ERROR;before.write_error=ESP_ERR_INVALID_ARG;return before;}
    /* Preserve foreign/corrupt bytes and reject a stale caller before opening RW. */
    if(before.status!=AM_IO_OK&&before.status!=AM_IO_MISSING)return before;
    if(!am_config_equal(expected,&before.config)){before.status=AM_IO_DIFFERENT;return before;}
    uint8_t bytes[AM_WIRE_MAX];size_t size=am_encode(proposed,bytes,sizeof bytes);
    nvs_handle_t h;esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(err==ESP_OK){
        err=nvs_set_blob(h,"alarms",bytes,size);
        if(err==ESP_OK)err=nvs_commit(h);
        nvs_close(h);
    }
    /* Commit/set errors do not prove old bytes survived. Resolve independently. */
    am_io_result after=am_nvs_load();after.write_error=err;
    if(after.status==AM_IO_OK&&am_config_equal(proposed,&after.config))after.status=AM_IO_SAVED;
    else if(after.status==AM_IO_OK||after.status==AM_IO_MISSING)after.status=AM_IO_DIFFERENT;
    else after.status=AM_IO_UNCERTAIN;
    return after;
}
