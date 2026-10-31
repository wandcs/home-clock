#include "calendar_http.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <strings.h>
typedef struct {
    uint8_t *body;
    size_t used;
    uint64_t start;
    bool oversized, expired, encoding, cancelled;
}
body;
static uint64_t now_ms(void) {
    return (uint64_t)esp_timer_get_time()/1000u;
}
static esp_err_t event(esp_http_client_event_t *e) {
    body *b=e->user_data;
    if(now_ms()-b->start>=25000u)b->expired=true;
    if(e->event_id==HTTP_EVENT_ON_HEADER&&e->header_key&&e->header_value&& !strcasecmp(e->header_key,"Content-Encoding")&&strcasecmp(e->header_value,"identity"))b->encoding=true;
    if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0) {
        size_t n=(size_t)e->data_len;
        if(n>CC_BODY_LIMIT-b->used)b->oversized=true;
        else {
            memcpy(b->body+b->used,e->data,n);
            b->used+=n;
        }
    }
    /* IDF's parser ignores ON_DATA callback return: close in this task once. */
    if((b->oversized||b->expired||b->encoding)&&!b->cancelled&& (e->event_id==HTTP_EVENT_ON_DATA||e->event_id==HTTP_EVENT_ON_HEADER||e->event_id==HTTP_EVENT_ON_CONNECTED)) {
        b->cancelled=true;
        esp_http_client_close(e->client);
    }
    return ESP_OK;
}
cc_result cc_http_candidate(uint16_t year,bool online,bool clock,uint8_t *scratch,size_t cap,cc_candidate *out,cc_http_info *info) {
    if(!scratch||cap<CC_BODY_LIMIT||!out||!info)return CC_ARGUMENT;
    *info=(cc_http_info) {
        0
    };
    if(!clock)return CC_CLOCK;
    if(!online)return CC_OFFLINE;
    char url[112];
    if(!cc_url(year,url,sizeof url))return CC_ARGUMENT;
    body b= {
        .body=scratch,.start=now_ms()
    };
    esp_http_client_config_t cfg= {
        .url=url,.timeout_ms=4000,.crt_bundle_attach=esp_crt_bundle_attach, .skip_cert_common_name_check=false,.disable_auto_redirect=true,.max_authorization_retries=-1, .transport_type=HTTP_TRANSPORT_OVER_SSL,.buffer_size=1024,.buffer_size_tx=512, .event_handler=event,.user_data=&b,.is_async=true
    };
    esp_http_client_handle_t h=esp_http_client_init(&cfg);
    esp_err_t err=ESP_ERR_NO_MEM;
    if(h) {
        err=esp_http_client_set_header(h,"Accept-Encoding","identity");
        if(err==ESP_OK)do {
            if(now_ms()-b.start>=25000u) {
                b.expired=true;
                err=ESP_ERR_TIMEOUT;
                break;
            }
            err=esp_http_client_perform(h);
            if(err==ESP_ERR_HTTP_EAGAIN)vTaskDelay(pdMS_TO_TICKS(20));
        }
        while(err==ESP_ERR_HTTP_EAGAIN&&!b.cancelled);
    }
    info->status=h?esp_http_client_get_status_code(h):0;
    info->transport=err;
    info->bytes=b.used;
    cc_result result;
    if(b.oversized)result=CC_SIZE;
    else if(b.encoding)result=CC_HTTP;
    else if(b.expired||err==ESP_ERR_TIMEOUT||err==ESP_ERR_HTTP_EAGAIN||err==ESP_ERR_HTTP_READ_TIMEOUT)result=CC_TIMEOUT;
    else if(err!=ESP_OK)result=CC_TRANSPORT;
    else if(!esp_http_client_is_complete_data_received(h))result=CC_HTTP;
    else if(info->status==404)result=CC_NOT_FOUND;
    else if(info->status!=200)result=CC_HTTP;
    else result=CC_OK;
    if(h) {
        esp_http_client_get_and_clear_last_tls_error(h,&info->tls_error,&info->tls_flags);
        esp_http_client_cleanup(h);
    }
    info->elapsed_ms=now_ms()-b.start;
    if(result==CC_OK)result=cc_parse(scratch,b.used,year,out);
    return result;
}
