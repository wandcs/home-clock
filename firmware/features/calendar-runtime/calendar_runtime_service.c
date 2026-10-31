#include "calendar_runtime_core.h"
#include "calendar_http.h"
#include "app_state.h"
#include "weather_service.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "psa/crypto.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM || !CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY || !CONFIG_SPIRAM_XIP_FROM_PSRAM || !CONFIG_SPIRAM_FETCH_INSTRUCTIONS || !CONFIG_SPIRAM_RODATA
#error "calendar PSRAM worker requires reviewed external-stack and XiP configuration"
#endif
enum { CR_WORKER_STACK=8192 };
typedef struct {
    uint8_t body[CC_BODY_LIMIT],wire[AF_YEAR_BYTES],digest[32];
    cc_candidate base,adjacent;psa_hash_operation_t hash,base_hash;
} workspace;
typedef struct {cr_core core;workspace work;} storage;
/* ABI evidence only; no dynamic or large static workspace allocation here. */
const uint32_t cr_service_sizes[]={sizeof(storage),sizeof(workspace),sizeof(StaticTask_t),sizeof(portMUX_TYPE)};
static storage *memory;
static cr_snapshot published;
static portMUX_TYPE mux=portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t worker;
static cr_audio_busy_fn audio_busy;
static bool started,launching;
static uint32_t stack_min;
static void publish(void) {
    portENTER_CRITICAL(&mux);published=memory->core.view;portEXIT_CRITICAL(&mux);
}
bool calendar_runtime_copy_into(cr_snapshot *out) {
    if(!out)return false;
    portENTER_CRITICAL(&mux);
    bool ok=started;if(ok)*out=published;
    portEXIT_CRITICAL(&mux);return ok;
}
static cr_input inputs(void) {
    network_state_t n=app_network_read();cr_input in={.now_ms=app_uptime()};
    if(!cr_input_from_utc(n.connected,n.time_valid,app_utc_us(),in.now_ms,&in))in=(cr_input){.online=n.connected,.now_ms=in.now_ms};
    return in;
}
static cc_result fetch(uint16_t title,const cr_input *in,cc_candidate *out) {
    cc_http_info h={0};cc_result result=cc_http_candidate(title,in->online,in->trusted,memory->work.body,CC_BODY_LIMIT,out,&h);
    /* Provider is immutable in the reviewed client. Never log body or location. */
    flockfile(stdout);
    printf("{\"event\":\"calendar_http\",\"title_year\":%u,\"host\":\"raw.githubusercontent.com\",\"http\":%d,\"transport\":%d,\"bytes\":%u,\"result\":%u,\"elapsed_ms\":%"PRIu64",\"strict_tls_client\":true,\"direct_device_attempt\":true}\n",title,h.status,h.transport,(unsigned)h.bytes,(unsigned)result,h.elapsed_ms);
    funlockfile(stdout);
    if(result==CC_OK) {
        if(psa_hash_update(&memory->work.hash,memory->work.body,h.bytes)!=PSA_SUCCESS)return CC_STORAGE;
    }
    return result;
}
static cc_result collect(uint16_t year,const cr_input *in,cc_result *adjacent) {
    workspace *w=&memory->work;*adjacent=CC_ARGUMENT; /* not requested */
    /* The existing provider/parser bounds end at title2099. Do not pretend a
     * never-requested2100 response was an explicit404 or empty announcement. */
    if(year>=2099u)return CC_DATE;
    w->hash=psa_hash_operation_init();w->base_hash=psa_hash_operation_init();
    cc_result result=CC_STORAGE;
    if(psa_crypto_init()!=PSA_SUCCESS||psa_hash_setup(&w->hash,PSA_ALG_SHA_256)!=PSA_SUCCESS)goto done;
    result=fetch(year,in,&w->base);
    if(result!=CC_OK)goto done;
    /* Snapshot the base hash operation. Empty/404 adjacent sources must use
     * the base-only digest; complete adjacent JSON extends rawY||0||rawY+1. */
    const uint8_t delimiter=0;
    if(psa_hash_clone(&w->hash,&w->base_hash)!=PSA_SUCCESS){result=CC_STORAGE;goto done;}
    if(psa_hash_update(&w->hash,&delimiter,1)!=PSA_SUCCESS){result=CC_STORAGE;goto done;}
    *adjacent=fetch((uint16_t)(year+1u),in,&w->adjacent);
    if(*adjacent!=CC_OK&&*adjacent!=CC_EMPTY&&*adjacent!=CC_NOT_FOUND){result=*adjacent;goto done;}
    size_t size=0;
    psa_hash_operation_t *selected=*adjacent==CC_OK?&w->hash:&w->base_hash;
    if(psa_hash_finish(selected,w->digest,sizeof w->digest,&size)!=PSA_SUCCESS||size!=sizeof w->digest){result=CC_STORAGE;goto done;}
    result=CC_OK;
done:
    (void)psa_hash_abort(&w->hash);(void)psa_hash_abort(&w->base_hash);return result;
}
static uint16_t due(const cr_core *core,uint64_t now) {
    if(!core->view.online||!core->view.trusted||core->view.busy)return 0;
    for(unsigned i=0;i<2;++i)if(core->targets[i]&&now>=core->schedule[i].due_ms)return core->targets[i];
    return 0;
}
static void run(void *arg) {
    (void)arg;cr_core *core=&memory->core;cr_core_reload(core);publish();
    for(;;) {
        cr_input in=inputs();cr_core_input(core,&in);uint16_t year=due(core,in.now_ms);
        if(year&&core->slots[year&1u].blocked)cr_core_reload(core);
        if(year&&core->slots[year&1u].blocked) {
            uint16_t requested;if(cr_core_take(core,in.now_ms,&requested))(void)cr_core_finish(core,requested,CC_UNCERTAIN,CC_ARGUMENT,NULL,in.now_ms);
        } else if(year&&(!audio_busy||!audio_busy())&&weather_http_take(0)) {
            uint16_t requested;
            if(cr_core_take(core,in.now_ms,&requested)) {
                publish();cc_result adjacent,prepared=collect(requested,&in,&adjacent);
                weather_http_give(); /* No UI/state/NVS lock during HTTPS. */
                if(prepared==CC_OK)prepared=cc_prepare(&memory->work.base,adjacent==CC_OK?&memory->work.adjacent:NULL,adjacent,CC_TRUSTED_UPSTREAM,memory->work.digest,memory->work.wire);
                cr_input after=inputs();if(!after.trusted)prepared=CC_CLOCK;
                (void)cr_core_finish(core,requested,prepared,adjacent,prepared==CC_OK?memory->work.wire:NULL,after.now_ms);
            } else weather_http_give();
        }
        uint32_t measured=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
        portENTER_CRITICAL(&mux);stack_min=measured;portEXIT_CRITICAL(&mux);publish();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
bool calendar_runtime_start(cr_audio_busy_fn busy) {
    portENTER_CRITICAL(&mux);
    bool reserved=!started&&!launching;if(reserved)launching=true;
    portEXIT_CRITICAL(&mux);
    if(!reserved)return false;
    memory=heap_caps_calloc(1,sizeof *memory,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!memory){portENTER_CRITICAL(&mux);launching=false;portEXIT_CRITICAL(&mux);return false;}
    cr_core_init(&memory->core,cr_nvs_store());audio_busy=busy;publish();
    if(xTaskCreatePinnedToCoreWithCaps(run,"calendar",CR_WORKER_STACK,NULL,2,&worker,tskNO_AFFINITY,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS) {
        heap_caps_free(memory);memory=NULL;worker=NULL;
        portENTER_CRITICAL(&mux);launching=false;portEXIT_CRITICAL(&mux);return false;
    }
    portENTER_CRITICAL(&mux);started=true;launching=false;portEXIT_CRITICAL(&mux);
    return true;
}
void calendar_runtime_status(void) {
    struct {uint32_t revision,attempts,successes,stack;uint16_t year,last,years[2];uint8_t count,blocked;bool available,busy,online,trusted;cc_result result,adjacent;int error;} s={0};
    portENTER_CRITICAL(&mux);
    s.available=started;s.revision=published.revision;s.attempts=published.attempts;s.successes=published.successes;s.stack=stack_min;s.year=published.current_year;s.last=published.last_year;s.count=published.count;s.blocked=published.blocked_mask;s.busy=published.busy;s.result=published.last_result;s.adjacent=published.adjacent_status;
    s.online=published.online;s.trusted=published.trusted;s.error=published.storage_error;
    for(unsigned i=0;i<2;++i)s.years[i]=published.years[i].year;
    portEXIT_CRITICAL(&mux);
    printf(",\"calendar\":{\"available\":%s,\"revision\":%"PRIu32",\"current_year\":%u,\"years\":[%u,%u],\"count\":%u,\"busy\":%s,\"online\":%s,\"trusted\":%s,\"last_year\":%u,\"result\":%u,\"adjacent_status\":%u,\"storage_error\":%d,\"blocked_mask\":%u,\"attempts\":%"PRIu32",\"successes\":%"PRIu32",\"worker_stack_min_bytes\":%"PRIu32",\"worker_stack_psram\":true,\"workspace_psram_bytes\":%u}",s.available?"true":"false",s.revision,s.year,s.years[0],s.years[1],s.count,s.busy?"true":"false",s.online?"true":"false",s.trusted?"true":"false",s.last,(unsigned)s.result,(unsigned)s.adjacent,s.error,s.blocked,s.attempts,s.successes,s.stack,(unsigned)sizeof(storage));
}
