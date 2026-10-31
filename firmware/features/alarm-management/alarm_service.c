#include "alarm_service.h"
#include "alarm_store_nvs.h"
#include "alarm_bsp.h"
#include "calendar_runtime.h"
#include "app_state.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include <stdatomic.h>
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM || !CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY || !CONFIG_SPIRAM_XIP_FROM_PSRAM || !CONFIG_SPIRAM_FETCH_INSTRUCTIONS || !CONFIG_SPIRAM_RODATA
#error "Alarm owner PSRAM stack requires reviewed external-stack and XiP configuration"
#endif
static ar_runtime owner;
static ar_snapshot published;
static cr_snapshot calendar;
static portMUX_TYPE snapshot_lock=portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE audio_gate=portMUX_INITIALIZER_UNLOCKED;
static aa_player *audio;
static atomic_bool audio_busy;
static atomic_uint stack_free_min;
/* The existing owner also owns this single u8 record. UI never waits for NVS.
 * audio_gate protects short volume requests and preview ownership, not I/O. */
static am_volume_snapshot volume = { .level=70, .saved_level=70 };
static bool volume_stored, preview_requested, preview_owned, save_pending, save_inflight;
static unsigned edit_revision, finished_revision, save_level;
static bool real_active(void){
    at_phase phase=owner.trigger.phase;
    return phase==AT_START_COMMAND||phase==AT_STARTING||phase==AT_RINGING||phase==AT_STOPPING;
}
static int volume_read_record(unsigned *out){
    nvs_handle_t handle;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READONLY,&handle);
    if(err!=ESP_OK)return err;
    uint8_t value=0;err=nvs_get_u8(handle,"alarm_volume",&value);nvs_close(handle);
    if(err==ESP_OK){if(value>100)return ESP_ERR_INVALID_ARG;*out=value;}
    return err;
}
static void volume_load(void){
    unsigned level=70;int error=volume_read_record(&level);
    portENTER_CRITICAL(&audio_gate);
    volume_stored=error==ESP_OK;
    volume.level=volume.saved_level=level;volume.loaded=true;
    volume.load_error=error==ESP_ERR_NVS_NOT_FOUND?0:error;
    portEXIT_CRITICAL(&audio_gate);
}
static int volume_save(unsigned level,unsigned *observed,bool *known){
    nvs_handle_t handle;
    esp_err_t err=nvs_open_from_partition("nvs","home_clock",NVS_READWRITE,&handle);
    if(err==ESP_OK){
        err=nvs_set_u8(handle,"alarm_volume",(uint8_t)level);
        if(err==ESP_OK)err=nvs_commit(handle);
        nvs_close(handle);
    }
    int read_error=volume_read_record(observed);*known=read_error==ESP_OK;
    if(err!=ESP_OK)return err;
    if(read_error!=ESP_OK)return read_error;
    return *observed==level?ESP_OK:ESP_FAIL;
}
static void volume_finish_locked(void){
    preview_requested=false;volume.previewing=false;
    if(preview_owned&&audio){
        aa_state state=aa_status(audio,NULL);
        if(state!=AA_IDLE&&state!=AA_FAILED&&state!=AA_STOPPING)aa_stop(audio);
    }
    if(edit_revision==finished_revision)return;
    finished_revision=edit_revision;
    if(!volume_stored||volume.level!=volume.saved_level||save_pending||save_inflight){
        save_level=volume.level;save_pending=true;volume.saving=true;volume.save_error=0;
    }
}
static void volume_persist(void){
    unsigned level=0;bool take=false;
    portENTER_CRITICAL(&audio_gate);
    if(save_pending&&!real_active()&&!preview_owned&&!preview_requested){
        level=save_level;save_pending=false;
        take=!volume_stored||level!=volume.saved_level;
        save_inflight=take;volume.saving=take;
    }
    portEXIT_CRITICAL(&audio_gate);
    if(!take)return;
    unsigned observed=0;bool known=false;
    int error=volume_save(level,&observed,&known);
    portENTER_CRITICAL(&audio_gate);
    if(known){volume.saved_level=observed;volume_stored=true;volume.load_error=0;}
    else volume_stored=false; /* uncertain persistence must not suppress the next explicit save */
    save_inflight=false;volume.save_error=error;volume.saving=save_pending;
    portEXIT_CRITICAL(&audio_gate);
}
static bool clock_read(void *unused,at_clock *out){
    (void)unused;network_state_t n=app_network_read();uint64_t now=app_uptime();
    if(at_clock_from_sync(n.time_valid,n.sync_count,n.last_sync_utc_us,n.last_sync_ms,now,out))return true;
    *out=(at_clock){.trusted=false,.now_mono_ms=now};return true;
}
static void run(void *arg){
    am_service *s=arg;am_job job;volume_load();
    for(;;){
        (void)calendar_runtime_copy_into(&calendar);
        if(xQueueReceive(s->jobs,&job,pdMS_TO_TICKS(100))==pdTRUE){
            am_reply reply=ar_ui(&owner,&job);
            (void)xQueueOverwrite(s->replies,&reply);
        }
        bool ready=aa_bsp_ready();
        aa_player *current=aa_bsp_player();
        portENTER_CRITICAL(&audio_gate);
        if(current){audio=current;(void)aa_set_level(audio,volume.level);}
        aa_state state=audio?aa_status(audio,NULL):AA_IDLE;
        if(preview_owned&&(state==AA_IDLE||state==AA_FAILED))preview_owned=false;
        bool hidden=preview_owned;
        portEXIT_CRITICAL(&audio_gate);
        if(state==AA_FAILED||state==AA_RECOVERING)ready=false;
        /* Preview is never presented as a scheduled ring. The trigger still
         * scans and checkpoints real due occurrences while preview is active. */
        ar_step(&owner,hidden?NULL:audio,ready,calendar.years,calendar.count);
        portENTER_CRITICAL(&audio_gate);
        bool active=real_active();
        atomic_store_explicit(&audio_busy,active,memory_order_release);
        if(active||!ready)volume_finish_locked();
        state=audio?aa_status(audio,NULL):AA_IDLE;
        if(preview_owned&&(state==AA_IDLE||state==AA_FAILED))preview_owned=false;
        /* Keep a real START command pending until the preview worker is quiet;
         * do not consume it as AA_BUSY or let preview AA_PLAYING satisfy it. */
        if(!preview_owned)ar_dispatch(&owner,audio,ready);
        if(!active&&ready&&preview_requested&&!preview_owned&&audio&&aa_status(audio,NULL)==AA_IDLE){
            preview_owned=aa_start_preview(audio)==AA_OK;
        }
        volume.previewing=preview_requested&&preview_owned;
        hidden=preview_owned;
        portEXIT_CRITICAL(&audio_gate);
        ar_snapshot snapshot;ar_copy_into(&owner,hidden?NULL:audio,ready,calendar.years,calendar.count,&snapshot);
        atomic_store_explicit(&audio_busy,snapshot.active,memory_order_release);
        portENTER_CRITICAL(&snapshot_lock);published=snapshot;portEXIT_CRITICAL(&snapshot_lock);
        volume_persist();
        atomic_store(&stack_free_min,(unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
}
bool am_service_start(am_service *s){
    if(!s||s->worker||s->jobs||s->replies)return false;
    ar_init(&owner,as_nvs_port(),clock_read,NULL);
    s->jobs=xQueueCreate(1,sizeof(am_job));s->replies=xQueueCreate(1,sizeof(am_reply));
    if(!s->jobs||!s->replies||xTaskCreateWithCaps(run,"alarm_owner",8192,s,3,&s->worker,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)!=pdPASS){
        if(s->jobs)vQueueDelete(s->jobs);
        if(s->replies)vQueueDelete(s->replies);
        *s=(am_service){0};return false;
    }
    return true;
}
bool am_service_submit(void *arg,const am_job *job){
    am_service *s=arg;return s&&s->worker&&job&&xQueueSend(s->jobs,job,0)==pdTRUE;
}
bool am_service_take(void *arg,am_reply *reply){
    am_service *s=arg;return s&&s->worker&&reply&&xQueueReceive(s->replies,reply,0)==pdTRUE;
}
bool am_service_copy_into(void *arg,ar_snapshot *out){
    am_service *s=arg;if(!s||!s->worker||!out)return false;
    portENTER_CRITICAL(&snapshot_lock);*out=published;portEXIT_CRITICAL(&snapshot_lock);return true;
}
void am_service_stop(void *arg){
    am_service *s=arg;if(!s||!s->worker)return;
    /* Request-only gate: never held across NVS/I2C or UI work. */
    portENTER_CRITICAL(&audio_gate);
    ar_request_stop(&owner);
    if(audio){aa_stop(audio);if(aa_status(audio,NULL)==AA_FAILED)(void)aa_ack_error(audio);}
    portEXIT_CRITICAL(&audio_gate);
}
bool am_service_audio_busy(void){return atomic_load_explicit(&audio_busy,memory_order_acquire);}
unsigned am_service_stack_free_min(void){return atomic_load(&stack_free_min);}

void am_service_volume_read(am_volume_snapshot *out){
    if(!out)return;
    portENTER_CRITICAL(&audio_gate);
    *out=volume;
    out->available=volume.loaded&&audio&&aa_bsp_ready()&&!atomic_load(&audio_busy);
    portEXIT_CRITICAL(&audio_gate);
}
bool am_service_volume_preview(unsigned level){
    if(level>100)return false;
    portENTER_CRITICAL(&audio_gate);
    bool accept=volume.loaded&&audio&&aa_bsp_ready()&&!atomic_load(&audio_busy);
    if(accept){
        if(!preview_requested||volume.level!=level)++edit_revision;
        volume.level=level;preview_requested=level!=0;
        (void)aa_set_level(audio,level);
        if(!level){
            volume.previewing=false;
            if(preview_owned)aa_stop(audio); /* zero silences PA now; save only on finish */
        }
    }
    portEXIT_CRITICAL(&audio_gate);
    return accept;
}
void am_service_volume_finish(void){
    portENTER_CRITICAL(&audio_gate);volume_finish_locked();portEXIT_CRITICAL(&audio_gate);
}
