#include "wifi_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>

typedef struct {
    uint8_t version, ssid_len, password_len, auth;
    uint8_t ssid[32], password[64];
} record_t;
static void wipe(void *p,size_t n) { volatile uint8_t *v=p; while(n--) *v++=0; }
static bool supported(unsigned auth) {
    return auth==WIFI_AUTH_OPEN || auth==WIFI_AUTH_WPA_PSK || auth==WIFI_AUTH_WPA2_PSK
        || auth==WIFI_AUTH_WPA_WPA2_PSK || auth==WIFI_AUTH_WPA3_PSK || auth==WIFI_AUTH_WPA2_WPA3_PSK;
}
static bool valid(const record_t *r) {
    if(r->version!=1 || !r->ssid_len || r->ssid_len>32 || r->password_len>64 || !supported(r->auth)) return false;
    if(r->auth==WIFI_AUTH_OPEN) return r->password_len==0;
    if(r->password_len<8) return false;
    if(r->password_len==64) for(unsigned i=0;i<64;i++) {
        uint8_t c=r->password[i];
        if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false;
    }
    return true;
}
esp_err_t wifi_store_init(void) { return nvs_flash_init_partition("nvs"); }
static esp_err_t read_record(record_t *r) {
    nvs_handle_t h; esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&h);
    if(err!=ESP_OK) return err;
    size_t size=sizeof *r; err=nvs_get_blob(h,"wifi",r,&size); nvs_close(h);
    if(err==ESP_OK && (size!=sizeof *r || !valid(r))) err=ESP_ERR_INVALID_VERSION;
    return err;
}
esp_err_t wifi_store_load(wifi_config_t *config) {
    record_t r={0}; esp_err_t err=read_record(&r);
    memset(config,0,sizeof *config);
    if(err==ESP_OK) {
        memcpy(config->sta.ssid,r.ssid,r.ssid_len); memcpy(config->sta.password,r.password,r.password_len);
        config->sta.threshold.authmode=r.auth;
        config->sta.sae_pwe_h2e=WPA3_SAE_PWE_BOTH; config->sta.pmf_cfg.capable=true;
    }
    wipe(&r,sizeof r); return err;
}
esp_err_t wifi_store_save(const wifi_config_t *config) {
    record_t r={.version=1,.ssid_len=strnlen((const char*)config->sta.ssid,32),
        .password_len=strnlen((const char*)config->sta.password,64),.auth=config->sta.threshold.authmode};
    memcpy(r.ssid,config->sta.ssid,32); memcpy(r.password,config->sta.password,64);
    if(!valid(&r)) { wipe(&r,sizeof r); return ESP_ERR_INVALID_ARG; }
    nvs_handle_t h; esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(err==ESP_OK) {
        err=nvs_set_blob(h,"wifi",&r,sizeof r);
        if(err==ESP_OK) err=nvs_commit(h);
        nvs_close(h);
        record_t check={0}; esp_err_t read=read_record(&check);
        /* Resolve even an uncertain commit by independently opening/reading. */
        if(read==ESP_OK && memcmp(&r,&check,sizeof r)==0) err=ESP_OK;
        else if(err==ESP_OK) err=read==ESP_OK?ESP_ERR_INVALID_RESPONSE:read;
        wipe(&check,sizeof check);
    }
    wipe(&r,sizeof r); return err;
}
#if CONFIG_CLOCK_DIAGNOSTICS
esp_err_t wifi_store_clear(void) {
    nvs_handle_t h;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND) err=ESP_OK;
    else if(err==ESP_OK) {
        err=nvs_erase_key(h,"wifi");
        if(err==ESP_ERR_NVS_NOT_FOUND) err=ESP_OK;
        if(err==ESP_OK) err=nvs_commit(h);
        nvs_close(h);
    }
    /* A missing readback cannot erase a real erase/commit error. */
    if(err!=ESP_OK) return err;
    wifi_config_t check={0};err=wifi_store_load(&check);wipe(&check,sizeof check);
    return err==ESP_ERR_NVS_NOT_FOUND?ESP_OK:err==ESP_OK?ESP_ERR_INVALID_RESPONSE:err;
}
#endif
