#include "location_service.h"
#include "location_catalog.h"
#include "location_store.h"
#include "weather_service.h"
#include "wifi_store.h"
#include "app_state.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>

extern const uint8_t _binary_locations_lco_start[],_binary_locations_lco_end[];
extern const uint8_t _binary_location_metadata_bin_start[],_binary_location_metadata_bin_end[];
static SemaphoreHandle_t mutex;
static QueueHandle_t jobs;
static location_state_t state;
static location_results_t *results;
static location_catalog_t catalog;
typedef struct { int kind;uint32_t generation;char query[LOCATION_QUERY_BYTES];location_record_t record; } job_t;
#if CONFIG_CLOCK_DIAGNOSTICS
static atomic_bool public_test;
static atomic_uint fail_save,search_fault,delay_ms;
static wifi_config_t original_wifi;
static esp_err_t original_wifi_result;
bool location_public_test(void){return public_test;}
#endif
const char *location_phase_name(location_phase_t p) {
    static const char *names[]={"idle","searching","results","empty","offline","clock","transport","timeout","http","oversize","parse","saving","saved","save_failed"};
    return (unsigned)p<sizeof names/sizeof names[0]?names[p]:"unknown";
}
location_state_t location_read(void) {
    location_state_t s={0};if(mutex){xSemaphoreTake(mutex,portMAX_DELAY);s=state;xSemaphoreGive(mutex);}return s;
}
uint32_t location_search(const char *query) {
    if(!mutex||!location_query_valid(query))return 0;
    job_t job={.kind=1};strcpy(job.query,query);
    xSemaphoreTake(mutex,portMAX_DELAY);
    if(state.saving){xSemaphoreGive(mutex);return 0;}
    job.generation=++state.generation;state.phase=LOC_SEARCHING;state.count=0;state.total=0;state.rejected=0;
    xQueueOverwrite(jobs,&job);xSemaphoreGive(mutex);return job.generation;
}
void location_cancel(void) {
    if(!mutex)return;
    xSemaphoreTake(mutex,portMAX_DELAY);
    if(!state.saving){state.generation++;state.count=0;state.total=0;state.phase=LOC_IDLE;memset(results,0,sizeof *results);}
    xSemaphoreGive(mutex);
}
bool location_result(unsigned i,uint32_t generation,location_record_t *out) {
    bool ok=false;xSemaphoreTake(mutex,portMAX_DELAY);
    if(generation==state.generation&&i<state.count&&!state.saving){*out=results->items[i];ok=true;}
    xSemaphoreGive(mutex);return ok;
}
bool location_save(unsigned i,uint32_t generation) {
    job_t job={.kind=2,.generation=generation};bool ok=false;
    xSemaphoreTake(mutex,portMAX_DELAY);
    if(!state.saving&&generation==state.generation&&i<state.count&&(state.phase==LOC_RESULTS||state.phase==LOC_SAVE_FAILED)){
        job.record=results->items[i];ok=xQueueSend(jobs,&job,0)==pdPASS;
        if(ok){state.saving=true;state.phase=LOC_SAVING;}
    }
    xSemaphoreGive(mutex);return ok;
}
static void set_weather(const location_record_t *r) {
    /* Old full records remain independent. The already approved mainland
     * product-time policy does not require a catalog identity migration. */
    const char *zone=r->version==2?r->timezone:!strcmp(r->country_code,"CN")?"Asia/Shanghai":r->timezone;
    weather_set_location(r->latitude,r->longitude,zone);
}
static void service(void *unused) {
    (void)unused;
    job_t *job=heap_caps_calloc(1,sizeof *job,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    location_results_t *parsed=heap_caps_calloc(1,sizeof *parsed,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    configASSERT(job&&parsed);
    int64_t started=esp_timer_get_time();
    bool ready=location_catalog_open(&catalog,_binary_locations_lco_start,_binary_locations_lco_end-_binary_locations_lco_start,
                                    _binary_location_metadata_bin_start,_binary_location_metadata_bin_end-_binary_location_metadata_bin_start);
    size_t workspace_bytes=ready?lix_workspace_size(&catalog.index):0;
    void *workspace=ready?heap_caps_malloc(workspace_bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT):NULL;
    ready=ready&&workspace!=NULL;
    xSemaphoreTake(mutex,portMAX_DELAY);state.catalog_ready=ready;state.workspace_bytes=workspace_bytes;
    state.init_us=esp_timer_get_time()-started;xSemaphoreGive(mutex);
    for(;;){
        if(xQueueReceive(jobs,job,pdMS_TO_TICKS(200))==pdPASS){
            xSemaphoreTake(mutex,portMAX_DELAY);bool current=job->generation==state.generation;xSemaphoreGive(mutex);
            if(!current)continue;
            if(job->kind==1){
                location_phase_t phase=LOC_PARSE;lix_result stats={0};memset(parsed,0,sizeof *parsed);
                unsigned fault=0;
#if CONFIG_CLOCK_DIAGNOSTICS
                fault=atomic_exchange(&search_fault,0);unsigned delay=atomic_exchange(&delay_ms,0);
                if(delay)vTaskDelay(pdMS_TO_TICKS(delay));
#endif
                started=esp_timer_get_time();
                if(fault)phase=fault==3?LOC_TIMEOUT:LOC_PARSE;
                else if(ready&&location_catalog_query(&catalog,job->query,workspace,workspace_bytes,parsed,&stats))phase=parsed->count?LOC_RESULTS:LOC_EMPTY;
                uint32_t elapsed=esp_timer_get_time()-started;
                xSemaphoreTake(mutex,portMAX_DELAY);state.searches++;
                if(job->generation==state.generation){*results=*parsed;state.count=parsed->count;state.rejected=parsed->rejected;state.total=stats.total;state.phase=phase;state.query_us=elapsed;}
                else state.discarded++;
                xSemaphoreGive(mutex);
                /* No query text, coordinates or selected identity in logs. */
            }else if(job->kind==2){
                esp_err_t err;
#if CONFIG_CLOCK_DIAGNOSTICS
                unsigned fault=atomic_load(&fail_save);
                if(fault==2)location_store_fail_readback_once();
                if(fault==1)err=ESP_ERR_NVS_NOT_ENOUGH_SPACE;else
#endif
                err=location_store_save(&job->record);
                xSemaphoreTake(mutex,portMAX_DELAY);state.saving=false;state.storage_error=err;
                if(err==ESP_OK){state.saved=job->record;state.configured=true;state.save_unconfirmed=false;state.revision++;state.saves++;state.phase=LOC_SAVED;set_weather(&job->record);state.first_fetch_pending=true;}
                else{state.save_unconfirmed=true;state.failures++;state.phase=LOC_SAVE_FAILED;}
                xSemaphoreGive(mutex);
                printf("{\"event\":\"location_save\",\"result\":%d,\"independent_readback\":%s}\n",err,err==ESP_OK?"true":"false");
            }
#if CONFIG_CLOCK_DIAGNOSTICS
            else if(job->kind==3){
                esp_err_t err=location_store_clear();xSemaphoreTake(mutex,portMAX_DELAY);
                if(err==ESP_OK){memset(&state.saved,0,sizeof state.saved);state.configured=false;state.boot_loaded=false;state.first_fetch_pending=false;state.revision++;state.generation++;state.count=0;state.phase=LOC_IDLE;weather_clear_location();}
                state.storage_error=err;state.saving=false;xSemaphoreGive(mutex);
            }
#endif
        }
        weather_state_t weather=weather_read();
        xSemaphoreTake(mutex,portMAX_DELAY);state.first_fetch_pending=state.configured&&!weather.attempted_ms;xSemaphoreGive(mutex);
    }
}
void location_start(void) {
    mutex=xSemaphoreCreateMutex();jobs=xQueueCreate(1,sizeof(job_t));
    results=heap_caps_calloc(1,sizeof *results,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);configASSERT(mutex&&jobs&&results);
    esp_err_t err=nvs_flash_init_partition("nvs");state.initialized=err==ESP_OK;
    if(err==ESP_OK)err=location_store_load(&state.saved);
    state.storage_error=err==ESP_ERR_NVS_NOT_FOUND?ESP_OK:err;
    if(err==ESP_OK){state.configured=true;state.boot_loaded=true;state.revision=1;set_weather(&state.saved);state.first_fetch_pending=true;}
#if CONFIG_CLOCK_DIAGNOSTICS
    original_wifi_result=wifi_store_load(&original_wifi);
#endif
    configASSERT(xTaskCreate(service,"location",16384,NULL,3,NULL)==pdPASS);
}
void location_status(void) {
    location_state_t s=location_read();
    printf(",\"location\":{\"configured\":%s,\"boot_loaded\":%s,\"generation\":%"PRIu32",\"revision\":%"PRIu32",\"phase\":\"%s\",\"count\":%u,\"total\":%"PRIu32",\"rejected\":%u,\"searches\":%"PRIu32",\"saves\":%"PRIu32",\"failures\":%"PRIu32",\"discarded\":%"PRIu32",\"storage_error\":%d,\"first_fetch_pending\":%s,\"catalog_ready\":%s,\"workspace_bytes\":%"PRIu32",\"init_us\":%"PRIu32",\"query_us\":%"PRIu32,
           s.configured?"true":"false",s.boot_loaded?"true":"false",s.generation,s.revision,location_phase_name(s.phase),s.count,s.total,s.rejected,s.searches,s.saves,s.failures,s.discarded,s.storage_error,s.first_fetch_pending?"true":"false",s.catalog_ready?"true":"false",s.workspace_bytes,s.init_us,s.query_us);
#if CONFIG_CLOCK_DIAGNOSTICS
    wifi_config_t check_wifi;esp_err_t result=wifi_store_load(&check_wifi);
    bool equal=result==original_wifi_result&&(result==ESP_OK?memcmp(&check_wifi,&original_wifi,sizeof check_wifi)==0:result==ESP_ERR_NVS_NOT_FOUND);
    volatile unsigned char *wipe=(volatile unsigned char*)&check_wifi;for(size_t i=0;i<sizeof check_wifi;i++)wipe[i]=0;
    printf(",\"wifi_record_unchanged\":%s,\"public_test\":%s,\"save_fault\":%u",equal?"true":"false",public_test?"true":"false",atomic_load(&fail_save));
    if(public_test){location_record_t check;int err=location_store_load(&check);
        printf(",\"stored_read_error\":%d,\"stored_present\":%s",err,err==ESP_OK?"true":"false");
        if(err==ESP_OK)printf(",\"stored_public_id\":%"PRIu32,check.id);
        if(s.configured)printf(",\"public_id\":%"PRIu32",\"latitude\":%.5f,\"longitude\":%.5f,\"iana\":\"%s\",\"record_version\":%"PRIu32",\"record_readback_equal\":%s",s.saved.id,s.saved.latitude,s.saved.longitude,s.saved.timezone,s.saved.version,err==ESP_OK&&location_equal(&check,&s.saved)?"true":"false");
    }
#endif
    printf("}");
}
#if CONFIG_CLOCK_DIAGNOSTICS
bool location_command(const char *line) {
    unsigned id;char op[24],action[24],extra;int value;
    if(sscanf(line,"%u %23s %23s %d %c",&id,op,action,&value,&extra)!=4||strcmp(op,"location"))return false;
    if(!strcmp(action,"public")&&(value==0||value==1)){public_test=value;return true;}
    if(!public_test)return false;
    if(!strcmp(action,"fail")&&value>=0&&value<=2){fail_save=value;return true;}
    if(!strcmp(action,"fault")&&value>=0&&value<=4){search_fault=value;return true;}
    if(!strcmp(action,"delay")&&value>=0&&value<=5000){delay_ms=value;return true;}
    if(!strcmp(action,"clear")&&value==1){job_t job={.kind=3};xSemaphoreTake(mutex,portMAX_DELAY);bool ok=!state.saving;
        if(ok){job.generation=++state.generation;state.saving=true;xQueueOverwrite(jobs,&job);}xSemaphoreGive(mutex);return ok;}
    return false;
}
#endif
