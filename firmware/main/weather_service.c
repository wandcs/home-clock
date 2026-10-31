/* One weather owner, serial providers and RAM snapshots. No HTTP under UI locks. */
#include "weather_service.h"
#include "../features/met-weather/met_forecast.h"
#include "app_state.h"
#include "location_service.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <inttypes.h>
#include "zlib.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if CONFIG_CLOCK_DIAGNOSTICS
#include <errno.h>
#include "esp_tls.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#endif

static SemaphoreHandle_t mutex;
static SemaphoreHandle_t http_budget;
static weather_state_t state;
static weather_schedule_t schedule;
#if CONFIG_CLOCK_DIAGNOSTICS
static int next_fault;
static weather_data_t sample_original;
static bool sample_saved;
static bool tls_probe_pending,tls_probe_running;
static uint32_t tls_probe_id;
#endif
bool weather_http_take(uint32_t wait_ms) { return http_budget && xSemaphoreTake(http_budget,pdMS_TO_TICKS(wait_ms))==pdTRUE; }
void weather_http_give(void) { xSemaphoreGive(http_budget); }
typedef struct { double latitude,longitude;uint32_t generation;int fault;bool test;char timezone[WEATHER_ZONE_BYTES]; } job_t;
typedef struct {
    uint32_t generation;
    int32_t latitude_e4, longitude_e4;
    bool complete;
    met_forecast_t forecast;
    int64_t fetched_utc, not_before_utc;
    uint64_t fetched_ms;
    char modified[80];
} met_cache_t;
static met_cache_t *met_cache; /* Weather worker only; location changes use generation. */
static uint32_t met_requests;
extern const uint8_t _binary_weather_met_contact_bin_start[] __attribute__((weak));
extern const uint8_t _binary_weather_met_contact_bin_end[] __attribute__((weak));
static const char *met_contact(void) {
    const uint8_t *start=_binary_weather_met_contact_bin_start,*end=_binary_weather_met_contact_bin_end;
    uintptr_t a=(uintptr_t)start,b=(uintptr_t)end;
    if(!a||!b||b<=a||b-a>241||b-a<9)return NULL;
    size_t span=(size_t)(b-a);
    if(start[span-1]||!start[0])return NULL;
    return (const char *)start;
}
weather_state_t weather_read(void) {
    weather_state_t copy={.error=WX_UNCONFIGURED};
    if(mutex) { xSemaphoreTake(mutex,portMAX_DELAY); copy=state; copy.next_due_ms=schedule.due_ms; copy.failures=schedule.failures; xSemaphoreGive(mutex); }
    copy.backup_configured=met_contact()!=NULL&&met_cache!=NULL;
    return copy;
}
static bool set_location(double lat,double lon,const char *timezone,bool test) {
    char checked[WEATHER_URL_BYTES];
    if(!mutex||!weather_url(lat,lon,timezone,checked,sizeof checked))return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    uint32_t generation=state.generation+1; uint32_t discarded=state.discarded;
    state=(weather_state_t){.configured=true,.development_test=test,.latitude=lat,.longitude=lon,.generation=generation,.error=WX_OK,.discarded=discarded};
    strcpy(state.requested_timezone,timezone);
    weather_schedule_location(&schedule,true,generation,app_uptime());
#if CONFIG_CLOCK_DIAGNOSTICS
    next_fault=0;sample_saved=false;
#endif
    xSemaphoreGive(mutex); return true;
}
bool weather_set_location(double lat,double lon,const char *timezone){return set_location(lat,lon,timezone,false);}
void weather_clear_location(void) {
    if(!mutex) return;
    xSemaphoreTake(mutex,portMAX_DELAY);
    uint32_t generation=state.generation+1; uint32_t discarded=state.discarded;
    state=(weather_state_t){.generation=generation,.error=WX_UNCONFIGURED,.discarded=discarded};
    weather_schedule_location(&schedule,false,generation,app_uptime());
#if CONFIG_CLOCK_DIAGNOSTICS
    next_fault=0;sample_saved=false;
#endif
    xSemaphoreGive(mutex);
}
#if CONFIG_CLOCK_DIAGNOSTICS
static bool request(int fault) {
    if(!mutex) return false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    bool ok=state.configured;
    /* In-flight requests already cover a trigger: do not enqueue another. */
    if(ok && !schedule.in_flight) { schedule.due_ms=app_uptime(); next_fault=fault; }
    xSemaphoreGive(mutex); return ok;
}
#endif
typedef struct { char *body; size_t used; bool oversized,deadline,cancelled; uint64_t started,connected_ms,sent_ms,header_ms;
#if CONFIG_CLOCK_DIAGNOSTICS
    int last_rc,last_errno;
    uint32_t perform_calls;
    bool loop_deadline,first_socket,four_socket;
#endif
} body_t;
#if CONFIG_CLOCK_DIAGNOSTICS
typedef struct { int scope;uint32_t hash;char public_ip[INET_ADDRSTRLEN]; } diagnostic_address;
static diagnostic_address diagnostic_ip(const struct sockaddr *address,socklen_t length){
    diagnostic_address out={0};const uint8_t *bytes=NULL;size_t count=0;
    if(address&&address->sa_family==AF_INET&&length>=sizeof(struct sockaddr_in)){
        const struct sockaddr_in *v4=(const struct sockaddr_in *)address;
        bytes=(const uint8_t *)&v4->sin_addr;count=4;
        /* Only public addresses of this fixed provider may be printed. Local,
         * shared, documentation and other reserved ranges remain hashed. */
        bool public=bytes[0]&&bytes[0]<224&&bytes[0]!=10&&bytes[0]!=127&&
            !(bytes[0]==100&&bytes[1]>=64&&bytes[1]<=127)&&
            !(bytes[0]==169&&bytes[1]==254)&&!(bytes[0]==172&&bytes[1]>=16&&bytes[1]<=31)&&
            !(bytes[0]==192&&(bytes[1]==0||bytes[1]==168||(bytes[1]==88&&bytes[2]==99)))&&
            !(bytes[0]==198&&(bytes[1]==18||bytes[1]==19||(bytes[1]==51&&bytes[2]==100)))&&
            !(bytes[0]==203&&bytes[1]==0&&bytes[2]==113);
        out.scope=public?1:2;
        if(public)inet_ntop(AF_INET,&v4->sin_addr,out.public_ip,sizeof out.public_ip);
    }else if(address&&address->sa_family==AF_INET6&&length>=sizeof(struct sockaddr_in6)){
        bytes=(const uint8_t *)&((const struct sockaddr_in6 *)address)->sin6_addr;count=16;out.scope=3;
    }
    if(bytes){out.hash=2166136261u;for(size_t i=0;i<count;i++)out.hash=(out.hash^bytes[i])*16777619u;}
    return out;
}
static void diagnostic_socket(esp_http_client_handle_t client,const body_t *body,int point,bool final){
    int preserved_errno=errno;
    int fd=esp_http_client_get_socket(client),state=esp_http_client_get_state(client);
    /* This public getter also clears the transport's cached socket errno. */
    int transport_errno=final?esp_http_client_get_errno(client):-1,peer_rc=-1,peer_errno=0;
    int select_rc=-1,select_errno=0,write_ready=0,except_ready=0;
    int so_rc=-1,so_errno=0,so_error=0;
    struct sockaddr_storage peer={0};socklen_t peer_length=sizeof peer;
    if(fd>=0){
        peer_rc=getpeername(fd,(struct sockaddr *)&peer,&peer_length);peer_errno=peer_rc<0?errno:0;
        if(fd<FD_SETSIZE){
            fd_set writable,exceptional;FD_ZERO(&writable);FD_ZERO(&exceptional);FD_SET(fd,&writable);FD_SET(fd,&exceptional);
            struct timeval zero={0};select_rc=select(fd+1,NULL,&writable,&exceptional,&zero);select_errno=select_rc<0?errno:0;
            if(select_rc>=0){write_ready=FD_ISSET(fd,&writable);except_ready=FD_ISSET(fd,&exceptional);}
        }else select_errno=EINVAL;
        /* SO_ERROR can consume a pending error. Read it only when this fetch
         * has already stopped, immediately before cleanup; never mid-perform. */
        if(final){socklen_t size=sizeof so_error;so_rc=getsockopt(fd,SOL_SOCKET,SO_ERROR,&so_error,&size);so_errno=so_rc<0?errno:0;}
    }
    diagnostic_address ip=diagnostic_ip(peer_rc==0?(struct sockaddr *)&peer:NULL,peer_length);
    printf("{\"event\":\"weather_socket\",\"point\":%d,\"elapsed_ms\":%"PRIu64",\"http_state\":%d,\"fd\":%d,\"transport_errno\":%d,\"peer_rc\":%d,\"peer_errno\":%d,\"peer_scope\":%d,\"peer_ip\":\"%s\",\"peer_hash\":%"PRIu32",\"select_rc\":%d,\"select_errno\":%d,\"write_ready\":%d,\"except_ready\":%d,\"so_error_read\":%s,\"so_rc\":%d,\"so_errno\":%d,\"so_error\":%d}\n",
        point,app_uptime()-body->started,state,fd,transport_errno,peer_rc,peer_errno,ip.scope,ip.public_ip,ip.hash,select_rc,select_errno,write_ready,except_ready,final?"true":"false",so_rc,so_errno,so_error);
    /* These observational syscalls must not change the errno seen by the next
     * asynchronous HTTP perform call. getpeername alone is not TCP-established:
     * this lwIP also returns a remote peer while the PCB is SYN_SENT. */
    errno=preserved_errno;
}
/* Fixed development-only TLS handshakes: no HTTP request or location data.
 * State is sampled between public calls, not inside a blocking SDK call. */
static void tls_probe_step(uint32_t id,int axis,uint64_t batch_started){
    bool async=axis!=2;
    const char *host=axis==3?"www.baidu.com":"188.40.99.226";
    const char *name=axis==3?"www.baidu.com":"api.open-meteo.com";
    uint64_t started=app_uptime();uint32_t calls=0;
    int rc=-1,call_errno=0,last_state=-1,fd=-1;
    bool deadline=false;
    esp_tls_t *tls=esp_tls_init();
    esp_tls_cfg_t cfg={.timeout_ms=4000,.non_block=async,
        .crt_bundle_attach=esp_crt_bundle_attach,.skip_common_name=false,
        .common_name=name,.addr_family=ESP_TLS_AF_INET};
    printf("{\"event\":\"weather_tls_probe_step\",\"rid\":%"PRIu32",\"axis\":%d,\"async\":%s,\"internal8_free\":%u,\"internal8_largest\":%u,\"dma_free\":%u}\n",
        id,axis,async?"true":"false",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    if(tls){
        esp_tls_conn_state_t phase=ESP_TLS_INIT;
        do{
            if(app_uptime()-batch_started>=75000||(async&&app_uptime()-started>=25000)){deadline=true;break;}
            /* Sync is one SDK call with its original 4s configuration. The
             * batch checkpoint cannot preempt DNS or a synchronous SDK call. */
            rc=async?esp_tls_conn_new_async(host,strlen(host),443,&cfg,tls):
                esp_tls_conn_new_sync(host,strlen(host),443,&cfg,tls);
            call_errno=errno;calls++;
            if(esp_tls_get_conn_state(tls,&phase)==ESP_OK&&(int)phase!=last_state){
                last_state=phase;
                printf("{\"event\":\"weather_tls_probe_state\",\"rid\":%"PRIu32",\"axis\":%d,\"state\":%d,\"rc\":%d,\"calls\":%"PRIu32",\"elapsed_ms\":%"PRIu64"}\n",id,axis,last_state,rc,calls,app_uptime()-started);
            }
            if(async&&rc==0)vTaskDelay(pdMS_TO_TICKS(20));
        }while(async&&rc==0);
    }
    int tls_code=0,flags=0,error_rc=ESP_ERR_NO_MEM;
    esp_tls_error_handle_t error=NULL;
    struct sockaddr_storage peer={0};socklen_t length=sizeof peer;
    int peer_rc=-1,so_rc=-1,so_error=0;
    if(tls){
        esp_tls_get_conn_sockfd(tls,&fd);
        if(fd>=0){
            peer_rc=getpeername(fd,(struct sockaddr *)&peer,&length);
            socklen_t size=sizeof so_error;
            /* Final only: reading SO_ERROR may consume a pending error. */
            so_rc=getsockopt(fd,SOL_SOCKET,SO_ERROR,&so_error,&size);
        }
        if(esp_tls_get_error_handle(tls,&error)==ESP_OK)
            error_rc=esp_tls_get_and_clear_last_error(error,&tls_code,&flags);
    }
    diagnostic_address ip=diagnostic_ip(peer_rc==0?(struct sockaddr *)&peer:NULL,length);
    /* Only the already-approved public weather peer is printed. The fixed
     * control peer is classified/hashed, never a local address or gateway. */
    printf("{\"event\":\"weather_tls_probe_result\",\"rid\":%"PRIu32",\"axis\":%d,\"async\":%s,\"rc\":%d,\"call_errno\":%d,\"deadline\":%s,\"state\":%d,\"calls\":%"PRIu32",\"elapsed_ms\":%"PRIu64",\"error_rc\":%d,\"tls_error\":%d,\"verification_flags\":%d,\"fd\":%d,\"peer_rc\":%d,\"peer_scope\":%d,\"peer_ip\":\"%s\",\"peer_hash\":%"PRIu32",\"so_rc\":%d,\"so_error\":%d,\"internal8_free\":%u,\"internal8_largest\":%u,\"dma_free\":%u}\n",
        id,axis,async?"true":"false",rc,call_errno,deadline?"true":"false",last_state,calls,app_uptime()-started,error_rc,tls_code,flags,fd,peer_rc,ip.scope,axis==3?"":ip.public_ip,ip.hash,so_rc,so_error,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    int destroyed=tls?esp_tls_conn_destroy(tls):0;
    printf("{\"event\":\"weather_tls_probe_cleanup\",\"rid\":%"PRIu32",\"axis\":%d,\"destroy_rc\":%d,\"internal8_free\":%u,\"dma_free\":%u}\n",id,axis,destroyed,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
}
static void tls_probe(uint32_t id){
    uint64_t started=app_uptime();int completed=0;
    printf("{\"event\":\"weather_tls_probe_start\",\"rid\":%"PRIu32",\"uptime_ms\":%"PRIu64"}\n",id,started);
    bool budget=weather_http_take(30000);
    if(budget){
        for(int axis=1;axis<=3&&app_uptime()-started<75000;axis++){
            network_state_t net=app_network_read();
            if(!net.connected||!net.time_valid)break;
            tls_probe_step(id,axis,started);completed++;
        }
        weather_http_give();
    }
    printf("{\"event\":\"weather_tls_probe_done\",\"rid\":%"PRIu32",\"budget\":%s,\"completed\":%d,\"soft_limit_reached\":%s,\"elapsed_ms\":%"PRIu64"}\n",id,budget?"true":"false",completed,app_uptime()-started>=75000?"true":"false",app_uptime()-started);
}
#endif
static esp_err_t event(esp_http_client_event_t *e) {
    body_t *b=e->user_data;
#if CONFIG_CLOCK_DIAGNOSTICS
    if(e->event_id==HTTP_EVENT_ON_CONNECTED)b->connected_ms=app_uptime();
    if(e->event_id==HTTP_EVENT_HEADERS_SENT)b->sent_ms=app_uptime();
    if(e->event_id==HTTP_EVENT_ON_HEADER&&!b->header_ms)b->header_ms=app_uptime();
#endif
    if(app_uptime()-b->started>25000) b->deadline=true;
    if(e->event_id==HTTP_EVENT_ON_DATA && e->data_len>0) {
        size_t n=(size_t)e->data_len;
        if(n>WEATHER_BODY_LIMIT-b->used) b->oversized=true;
        else { memcpy(b->body+b->used,e->data,n); b->used+=n; b->body[b->used]=0; }
    }
    /* IDF ignores the event handler return value for data. Close the socket in
     * this same task instead; cleanup remains owned by fetch(). */
    if((b->deadline||b->oversized) && !b->cancelled &&
       (e->event_id==HTTP_EVENT_ON_DATA || e->event_id==HTTP_EVENT_ON_HEADER || e->event_id==HTTP_EVENT_ON_CONNECTED)) {
        b->cancelled=true; esp_http_client_close(e->client);
    }
    return ESP_OK;
}
static weather_error_t fetch(job_t job,weather_data_t *data,int *status,int *transport,int *bytes) {
    network_state_t net=app_network_read();
    if(!net.time_valid) return WX_CLOCK;
    if(!net.connected) return WX_OFFLINE;
    struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM},*address=NULL;
    int dns=getaddrinfo("api.open-meteo.com",NULL,&hints,&address);
#if CONFIG_CLOCK_DIAGNOSTICS
    int diagnostic_errno=errno;
    diagnostic_address target=diagnostic_ip(address?address->ai_addr:NULL,address?address->ai_addrlen:0);
    printf("{\"event\":\"weather_dns_target\",\"result\":%d,\"scope\":%d,\"public_ip\":\"%s\",\"address_hash\":%"PRIu32"}\n",dns,target.scope,target.public_ip,target.hash);
    errno=diagnostic_errno;
#endif
    if(address) freeaddrinfo(address);
    printf("{\"event\":\"weather_dns\",\"host\":\"api.open-meteo.com\",\"result\":%d,\"uptime_ms\":%"PRIu64"}\n",dns,app_uptime());
    if(dns) { *transport=dns; return WX_TRANSPORT; }
    char url[WEATHER_URL_BYTES];
    if(!weather_url(job.latitude,job.longitude,job.timezone,url,sizeof url))return WX_SCHEMA;
    /* CPU-only response data leaves internal memory available for networking. */
    body_t b={.body=heap_caps_calloc(WEATHER_BODY_LIMIT+1,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),.started=app_uptime()};
    if(!b.body) { *transport=ESP_ERR_NO_MEM; return WX_TRANSPORT; }
    esp_http_client_config_t cfg={.url=url,.timeout_ms=4000,.crt_bundle_attach=esp_crt_bundle_attach,
        .skip_cert_common_name_check=false,.disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=768,
        .event_handler=event,.user_data=&b,.is_async=true};
    esp_http_client_handle_t client=esp_http_client_init(&cfg);
    esp_err_t err=ESP_ERR_NO_MEM;
    if(client) do {
        if(app_uptime()-b.started>25000) { b.deadline=true; err=ESP_ERR_TIMEOUT;
#if CONFIG_CLOCK_DIAGNOSTICS
            b.loop_deadline=true;
#endif
            break; }
        err=esp_http_client_perform(client);
#if CONFIG_CLOCK_DIAGNOSTICS
        b.last_rc=err;b.last_errno=errno;b.perform_calls++;
        if(err==ESP_ERR_HTTP_EAGAIN){
            if(!b.first_socket){b.first_socket=true;b.four_socket=app_uptime()-b.started>=4000;diagnostic_socket(client,&b,1,false);}
            else if(!b.four_socket&&app_uptime()-b.started>=4000){b.four_socket=true;diagnostic_socket(client,&b,2,false);}
        }
#endif
        if(err==ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(20));
    } while(err==ESP_ERR_HTTP_EAGAIN && !b.oversized && !b.deadline);
    *status=client?esp_http_client_get_status_code(client):0; *transport=err; *bytes=b.used;
    weather_error_t result;
    if(b.oversized) result=WX_TOO_LARGE;
    else if(b.deadline || err==ESP_ERR_TIMEOUT || err==ESP_ERR_HTTP_EAGAIN) result=WX_TIMEOUT;
    else if(err==ESP_ERR_HTTP_READ_TIMEOUT) result=WX_TIMEOUT;
    else if(err==ESP_ERR_HTTP_INCOMPLETE_DATA || err==ESP_ERR_HTTP_CONNECTION_CLOSED) result=WX_TRUNCATED;
    else if(err!=ESP_OK) result=WX_TRANSPORT;
    else if(*status!=200) result=WX_HTTP;
    else if(!esp_http_client_is_complete_data_received(client)) result=WX_TRUNCATED;
    else result=WX_OK;
    if(client) {
#if CONFIG_CLOCK_DIAGNOSTICS
        diagnostic_socket(client,&b,3,true);
        const char *stop=b.oversized?"oversized":b.loop_deadline?"loop_deadline":b.deadline?"event_deadline":"perform_return";
        printf("{\"event\":\"weather_http_stop\",\"stop_reason\":\"%s\",\"deadline\":%s,\"cancelled\":%s,\"oversized\":%s,\"last_perform_rc\":%d,\"last_perform_errno\":%d,\"perform_calls\":%"PRIu32",\"final_rc\":%d,\"elapsed_ms\":%"PRIu64"}\n",stop,b.deadline?"true":"false",b.cancelled?"true":"false",b.oversized?"true":"false",b.last_rc,b.last_errno,b.perform_calls,err,app_uptime()-b.started);
#endif
        int tls=0,flags=0;
        esp_http_client_get_and_clear_last_tls_error(client,&tls,&flags);
        printf("{\"event\":\"weather_tls\",\"tls_error\":%d,\"verification_flags\":%d}\n",tls,flags);
#if CONFIG_CLOCK_DIAGNOSTICS
        printf("{\"event\":\"weather_phases\",\"started_ms\":%"PRIu64",\"connected_ms\":%"PRIu64",\"sent_ms\":%"PRIu64",\"header_ms\":%"PRIu64",\"internal8_free\":%u,\"internal8_largest\":%u,\"dma_free\":%u}\n",b.started,b.connected_ms,b.sent_ms,b.header_ms,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
#endif
        esp_http_client_cleanup(client);
    }
#if CONFIG_CLOCK_DIAGNOSTICS
    else printf("{\"event\":\"weather_http_stop\",\"stop_reason\":\"client_init_failed\",\"perform_calls\":0,\"final_rc\":%d,\"elapsed_ms\":%"PRIu64"}\n",err,app_uptime()-b.started);
#endif
    if(result==WX_OK) {
        result=weather_parse(b.body,b.used,app_utc_us()/1000000,app_uptime(),data);
#if CONFIG_CLOCK_DIAGNOSTICS
        if((job.test || location_public_test()) && (result==WX_OK || result==WX_PARTIAL)) {
            flockfile(stdout);
            printf("{\"event\":\"weather_payload\",\"development_test\":true,\"provider_response\":%s}\n",b.body);
            funlockfile(stdout);
        }
#endif
    }
    free(b.body); return result;
}
#define MET_COMPRESSED_LIMIT (64u*1024u)
typedef struct {
    char *body;
    size_t input, used;
    uint64_t started;
    bool gzip, encoding_seen, unsupported, oversized, invalid, deadline, cancelled, zinit, zend;
    z_stream z;
    char modified[80], expires[80], retry[80], location[WEATHER_URL_BYTES];
    met_forecast_t parsed;
} met_response_t;
static voidpf met_zalloc(voidpf opaque,uInt items,uInt size) {
    (void)opaque;
    if(items && size>SIZE_MAX/items)return NULL;
    return heap_caps_calloc(items,size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
}
static void met_zfree(voidpf opaque,voidpf pointer) {(void)opaque;free(pointer);}
static void header_copy(char *to,size_t capacity,const char *from) {
    size_t n=from?strlen(from):0;
    if(!n||n>=capacity)return;
    for(size_t i=0;i<n;i++)if((unsigned char)from[i]<32||(unsigned char)from[i]>126)return;
    memcpy(to,from,n+1);
}
static esp_err_t met_event(esp_http_client_event_t *e) {
    met_response_t *r=e->user_data;
    if(app_uptime()-r->started>25000)r->deadline=true;
    if(e->event_id==HTTP_EVENT_ON_HEADER&&e->header_key&&e->header_value) {
        if(!strcasecmp(e->header_key,"Content-Encoding")) {
            if(r->encoding_seen)r->unsupported=true;
            r->encoding_seen=true;
            if(!strcasecmp(e->header_value,"gzip"))r->gzip=true;
            else if(strcasecmp(e->header_value,"identity"))r->unsupported=true;
        } else if(!strcasecmp(e->header_key,"Last-Modified"))header_copy(r->modified,sizeof r->modified,e->header_value);
        else if(!strcasecmp(e->header_key,"Expires"))header_copy(r->expires,sizeof r->expires,e->header_value);
        else if(!strcasecmp(e->header_key,"Retry-After"))header_copy(r->retry,sizeof r->retry,e->header_value);
        else if(!strcasecmp(e->header_key,"Location"))header_copy(r->location,sizeof r->location,e->header_value);
    }
    if(e->event_id==HTTP_EVENT_ON_DATA&&e->data_len>0&&!r->invalid&&!r->oversized&&!r->unsupported) {
        size_t n=(size_t)e->data_len;
        if(n>MET_COMPRESSED_LIMIT-r->input)r->oversized=true;
        else {
            r->input+=n;
            if(r->gzip) {
                if(r->zend)r->invalid=true; /* Reject trailing bytes and concatenated members. */
                else {
                    if(!r->zinit) {
                        r->z.zalloc=met_zalloc;r->z.zfree=met_zfree;
                        r->zinit=inflateInit2(&r->z,MAX_WBITS+16)==Z_OK;
                        if(!r->zinit)r->invalid=true;
                    }
                    if(r->zinit) {
                        r->z.next_in=(Bytef *)e->data;r->z.avail_in=(uInt)n;
                        do {
                            r->z.next_out=(Bytef *)r->body+r->used;
                            r->z.avail_out=(uInt)(MET_FORECAST_BODY_LIMIT+1-r->used);
                            int z=inflate(&r->z,Z_NO_FLUSH);r->used=(size_t)r->z.total_out;
                            if(r->used>MET_FORECAST_BODY_LIMIT){r->oversized=true;break;}
                            if(z==Z_STREAM_END){r->zend=true;if(r->z.avail_in)r->invalid=true;break;}
                            if(z!=Z_OK&&!(z==Z_BUF_ERROR&&!r->z.avail_in)){r->invalid=true;break;}
                        }while(r->z.avail_in);
                    }
                }
            } else if(n>MET_FORECAST_BODY_LIMIT-r->used)r->oversized=true;
            else {memcpy(r->body+r->used,e->data,n);r->used+=n;}
            if(r->used<=MET_FORECAST_BODY_LIMIT)r->body[r->used]=0;
        }
    }
    if((r->deadline||r->oversized||r->invalid||r->unsupported)&&!r->cancelled&&
        (e->event_id==HTTP_EVENT_ON_DATA||e->event_id==HTTP_EVENT_ON_HEADER||e->event_id==HTTP_EVENT_ON_CONNECTED)) {
        r->cancelled=true;esp_http_client_close(e->client);
    }
    return ESP_OK;
}
/* HTTP dates are metadata only; no global TZ mutation or local-time mktime. */
static int64_t http_date(const char *s) {
    /* The provider sends IMF-fixdate; reject signs, variable-width numbers and
     * trailing fields rather than interpreting malformed cache instructions. */
    if(strlen(s)!=29||s[3]!=','||s[4]!=' '||s[7]!=' '||s[11]!=' '||s[16]!=' '||
        s[19]!=':'||s[22]!=':'||s[25]!=' '||memcmp(s+26,"GMT",3))return 0;
    static const unsigned places[]={5,6,12,13,14,15,17,18,20,21,23,24};
    for(unsigned i=0;i<sizeof places/sizeof places[0];i++)if(s[places[i]]<'0'||s[places[i]]>'9')return 0;
    static const char *weekdays[]={"Mon","Tue","Wed","Thu","Fri","Sat","Sun"};
    bool weekday=false;for(unsigned i=0;i<7;i++)if(!memcmp(s,weekdays[i],3))weekday=true;
    if(!weekday)return 0;
    int d=(s[5]-'0')*10+s[6]-'0',y=(s[12]-'0')*1000+(s[13]-'0')*100+(s[14]-'0')*10+s[15]-'0';
    int h=(s[17]-'0')*10+s[18]-'0',m=(s[20]-'0')*10+s[21]-'0',sec=(s[23]-'0')*10+s[24]-'0';
    if(y<2020||y>2100||d<1||d>31||h>23||m>59||sec>59)return 0;
    static const char *months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int mo=0;while(mo<12&&memcmp(s+8,months[mo],3))mo++;if(mo==12)return 0;
    static const int lengths[]={31,28,31,30,31,30,31,31,30,31,30,31};
    int leap=y%4==0&&(y%100!=0||y%400==0);
    if(d>lengths[mo]+(mo==1&&leap))return 0;
    int mon=mo+1; y-=mon<=2;int era=y/400;unsigned yo=(unsigned)(y-era*400);
    unsigned doy=(153*(mon+(mon>2?-3:9))+2)/5+(unsigned)d-1;
    return ((int64_t)era*146097+yo*365+yo/4-yo/100+doy-719468)*86400+h*3600+m*60+sec;
}
static int64_t retry_time(const char *s,int64_t now) {
    if(!*s)return 0;
    uint64_t value=0;bool numeric=true;
    for(const unsigned char *p=(const unsigned char *)s;*p;p++) {
        if(!isdigit(*p)){numeric=false;break;}
        if(value>(UINT32_MAX-(*p-'0'))/10)return 0;
        value=value*10+*p-'0';
    }
    return numeric?now+(int64_t)value:http_date(s);
}
static bool met_redirect(const char *location,char *out,size_t capacity) {
    const char *prefix="https://api.met.no/";
    for(const unsigned char *p=(const unsigned char *)location;*p;p++)if(*p<=32||*p>=127||*p=='\\')return false;
    int n;
    if(!strncmp(location,prefix,strlen(prefix)))n=snprintf(out,capacity,"%s",location);
    else if(location[0]=='/'&&location[1]!='/')n=snprintf(out,capacity,"https://api.met.no%s",location);
    else return false;
    return n>0&&(size_t)n<capacity;
}
static void met_reset_generation(uint32_t generation) {
    if(met_cache&&met_cache->generation!=generation) {
        memset(met_cache,0,sizeof *met_cache);met_cache->generation=generation;
    }
}
static bool met_select(int64_t utc,uint64_t ms,weather_current_t *out) {
    if(!met_cache||!met_cache->complete||!met_forecast_select(&met_cache->forecast,utc,met_cache->fetched_utc,met_cache->fetched_ms,out))return false;
    out->provider=WX_PROVIDER_MET;out->model_updated_utc=met_cache->forecast.model_updated_utc;
    return weather_current_valid(out,utc,ms,true);
}
static weather_error_t met_fetch(job_t job,weather_current_t *out,int *status,int *transport,int *bytes,bool *cached) {
    *status=*transport=*bytes=0;*cached=false;
    if(!met_contact()||!met_cache)return WX_UNCONFIGURED;
    met_reset_generation(job.generation);
    int32_t lat_e4=(int32_t)trunc(job.latitude*10000.0),lon_e4=(int32_t)trunc(job.longitude*10000.0);
    if(met_cache->latitude_e4!=lat_e4||met_cache->longitude_e4!=lon_e4) {
        memset(met_cache,0,sizeof *met_cache);met_cache->generation=job.generation;
        met_cache->latitude_e4=lat_e4;met_cache->longitude_e4=lon_e4;
    }
    int64_t utc=app_utc_us()/1000000;uint64_t now=app_uptime();
    if(utc<met_cache->not_before_utc) {
        *cached=met_select(utc,now,out);
        return *cached?WX_OK:WX_HTTP;
    }
    met_response_t *r=heap_caps_calloc(1,sizeof *r,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!r){*transport=ESP_ERR_NO_MEM;return WX_TRANSPORT;}
    r->body=heap_caps_calloc(MET_FORECAST_BODY_LIMIT+1,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!r->body){free(r);*transport=ESP_ERR_NO_MEM;return WX_TRANSPORT;}
    char url[WEATHER_URL_BYTES];
    /* MET explicitly requires truncation, not rounding, to four decimals. */
    double lat=lat_e4/10000.0,lon=lon_e4/10000.0;
    snprintf(url,sizeof url,"https://api.met.no/weatherapi/locationforecast/2.0/compact?lat=%.4f&lon=%.4f",lat,lon);
    uint64_t started=app_uptime();weather_error_t result=WX_TRANSPORT;
    for(int redirect=0;redirect<2;redirect++) {
        char *body=r->body;memset(r,0,sizeof *r);r->body=body;r->started=started;
        esp_http_client_config_t cfg={.url=url,.timeout_ms=4000,.crt_bundle_attach=esp_crt_bundle_attach,
            .skip_cert_common_name_check=false,.disable_auto_redirect=true,.buffer_size=1024,.buffer_size_tx=768,
            .event_handler=met_event,.user_data=r,.is_async=true};
        esp_http_client_handle_t client=esp_http_client_init(&cfg);esp_err_t error=ESP_ERR_NO_MEM;
        if(client) {
            bool headers=esp_http_client_set_header(client,"User-Agent",met_contact())==ESP_OK&&
                esp_http_client_set_header(client,"Accept-Encoding","gzip")==ESP_OK;
            bool covered=false;
            for(size_t i=0;met_cache->complete&&i<met_cache->forecast.count;i++) {
                int64_t point=met_cache->forecast.points[i].source_utc;
                if(utc>=point&&utc-point<3600){covered=true;break;}
            }
            /* A 304 cannot refill an exhausted 24-hour subset. In that case
             * obtain a whole representation at the next permitted cycle. */
            if(headers&&covered&&met_cache->modified[0])headers=esp_http_client_set_header(client,"If-Modified-Since",met_cache->modified)==ESP_OK;
            if(headers) {
                met_requests++;
                do {
                    if(app_uptime()-started>25000){r->deadline=true;error=ESP_ERR_TIMEOUT;break;}
                    error=esp_http_client_perform(client);
                    if(error==ESP_ERR_HTTP_EAGAIN)vTaskDelay(pdMS_TO_TICKS(20));
                }while(error==ESP_ERR_HTTP_EAGAIN&&!r->deadline&&!r->oversized&&!r->invalid&&!r->unsupported);
            }
        }
        *status=client?esp_http_client_get_status_code(client):0;*transport=error;*bytes=(int)r->input;
        bool complete=client&&esp_http_client_is_complete_data_received(client);
        if(r->zinit)inflateEnd(&r->z);
        if(client)esp_http_client_cleanup(client); /* Release TLS before cJSON entry parsing. */
        utc=app_utc_us()/1000000;
        if(*status==200||*status==304)met_cache->not_before_utc=http_date(r->expires);
        if(*status==429||*status==503) {
            int64_t retry=retry_time(r->retry,utc),expires=http_date(r->expires);
            met_cache->not_before_utc=retry>expires?retry:expires;
        }
        if(r->oversized)result=WX_TOO_LARGE;
        else if(r->deadline||error==ESP_ERR_TIMEOUT||error==ESP_ERR_HTTP_EAGAIN||error==ESP_ERR_HTTP_READ_TIMEOUT)result=WX_TIMEOUT;
        else if(r->invalid||r->unsupported)result=WX_TRUNCATED;
        else if(error!=ESP_OK)result=WX_TRANSPORT;
        else if(!complete)result=WX_TRUNCATED;
        else if((*status==301||*status==302||*status==307||*status==308)&&redirect==0&&met_redirect(r->location,url,sizeof url))continue;
        else if(*status==304) {
            if(met_cache->complete) {
                weather_current_t selected;
                if(met_forecast_select(&met_cache->forecast,utc,utc,app_uptime(),&selected)) {
                    met_cache->fetched_utc=utc;met_cache->fetched_ms=app_uptime();
                    met_cache->not_before_utc=http_date(r->expires);
                    if(http_date(r->modified))memcpy(met_cache->modified,r->modified,sizeof met_cache->modified);
                    result=met_select(utc,app_uptime(),out)?WX_OK:WX_SCHEMA;
                } else result=WX_SCHEMA;
            } else result=WX_SCHEMA;
        } else if(*status!=200) {
            result=WX_HTTP;
        } else if(r->gzip&&!r->zend)result=WX_TRUNCATED;
        else {
            result=met_forecast_parse(r->body,r->used,utc,&r->parsed);
            weather_current_t selected;
            if(result==WX_OK&&!met_forecast_select(&r->parsed,utc,utc,app_uptime(),&selected))result=WX_SCHEMA;
            if(result==WX_OK) {
                met_cache->forecast=r->parsed;met_cache->complete=true;
                met_cache->fetched_utc=utc;met_cache->fetched_ms=app_uptime();
                met_cache->not_before_utc=http_date(r->expires);
                if(http_date(r->modified))memcpy(met_cache->modified,r->modified,sizeof met_cache->modified);
                else met_cache->modified[0]=0;
                result=met_select(utc,app_uptime(),out)?WX_OK:WX_SCHEMA;
            }
        }
        break;
    }
    free(r->body);free(r);return result;
}
static void service(void *unused) {
    (void)unused; job_t job;
    for(;;) {
        network_state_t net=app_network_read();
        xSemaphoreTake(mutex,portMAX_DELAY);
        /* Select a forecast hour already received, without a new HTTP request
         * or a new fetched timestamp when the clock crosses an hour. */
        if(met_cache&&met_cache->generation==state.generation&&!state.injected&&
            state.data.current.provider==WX_PROVIDER_MET&&net.time_valid) {
            weather_current_t selected;
            if(met_select(app_utc_us()/1000000,app_uptime(),&selected))state.data.current=selected;
        }
#if CONFIG_CLOCK_DIAGNOSTICS
        if(tls_probe_pending){
            uint32_t id=tls_probe_id;tls_probe_pending=false;tls_probe_running=true;
            xSemaphoreGive(mutex);tls_probe(id);
            xSemaphoreTake(mutex,portMAX_DELAY);tls_probe_running=false;xSemaphoreGive(mutex);
            continue;
        }
#endif
        bool claimed=weather_schedule_claim(&schedule,app_uptime(),net.connected,net.time_valid);
        if(claimed) {
            job=(job_t){.latitude=state.latitude,.longitude=state.longitude,.generation=state.generation,.test=state.development_test};
            strcpy(job.timezone,state.requested_timezone);
#if CONFIG_CLOCK_DIAGNOSTICS
            job.fault=next_fault; next_fault=0;
#endif
            state.fetching=true; state.attempted_ms=app_uptime(); state.issued++;
        }
        xSemaphoreGive(mutex);
        if(!claimed) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
        weather_data_t data={0}; int http=0,transport=0,bytes=0;
        weather_error_t result,primary_error=WX_UNCONFIGURED,backup_error=WX_UNCONFIGURED;
        int primary_http=0,backup_http=0;bool backup_cached=false,backup_attempted=false;
        uint32_t requests_before=met_requests;
        xSemaphoreTake(mutex,portMAX_DELAY);
        bool current=job.generation==state.generation;
        xSemaphoreGive(mutex);
        if(!current) {
            xSemaphoreTake(mutex,portMAX_DELAY); weather_schedule_finish(&schedule,job.generation,app_uptime(),false);state.discarded++;xSemaphoreGive(mutex);
            continue;
        }
#if CONFIG_CLOCK_DIAGNOSTICS
        if(job.fault&&job.fault!=6) {
            /* Explicit outcome injection, no request and no fabricated weather. */
            static const weather_error_t faults[]={WX_OK,WX_HTTP,WX_JSON,WX_TRUNCATED,WX_TIMEOUT,WX_TOO_LARGE};
            result=faults[job.fault]; if(job.fault==1) http=503;
            primary_error=result;primary_http=http;
        } else
#endif
        {
            if(weather_http_take(30000)) {
                xSemaphoreTake(mutex,portMAX_DELAY);current=job.generation==state.generation;xSemaphoreGive(mutex);
                if(current) {
                    if(job.fault==6){result=WX_HTTP;http=503;}
                    else result=fetch(job,&data,&http,&transport,&bytes);
                    primary_error=result;primary_http=http;
                    int64_t utc=app_utc_us()/1000000;uint64_t ms=app_uptime();
                    bool current_valid=weather_current_valid(&data.current,utc,ms,true);
                    if(!current_valid) {
                        data.current.present=false;
                        xSemaphoreTake(mutex,portMAX_DELAY);current=job.generation==state.generation;xSemaphoreGive(mutex);
                        if(current&&met_contact()&&met_cache) {
                            weather_current_t selected;
                            backup_error=met_fetch(job,&selected,&backup_http,&transport,&bytes,&backup_cached);
                            backup_attempted=met_requests!=requests_before;
                            http=backup_http;
                            if(backup_error==WX_OK&&weather_current_valid(&selected,app_utc_us()/1000000,app_uptime(),true)) {
                                data.current=selected;current_valid=true;
                            }
                        }
                    }
                    weather_forecast_t summary;weather_forecast_summary(&data,app_utc_us()/1000000,app_uptime(),true,&summary);
                    if(current_valid||summary.usable_count)result=current_valid&&summary.usable_count==7?WX_OK:WX_PARTIAL;
                    else if(backup_attempted||backup_cached)result=backup_error;
                    else if(result==WX_OK||result==WX_PARTIAL)result=WX_SCHEMA;
                } else result=WX_UNCONFIGURED;
                weather_http_give();
            }
            else result=WX_TIMEOUT;
        }
        weather_provider_t current_provider=WX_PROVIDER_UNKNOWN;
        xSemaphoreTake(mutex,portMAX_DELAY);
        weather_schedule_finish(&schedule,job.generation,app_uptime(),result==WX_OK||result==WX_PARTIAL);
        if(job.generation==state.generation) {
            state.attempts++; state.fetching=false; state.finished_ms=app_uptime();
            state.error=result; state.http_status=http; state.transport_error=transport; state.bytes=bytes;
            state.primary_http=primary_http;state.backup_http=backup_http;
            state.primary_error=primary_error;state.backup_error=backup_error;
            state.backup_attempted=backup_attempted;state.backup_cached=backup_cached;state.primary_injected=job.fault==6;
            if(result==WX_OK || result==WX_PARTIAL) {
                if(!data.timezone[0]&&!state.data.timezone[0]&&!strcmp(job.timezone,"Asia/Shanghai")) {
                    strcpy(data.timezone,"Asia/Shanghai");strcpy(data.abbreviation,"CST");data.offset_s=28800;
                }
                if(data.current.present&&(!weather_current_valid(&state.data.current,app_utc_us()/1000000,app_uptime(),true)||
                    state.data.current.provider!=data.current.provider))state.data.current.present=false;
                weather_merge(&state.data,&data); state.successes++;
            }
            if(state.data.current.present)current_provider=state.data.current.provider;
        } else state.discarded++;
        xSemaphoreGive(mutex);
        flockfile(stdout);
        printf("{\"event\":\"weather_attempt\",\"generation\":%"PRIu32",\"development_test\":%s,\"injected\":%s,\"primary_injected\":%s,\"primary_http\":%d,\"primary_result\":\"%s\",\"backup_http\":%d,\"backup_result\":\"%s\",\"backup_attempted\":%s,\"backup_cached\":%s,\"current_source\":\"%s\",\"direct_device_https\":%s,\"certificate_bundle\":true,\"hostname_and_dates_verified\":%s,\"http_status\":%d,\"transport_error\":%d,\"bytes\":%d,\"result\":\"%s\",\"uptime_ms\":%"PRIu64"}\n",
            job.generation,job.test?"true":"false",job.fault&&job.fault!=6?"true":"false",job.fault==6?"true":"false",
            primary_http,weather_error_name(primary_error),backup_http,weather_error_name(backup_error),
            backup_attempted?"true":"false",backup_cached?"true":"false",weather_provider_name(current_provider),
            ((primary_http>0&&job.fault==0)||backup_attempted)?"true":"false",((primary_http>0&&job.fault==0)||backup_http>0)?"true":"false",
            http,transport,bytes,weather_error_name(result),app_uptime());
        funlockfile(stdout);
    }
}
void weather_status(void) {
    weather_state_t w=weather_read(); network_state_t n=app_network_read();
    bool disclose=w.development_test;
#if CONFIG_CLOCK_DIAGNOSTICS
    disclose=disclose||location_public_test();
#endif
    int64_t utc=app_utc_us()/1000000; uint64_t ms=app_uptime(); char today[11]="";
    if(n.time_valid && w.data.timezone[0]) weather_local_date(utc,w.data.offset_s,today);
    printf(",\"weather\":{\"source\":\"%s\",\"model_data\":true,\"configured\":%s,\"development_test\":%s,\"fetching\":%s,\"generation\":%"PRIu32",\"attempts\":%"PRIu32",\"successes\":%"PRIu32",\"discarded\":%"PRIu32",\"error\":\"%s\",\"http\":%d,\"bytes\":%d,\"timezone\":\"%s\",\"abbreviation\":\"%s\",\"offset_s\":%d,\"today\":\"%s\",\"grid_latitude\":%.4f,\"grid_longitude\":%.4f",
        weather_provider_name(w.data.current.provider),w.configured?"true":"false",w.development_test?"true":"false",w.fetching?"true":"false",w.generation,w.attempts,w.successes,w.discarded,weather_error_name(w.error),w.http_status,w.bytes,disclose?w.data.timezone:"",disclose?w.data.abbreviation:"",disclose?w.data.offset_s:0,disclose?today:"",disclose?w.data.grid_latitude:0,disclose?w.data.grid_longitude:0);
    printf(",\"grid_present\":%s,\"grid_source\":\"%s\",\"primary_http\":%d,\"primary_result\":\"%s\",\"backup_configured\":%s,\"backup_http\":%d,\"backup_result\":\"%s\",\"backup_attempted\":%s,\"backup_cached\":%s,\"primary_injected\":%s",w.data.grid_present?"true":"false",w.data.grid_present?"Open-Meteo":"unknown",w.primary_http,weather_error_name(w.primary_error),w.backup_configured?"true":"false",w.backup_http,weather_error_name(w.backup_error),w.backup_attempted?"true":"false",w.backup_cached?"true":"false",w.primary_injected?"true":"false");
    if(w.development_test) printf(",\"test_location\":\"Shanghai-public-development-test\",\"latitude\":%.4f,\"longitude\":%.4f",w.latitude,w.longitude);
    weather_current_t *c=&w.data.current;
    printf(",\"issued\":%"PRIu32",\"next_due_ms\":%"PRIu64",\"consecutive_failures\":%u,\"injected\":%s",w.issued,w.next_due_ms,w.failures,w.injected?"true":"false");
    printf(",\"current\":{\"source\":\"%s\",\"model_updated_utc\":%"PRId64",\"present\":%s,\"valid\":%s,\"code\":%d,\"kind\":\"%s\",\"temperature_c\":%.2f,\"source_utc\":%"PRId64",\"interval_s\":%d,\"fetched_utc\":%"PRId64",\"fetched_ms\":%"PRIu64",\"wind_present\":%s,\"wind_mps\":%.3f,\"wind_grade\":%d},\"daily\":[",
        weather_provider_name(c->provider),c->model_updated_utc,c->present?"true":"false",weather_current_valid(c,utc,ms,n.time_valid)?"true":"false",c->code,c->present?weather_kind_name(c->code):"unknown",c->temperature_c,c->source_utc,c->interval_s,c->fetched_utc,c->fetched_ms,c->wind_present?"true":"false",c->wind_mps,c->wind_present?weather_wind_grade(c->wind_mps):-1);
    for(int i=0;i<7;i++) { weather_day_t *d=&w.data.days[i];
        bool date_ok=today[0] && strcmp(d->date,today)>=0;
        printf("%s{\"date\":\"%s\",\"source\":\"%s\",\"present\":%s,\"valid\":%s,\"today\":%s,\"code\":%d,\"kind\":\"%s\",\"high_c\":%.2f,\"low_c\":%.2f,\"precipitation_present\":%s,\"precipitation_pct\":%.2f,\"fetched_utc\":%"PRId64",\"wind_present\":%s,\"wind_mps\":%.3f,\"wind_grade\":%d,\"uv_present\":%s,\"uv_index\":%.2f}",i?",":"",d->date,weather_provider_name(d->provider),d->present?"true":"false",date_ok&&weather_day_valid(d,utc,ms,n.time_valid)?"true":"false",d->present&&today[0]&&strcmp(d->date,today)==0?"true":"false",d->code,d->present?weather_kind_name(d->code):"unknown",d->high_c,d->low_c,d->precipitation_present?"true":"false",d->precipitation_pct,d->fetched_utc,d->wind_present?"true":"false",d->wind_mps,d->wind_present?weather_wind_grade(d->wind_mps):-1,d->uv_present?"true":"false",d->uv_index);
    }
    printf("]}");
}
#if CONFIG_CLOCK_DIAGNOSTICS
int weather_command(const char *line,uint32_t *rid) {
    unsigned id; char op[24],action[24],extra; int fault;
    if(sscanf(line,"%u %23s",&id,op)!=2 || strcmp(op,"weather")) return 0;
    *rid=id;
    int count=sscanf(line,"%u %23s %23s %c",&id,op,action,&extra);
    if(count==3) {
        if(!strcmp(action,"tls-probe")){
            if(!mutex)return -1;
            network_state_t net=app_network_read();
            xSemaphoreTake(mutex,portMAX_DELAY);
            bool accepted=net.connected&&net.time_valid&&!tls_probe_pending&&!tls_probe_running&&!schedule.in_flight;
            if(accepted){tls_probe_id=id;tls_probe_pending=true;}
            xSemaphoreGive(mutex);return accepted?1:-1;
        }
        if(!strcmp(action,"test"))return set_location(31.2304,121.4737,"Asia/Shanghai",true)?1:-1;
        if(!strcmp(action,"clear")) { weather_clear_location(); return 1; }
        if(!strcmp(action,"fetch")) return request(0)?1:-1;
        if(!strcmp(action,"failover-test"))return met_contact()&&met_cache&&request(6)?1:-1;
        if(!strcmp(action,"stale")) {
            xSemaphoreTake(mutex,portMAX_DELAY);
            state.data.current.source_utc-=WEATHER_CURRENT_MAX_AGE+1;
            for(int i=0;i<7;i++) state.data.days[i].fetched_utc-=WEATHER_DAILY_MAX_AGE+1;
            xSemaphoreGive(mutex); return 1;
        }
    }
    if(sscanf(line,"%u %23s %23s %d %c",&id,op,action,&fault,&extra)==4 && !strcmp(action,"fault") && fault>=1 && fault<=5) return request(fault)?1:-1;
    if(sscanf(line,"%u %23s %23s %d %c",&id,op,action,&fault,&extra)==4 && !strcmp(action,"sample") && fault>=0 && fault<=3) {
        /* Explicit, RAM-only public-city rendering mutations; restores original. */
        if(!location_public_test())return -1;
        xSemaphoreTake(mutex,portMAX_DELAY);
        if(state.fetching||!state.successes) {xSemaphoreGive(mutex);return -1;}
        if(!sample_saved){sample_original=state.data;sample_saved=true;}
        state.data=sample_original;state.injected=fault!=0;
        if(fault==1)state.data.current.source_utc-=WEATHER_CURRENT_MAX_AGE+1;
        if(fault==2){state.data.days[0].precipitation_present=false;state.data.days[1].present=false;state.data.days[2].code=999;state.data.current.code=999;}
        if(fault==3){state.data.current.source_utc-=WEATHER_CURRENT_MAX_AGE+1;for(int i=0;i<7;i++)state.data.days[i].fetched_utc-=WEATHER_DAILY_MAX_AGE+1;}
        xSemaphoreGive(mutex);return 1;
    }
    return -1;
}
#endif
void weather_start(void) {
    mutex=xSemaphoreCreateMutex(); http_budget=xSemaphoreCreateMutex(); configASSERT(mutex&&http_budget);
    state.error=WX_UNCONFIGURED;
    met_cache=heap_caps_calloc(1,sizeof *met_cache,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    configASSERT(xTaskCreate(service,"weather",12288,NULL,3,NULL)==pdPASS);
}
