/* Application UI and bounded development input; all calls under the BSP LVGL lock. */
#include "app_state.h"
#include "app_ui.h"
#include "wifi_ui.h"
#include "network_time.h"
#include "weather_service.h"
#include "weather_hint.h"
#include "../../assets/application/weather_hint_fonts.h"
#include "location_service.h"
#include "location_ui.h"
#include "display_policy.h"
#include "background_runtime.h"
#include "alarm_ui.h"
#include "../../assets/application/weather_forecast_icons.h"
#include "alarm_service.h"
#include "calendar_runtime.h"
#include "alarm_bsp.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <time.h>
#include <assert.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "soc/uart_pins.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "../../assets/application/navigation_bell.h"
#include "../../assets/application/home_assets.h"

extern const lv_font_t clock_font_28, clock_font_44, clock_font_240, clock_font_weather_24;
extern const lv_image_dsc_t clock_gear_icon, clock_house_icon, clock_softTemp_icon, clock_softDrop_icon;
static lv_obj_t *home, *settings, *preview, *brightness_label;
static lv_obj_t *temperature_label, *humidity_label, *sensor_note, *indoor_title;
/* Three visible labels share one value/baseline; this cache is also the exact
 * text represented by those labels in diagnostic JSON, never a hidden label. */
typedef struct { lv_obj_t *whole,*fraction,*unit; char text[32]; int x,width; } home_reading_t;
static home_reading_t home_temperature,home_humidity,home_weather;
static lv_obj_t *brightness_slider,*preview_note,*brightness_summary,*brightness_detail,*wifi_detail;
static lv_obj_t *first_next,*first_status;
typedef struct {lv_obj_t *back,*title;} child_header_t;
static child_header_t settings_header,preview_header,volume_header;
static lv_obj_t *volume_page,*volume_slider,*volume_label,*volume_note,*volume_summary,*volume_detail;
static lv_obj_t *volume_min,*volume_max,*volume_explain;
static am_volume_snapshot volume_state;
static bool volume_syncing,volume_adjusting,volume_preview_failed;
static lv_obj_t *setting_rows[4],*preview_min,*preview_max,*preview_explain;
static lv_obj_t *first_caption,*first_title,*first_location,*first_later;
static bool slider_syncing,preview_restore_unconfirmed;
static lv_obj_t *clock_label,*date_label;
static lv_obj_t *wifi_setup,*wifi_summary,*location_summary,*location_name;
static lv_obj_t *weather_page,*weather_sources,*weather_title,*weather_value,*weather_note,*weather_icon;
static lv_obj_t *weather_place,*weather_admin,*weather_clock,*weather_update,*weather_notice,*weather_empty,*weather_zone;
static lv_obj_t *weather_current_wind,*weather_wind_symbol,*weather_wind_separator,*weather_hit,*weather_source_body,*weather_source_license,*weather_source_identity;
static lv_obj_t *day_date[7],*day_week[7],*day_icon[7],*day_kind[7],*day_range[7],*day_rain[7];
static lv_obj_t *day_hint_icon[7],*weather_update_icon,*weather_update_state;
/* Keep the existing diagnostic kind ids; selection lives in weather_hint.c. */
typedef enum { DAY_HINT_UNKNOWN,DAY_HINT_WIND,DAY_HINT_UV,DAY_HINT_RAIN,
    DAY_HINT_GUST,DAY_HINT_RAIN_STRONG,DAY_HINT_FREEZING_RAIN,DAY_HINT_SNOW_STRONG,
    DAY_HINT_THUNDER,DAY_HINT_HAIL } day_hint_t;
static day_hint_t day_hint_kind[7];
static weather_forecast_t forecast;
/* Reuse UI-owned snapshots under the LVGL lock; do not add a large automatic
 * settings/location copy beneath render or the UI completion callbacks. */
static wifi_settings_state_t wifi_settings;
static location_state_t saved_location;
static lv_obj_t *indicators[3][3],*pager_groups[3],*alarm_summary,*comfort_icon;
static display_policy_t display_policy;
static am_controller alarm_model;
static am_service alarm_store;
static ar_snapshot alarm_runtime={.next={.status=AM_SUMMARY_UNKNOWN}};
static lv_obj_t *ring_overlay,*ring_title,*ring_note,*ring_stop;
static bool ring_dismissed,stop_touch;
static bool brightness_override;
static bool backlight_failed;
static uint64_t backlight_retry_ms;
extern const lv_image_dsc_t comfort_unknown,comfort_comfortable,comfort_hot,comfort_cold,comfort_dry,comfort_humid;
extern const lv_image_dsc_t comfort_unknown_night,comfort_comfortable_night,comfort_hot_night,comfort_cold_night,comfort_dry_night,comfort_humid_night;


extern const lv_image_dsc_t weather_cloudunknown,weather_sunny,weather_cloudy,weather_overcast,weather_fog,weather_rain,weather_snow,weather_thunder;
static uint64_t browse_activity;
static unsigned lvgl_stack_free_min;
static int settings_return;
static bool overlay_was_visible;
static void render_weather(void);
static void text_if_changed(lv_obj_t *obj,const char *text);
static void volume_refresh(void);
static void volume_finish(void){
    if(!volume_adjusting)return;
    volume_adjusting=false;
    am_service_volume_finish();
}
static void text_if_changed(lv_obj_t *obj,const char *text);
#if CONFIG_CLOCK_DIAGNOSTICS
static int layout_sample;
static lv_obj_t *layout_sample_note;
#endif
static int64_t rendered_utc_us;
static void render_clock(const network_state_t *n);
static uint32_t rendered_sequence;
static lv_indev_t *physical, *virtual_pointer;
static lv_indev_read_cb_t physical_read;
static lv_point_t virtual_point;
static lv_indev_state_t virtual_state;
static uint32_t boot_id, event_seq, request_id, frames;
static unsigned font_missing;

static int brightness=50, resume_brightness=50, saved_brightness=50, page=0;
static bool suppress_touch;
static uint64_t last_touch_ms;
typedef struct { bool down,swiped,eligible; lv_point_t start; int origin; } browse_pointer_t;
static browse_pointer_t physical_browse,virtual_browse;
/* Reuse the three resident page trees: no screenshot or full-screen cache.
 * page stays at the origin until the release animation has settled. */
static struct {
    browse_pointer_t *owner;
    int origin,offset,destination;
    bool settling;
#if CONFIG_CLOCK_DIAGNOSTICS
    uint64_t render_start_us;
    uint32_t render_count,render_max_us;
    uint64_t flush_start_us,flush_wait_start_us;
    uint32_t render_flush_us,render_flush_wait_us,max_flush_us,max_flush_wait_us;
    int render_offset,max_offset;
    bool render_settling,max_settling;
    bool render_pending;
#endif
} browse_motion;
static bool browse_moving(void){return browse_motion.owner||browse_motion.settling;}
static void browse_cancel(void);
static bool pager_gesture_blocked;
#define PAGER_IDLE_MS 5000u
#define PAGER_FADE_MS 240u
enum { BROWSE_DIRECTION_LOCK_PX=12, BROWSE_COMMIT_PX=48, BROWSE_SETTLE_MS=180 };

static uint64_t uptime(void) { return esp_timer_get_time()/1000; }
static bool ring_blocks_navigation(void){
    return alarm_runtime.active||(ring_overlay&&lv_obj_is_visible(ring_overlay));
}
static bool browse_allowed(void){
    return brightness>0&&!ring_blocks_navigation()&&!wifi_ui_visible()&&!location_ui_visible()&&
        (page==0||page==3||(page==5&&!am_ui_editing()&&!am_ui_saving()));
}
static void pager_refresh(uint64_t now){
    bool eligible=browse_allowed()&&!browse_moving();uint64_t idle=now>=browse_activity?now-browse_activity:0;
    unsigned opacity=idle<PAGER_IDLE_MS?255:idle<PAGER_IDLE_MS+PAGER_FADE_MS?
        (unsigned)((PAGER_IDLE_MS+PAGER_FADE_MS-idle)*255/PAGER_FADE_MS):0;
    bool live=eligible&&idle<PAGER_IDLE_MS&&
        !(pager_gesture_blocked&&(physical_browse.down||virtual_browse.down));
    int active=page==0?0:page==3?1:2;
    for(int p=0;p<3;p++)if(pager_groups[p]){
        lv_obj_set_hidden(pager_groups[p],!eligible||p!=active||!opacity);
        lv_obj_set_style_opa(pager_groups[p],(lv_opa_t)opacity,0);
        for(int i=0;i<3;i++)lv_obj_set_clickable(lv_obj_get_parent(indicators[p][i]),live&&p==active);
    }
}
static const char *background_scene_name(void){
    static const char *names[]={"neutral","lowlight","sunny","cloudy","overcast","rain","snow","moon","cloudy-night"};
    return names[display_policy.frame.scene];
}
/* Fixed approved browse-page colors, indexed by the existing weather scene.
 * Hidden pages retain this scene: visibility only pauses background motion. */
static const am_ui_list_palette browse_palettes[]={
    UI_PALETTE_NEUTRAL,
    {0x000000,0xaebfcb,0x8da2b1,0x354955,0x8da2b1,0x8199aa,0x9eb4c4,0x10212d,0x101b24},
    {0x3a647f,0xf0f4f6,0xd1dfe8,0x7392a6,0xd1dfe8,0xc3d5e1,0xd5e1e8,0x193b52,0x244a63},
    {0x385c78,0xf0f4f6,0xc8d8e3,0x69889d,0xc8d8e3,0xb9cedd,0xcfdee7,0x19364b,0x24445d},
    {0x2f3c47,0xe0e8ed,0xafc2d0,0x5b6f7d,0xafc2d0,0x9fb6c6,0xbfd0dc,0x172a38,0x22323f},
    {0x283745,0xe0e8ed,0xafc2d0,0x566b7c,0xafc2d0,0x9fb6c6,0xbfd0dc,0x172a38,0x1d2d3b},
    {0x34434f,0xe0e8ed,0xbdcdd8,0x627888,0xbdcdd8,0xacc2d1,0xc9d7e0,0x1b303f,0x273844},
    {0x101e2d,0xd3dfe7,0x9fb4c4,0x3d5365,0x9fb4c4,0x90a8ba,0xb4c8d7,0x102536,0x1b3042},
    {0x132231,0xd3dfe7,0x9fb4c4,0x405667,0x9fb4c4,0x90a8ba,0xb4c8d7,0x102536,0x1b3042}
};
static const am_ui_list_palette *browse_palette=&browse_palettes[0];
static void theme_settings(const ui_palette_t *p);
static void automatic_resume(void){
    /* A manual ownership boundary, never a per-frame reset. The UI's separate
     * low-light selection survives the core's fresh measurement confirmation. */
    (void)env_runtime_init(&display_policy.environment,uptime(),(unsigned)brightness);
    brightness_override=false;
}
static void alarm_summary_text(am_summary next,const af_time *reference,int64_t utc_s,char *out,size_t size){
    if(next.status!=AM_SUMMARY_NEXT){snprintf(out,size,"--:--");return;}
    uint16_t a=0,b=0;(void)af_day_index(reference,&a);(void)af_day_index(&next.at,&b);
    int days=(int)b-(int)a;
    if(next.at.year>reference->year){unsigned y=reference->year;days+=(y%4==0&&(y%100!=0||y%400==0))?366:365;}
    char prefix[24]="";
    if(days==1)snprintf(prefix,sizeof prefix,"明天 ");
    else if(days>1&&days<7){
        const char *week[]={"日","一","二","三","四","五","六"};
        time_t local=(time_t)(utc_s+8*3600+(int64_t)days*86400);struct tm tm;gmtime_r(&local,&tm);
        snprintf(prefix,sizeof prefix,"周%s ",week[tm.tm_wday]);
    }else if(days>=7)snprintf(prefix,sizeof prefix,"%02u/%02u ",next.at.month,next.at.day);
    snprintf(out,size,"%s%02u:%02u",prefix,next.at.hour,next.at.minute);
}
void app_ui_status(const char *event,uint32_t rid) {
    char sha[65]; const esp_app_desc_t *desc=esp_app_get_description();
    for(int i=0;i<32;i++)snprintf(sha+i*2,3,"%02x",desc->app_elf_sha256[i]);
    sensor_state_t s=app_state_read(); uint64_t now=uptime();
    network_state_t n=app_network_read(); render_clock(&n); render_weather();
    location_ui_saved_weather(weather_place,weather_admin,weather_source_identity,540);
    lv_obj_update_layout(weather_source_body);
    lv_obj_set_y(weather_source_identity,lv_obj_get_height(weather_source_license)+24);
    lv_obj_set_y(weather_notice,lv_obj_get_y(weather_source_identity)+lv_obj_get_height(weather_source_identity)+24);
    lv_obj_set_y(weather_zone,lv_obj_get_y(weather_notice)+lv_obj_get_height(weather_notice)+24);
    weather_state_t wx_status=weather_read();
    bool disclose_weather_metadata=false;
#if CONFIG_CLOCK_DIAGNOSTICS
    disclose_weather_metadata=location_public_test()||wx_status.development_test;
#endif
    flockfile(stdout);
    printf("{\"event\":\"%s\",\"request_id\":%"PRIu32",\"boot_id\":%"PRIu32",\"uptime_ms\":%"PRIu64",\"project\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\",\"elf_sha256\":\"%s\",\"scene\":\"%s\",\"page\":%d,\"brightness\":%d,\"motion\":%s,\"weather_valid\":%s,\"event_seq\":%"PRIu32",\"refreshes\":%"PRIu32",\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"psram_free\":%u,\"font_missing\":%u,\"diagnostics\":%s,",
      event,rid,boot_id,now,desc->project_name,desc->version,desc->idf_ver,sha,background_scene_name(),page,brightness,display_policy.frame.moving?"true":"false",weather_current_valid(&wx_status.data.current,app_utc_us()/1000000,now,n.time_valid)?"true":"false",event_seq,frames,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),(unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),font_missing,CONFIG_CLOCK_DIAGNOSTICS?"true":"false");
#if CONFIG_CLOCK_DIAGNOSTICS
    printf("\"nav_active\":%s,\"nav_settling\":%s,\"nav_offset\":%d,\"nav_render_count\":%"PRIu32",\"nav_render_max_us\":%"PRIu32",\"nav_max_flush_us\":%"PRIu32",\"nav_max_flush_wait_us\":%"PRIu32",\"nav_max_offset\":%d,\"nav_max_settling\":%s,",
        browse_moving()?"true":"false",browse_motion.settling?"true":"false",browse_motion.offset,
        browse_motion.render_count,browse_motion.render_max_us,browse_motion.max_flush_us,
        browse_motion.max_flush_wait_us,browse_motion.max_offset,browse_motion.max_settling?"true":"false");
#endif
    printf("\"sensor_sequence\":%"PRIu32",\"rendered_sequence\":%"PRIu32",\"temperature_text\":\"%s\",\"humidity_text\":\"%s\",\"sht_valid\":%s,\"sht_reason\":\"%s\",\"sht_injection\":%d,\"sht_acquired_ms\":%"PRIu64",\"sht_attempted_ms\":%"PRIu64",\"sht_samples\":%"PRIu32",\"sht_failures\":%"PRIu32",\"sht_injected_failures\":%"PRIu32",",
      s.sequence,rendered_sequence,home_temperature.text,home_humidity.text,measurement_valid(&s.sht,now)?"true":"false",measurement_reason(&s.sht,now),s.sht.injection,s.sht.acquired_ms,s.sht.attempted_ms,s.sht.samples,s.sht.failures,s.sht.injected_failures);
    printf("\"bh_valid\":%s,\"bh_reason\":\"%s\",\"bh_injection\":%d,\"bh_acquired_ms\":%"PRIu64",\"bh_attempted_ms\":%"PRIu64",\"bh_samples\":%"PRIu32",\"bh_failures\":%"PRIu32",\"bh_injected_failures\":%"PRIu32,
      measurement_valid(&s.bh,now)?"true":"false",measurement_reason(&s.bh,now),s.bh.injection,s.bh.acquired_ms,s.bh.attempted_ms,s.bh.samples,s.bh.failures,s.bh.injected_failures);
    if(measurement_valid(&s.sht,now)) printf(",\"temperature_c\":%.2f,\"humidity_pct\":%.2f",s.sht.first,s.sht.second);
    if(measurement_valid(&s.bh,now)) printf(",\"illuminance_lux\":%.2f",s.bh.first);
    printf(",\"wifi_ram_ready\":%s,\"wifi_connected\":%s,\"wifi_associated\":%s,\"radio_running\":%s,\"config_loaded\":%s,\"config_generation\":%"PRIu32",\"connection_count\":%"PRIu32",\"ram_roundtrip\":%s,\"ram_retention_empty\":%s,\"time_valid\":%s,\"utc_us\":%"PRId64",\"rendered_utc_us\":%"PRId64",\"clock_text\":\"%s\",\"date_text\":\"%s\",\"sync_count\":%"PRIu32",\"last_sync_ms\":%"PRIu64",\"last_sync_utc_us\":%"PRId64",\"adjust_us\":%"PRId64",\"server_ip\":\"%s\",\"ntp_mode\":%d,\"sntp_waiting\":%s,\"sntp_timeout\":%s,\"https_done\":%s,\"https_ok\":%s,\"https_status\":%d,\"hold_until_ms\":%"PRIu64",\"disconnected_ms\":%"PRIu64",\"reconnected_ms\":%"PRIu64",\"network_error\":%d",
      n.wifi_ready?"true":"false",n.connected?"true":"false",n.associated?"true":"false",n.radio_running?"true":"false",n.config_loaded?"true":"false",n.config_generation,n.connection_count,
      n.ram_roundtrip?"true":"false",n.ram_retention_empty?"true":"false",n.time_valid?"true":"false",n.time_valid?app_utc_us():0,rendered_utc_us,lv_label_get_text(clock_label),lv_label_get_text(date_label),n.sync_count,n.last_sync_ms,n.last_sync_utc_us,n.last_adjust_us,n.server_ip,n.ntp_mode,n.sntp_waiting?"true":"false",n.sntp_timeout?"true":"false",n.https_done?"true":"false",n.https_ok?"true":"false",n.https_status,n.hold_until_ms,n.disconnected_ms,n.reconnected_ms,n.last_error);
#if CONFIG_CLOCK_DIAGNOSTICS
    printf(",\"layout_sample\":%d",layout_sample);
#endif
    printf(",\"browse_idle_ms\":%"PRIu64",\"home_weather_title\":\"%s\",\"home_weather_value\":\"%s\",\"home_weather_note\":\"%s\",\"weather_update_text\":\"%s\"",now-browse_activity,lv_label_get_text(weather_title),home_weather.text,lv_label_get_text(weather_note),disclose_weather_metadata?lv_label_get_text(weather_update):"");
    printf(",\"home_weather_highlight\":\"%s\",\"home_weather_highlight_visible\":%s",lv_label_get_text(weather_current_wind),lv_obj_is_hidden(weather_current_wind)?"false":"true");
    printf(",\"lvgl_stack_free_min\":%u",lvgl_stack_free_min);
    printf(",\"alarm_runtime_phase\":%d,\"alarm_runtime_error\":%d,\"alarm_audio_state\":%d,\"alarm_audio_error\":%d,\"alarm_audio_ready\":%s,\"alarm_runtime_blocked\":%s,\"alarm_overlay\":%s,\"alarm_owner_stack_free_min\":%u",alarm_runtime.phase,alarm_runtime.error,alarm_runtime.audio,alarm_runtime.audio_error,alarm_runtime.audio_ready?"true":"false",alarm_runtime.blocked?"true":"false",ring_overlay&&!lv_obj_is_hidden(ring_overlay)?"true":"false",am_service_stack_free_min());
    calendar_runtime_status();
    aa_bsp_diagnostic audio_diag;aa_bsp_diagnostics(&audio_diag);
    printf(",\"alarm_audio\":{\"preparation\":%d,\"ready\":%s,\"worker_active\":%s,\"pa_on\":%s,\"mute_readback\":%s,\"volume_readback\":%s,\"codec_volume\":%u,\"dac_register\":%u,\"dma_data_bytes\":%u,\"worker_stack_free_min\":%u,\"cutoff_count\":%u,\"cutoff_failures\":%u,\"normal_blocks\":%u,\"written_samples\":%u,\"preparation_error\":%d}",audio_diag.preparation,audio_diag.ready?"true":"false",audio_diag.worker_active?"true":"false",audio_diag.pa_on?"true":"false",audio_diag.mute_readback?"true":"false",audio_diag.volume_readback?"true":"false",audio_diag.codec_volume,audio_diag.dac_register,audio_diag.dma_data_bytes,audio_diag.worker_stack_free_min,audio_diag.cutoff_count,audio_diag.cutoff_failures,audio_diag.normal_blocks,audio_diag.written_samples,audio_diag.preparation_error);
    am_service_volume_read(&volume_state);
    printf(",\"alarm_volume\":{\"level\":%u,\"saved_level\":%u,\"loaded\":%s,\"saving\":%s,\"previewing\":%s,\"available\":%s,\"load_error\":%d,\"save_error\":%d}",volume_state.level,volume_state.saved_level,volume_state.loaded?"true":"false",volume_state.saving?"true":"false",volume_state.previewing?"true":"false",volume_state.available?"true":"false",volume_state.load_error,volume_state.save_error);
    printf(",\"outdoor_phase\":%d,\"low_light\":%s,\"low_light_known\":%s,\"background_scene\":%d,\"background_frames\":%"PRIu32",\"alarm_known\":%s,\"alarm_count\":%u,\"alarm_editing\":%s,\"alarm_saving\":%s",display_policy.sun.phase,display_policy.low_light?"true":"false",display_policy.env.brightness.low_light_known?"true":"false",display_policy.frame.scene,background_runtime_frames(),alarm_model.known?"true":"false",alarm_model.confirmed.count,am_ui_editing()?"true":"false",am_ui_saving()?"true":"false");
    printf(",\"alarm_runtime_known\":%s,\"alarm_storage_state\":%d,\"alarm_entries\":[",alarm_runtime.known?"true":"false",alarm_runtime.storage);
    for(unsigned i=0;i<alarm_runtime.config.count;++i){const am_entry *e=&alarm_runtime.config.entries[i];printf("%s{\"id\":%"PRIu32",\"hour\":%u,\"minute\":%u,\"mode\":%u,\"enabled\":%s}",i?",":"",e->id,e->hour,e->minute,e->mode,e->enabled?"true":"false");}
    printf("]");
    printf(",\"internal8_free\":%u,\"internal8_min\":%u,\"internal8_largest\":%u,\"dma_free\":%u,\"dma_min\":%u",(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA),(unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
    printf(",\"weather_slots\":[");
    for(int i=0;i<7;i++){
        char high[20]="--",low[20]="--",rain[20]="--";bool range_estimated=false;const char *provider="--";
        const weather_forecast_slot_t *slot=&forecast.slots[i];
        if(slot->state==WEATHER_SLOT_USABLE){
            const weather_day_t *d=&wx_status.data.days[slot->data_index];
            range_estimated=d->range_estimated;provider=weather_provider_name(d->provider);
            snprintf(high,sizeof high,"%.0f",d->high_c);snprintf(low,sizeof low,"%.0f",d->low_c);
            if(d->precipitation_present)snprintf(rain,sizeof rain,"%.0f%%",d->precipitation_pct);
        }
        printf("%s{\"date\":\"%s\",\"week\":\"%s\",\"kind\":\"%s\",\"high\":\"%s\",\"low\":\"%s\",\"rain\":\"%s\",\"range\":\"%s\",\"precipitation_text\":\"%s\",\"highlight\":\"%s\",\"highlight_kind\":%d,\"range_estimated\":%s,\"provider\":\"%s\"}",i?",":"",lv_label_get_text(day_date[i]),lv_label_get_text(day_week[i]),lv_label_get_text(day_kind[i]),high,low,rain,lv_label_get_text(day_range[i]),day_hint_kind[i]==DAY_HINT_RAIN?lv_label_get_text(day_rain[i]):"",lv_label_get_text(day_rain[i]),day_hint_kind[i],range_estimated?"true":"false",provider);
    }
    printf("]");
    wifi_ui_status(); weather_status(); location_status(); location_ui_status(); printf("}\n"); funlockfile(stdout);
}
static bool set_brightness(int value) {
    if(value==0)volume_finish();
    esp_err_t err=bsp_display_brightness_set(value);
    if(err==ESP_OK) {
        brightness=value; if(value>0) resume_brightness=value;
        if(brightness_label) lv_label_set_text_fmt(brightness_label,"%d%%",brightness);
    }
    printf("{\"event\":\"backlight\",\"request_id\":%"PRIu32",\"uptime_ms\":%"PRIu64",\"value\":%d,\"result\":\"%s\"}\n",request_id,uptime(),value,esp_err_to_name(err));
    return err==ESP_OK;
}
static lv_obj_t *box(lv_obj_t *parent,int x,int y,int w,int h,uint32_t color) {
    lv_obj_t *o=lv_obj_create(parent); lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0); lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_scrollable(o,false); return o;
}
static lv_obj_t *label(lv_obj_t *parent,const char *text,int x,int y,const lv_font_t *font,uint32_t color) {
    lv_obj_t *o=lv_label_create(parent); lv_label_set_text(o,text); lv_obj_set_pos(o,x,y);
    lv_obj_set_style_text_font(o,font,0); lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_obj_set_clickable(o,false); return o;
}
static lv_obj_t *image(lv_obj_t *parent,const lv_image_dsc_t *src,int x,int y) {
    lv_obj_t *o=lv_image_create(parent); lv_image_set_src(o,src); lv_obj_set_pos(o,x,y);
    lv_obj_set_clickable(o,false); return o;
}
/* Home's three shared rows use font baselines, not label box bottoms.
 * The reference glyph also supplies its descent: Chinese 内 for headings,
 * digit 0 for readings. This stays fixed when the displayed text changes. */
typedef struct { int baseline; const lv_font_t *font; uint32_t reference; } home_row_t;
static const home_row_t title_row={424,&clock_home_font_32,0x5185};
static const home_row_t value_row={509,&clock_home_font_64,'0'};
static const home_row_t note_row={554,&clock_font_hint_30,0x8212};
enum { HOME_LEFT=48, HOME_DIVIDER=421, HOME_INDOOR=458, HOME_HUMIDITY=776,
       HOME_RIGHT=976, HOME_ICON_SIZE=36, HOME_ICON_GAP=12 };
static lv_obj_t *home_label(const home_row_t *row,const char *text,int x,int width,uint32_t color) {
    lv_obj_t *o=label(home,text,x,row->baseline-(row->font->line_height-row->font->base_line),row->font,color);
    lv_obj_set_width(o,width);
    lv_label_set_long_mode(o,LV_LABEL_LONG_CLIP); /* One reserved row; no reflow. */
    return o;
}
static void home_icon_position(lv_obj_t *obj,const home_row_t *row,const lv_image_dsc_t *src,int x) {
    int left=src->header.w,right=0,top=src->header.h,bottom=0;
    for(int y=0;y<src->header.h;y++)for(int ix=0;ix<src->header.w;ix++){
        if(src->data[y*src->header.stride+ix*4+3]){
            if(ix<left)left=ix;
            if(ix+1>right)right=ix+1;
            if(y<top)top=y;
            if(y+1>bottom)bottom=y+1;
        }
    }
    lv_font_glyph_dsc_t glyph;
    bool found=lv_font_get_glyph_dsc(row->font,&glyph,row->reference,0);
    assert(found&&bottom>0);
    /* 32 -> 36px, fixed local icon scale; center visible ink on the row. */
    lv_image_set_pivot(obj,0,0);lv_image_set_scale(obj,288);
    int center=row->baseline-glyph.ofs_y-glyph.box_h/2;
    lv_obj_set_pos(obj,x+HOME_ICON_SIZE/2-(left+right)*9/16,center-(top+bottom)*9/16);
}
static lv_obj_t *home_icon(const home_row_t *row,const lv_image_dsc_t *src,int x) {
    lv_obj_t *o=image(home,src,x,0);home_icon_position(o,row,src,x);return o;
}
static int home_text_width(const char *text,const lv_font_t *font){
    lv_point_t size;lv_text_get_size(&size,text,font,0,0,1024,LV_TEXT_FLAG_NONE);return size.x;
}
/* The middle-dot ink is centered within its original 30px glyph box.
 * Center that glyph between the range and next group, not its label origin.
 * The rounded wind strokes reach their container's left edge. */
static void home_hint_separator_position(int range_right,bool windy){
    lv_font_glyph_dsc_t dot;
    bool found=lv_font_get_glyph_dsc(note_row.font,&dot,0x00b7,0);
    assert(found);
    int next_left=lv_obj_get_x(windy?weather_wind_symbol:weather_current_wind);
    int x=(range_right+next_left-dot.box_w)/2-dot.ofs_x;
    lv_obj_set_pos(weather_wind_separator,x,lv_obj_get_y(weather_note));
}
static void home_reading_set(home_reading_t *r,const char *text){
    if(!r->whole||!strcmp(r->text,text))return;
    char number[24],whole[24],fraction[12];const char *unit=strstr(text,"°C");
    if(!unit)unit=strchr(text,'%');
    if(!unit)unit=text+strlen(text);
    size_t n=(size_t)(unit-text);if(n>=sizeof number)n=sizeof number-1;
    memcpy(number,text,n);number[n]=0;
    char *dot=strchr(number,'.');fraction[0]=0;
    if(dot){snprintf(fraction,sizeof fraction,"%s",dot);*dot=0;}
    snprintf(whole,sizeof whole,"%s",number);
    text_if_changed(r->whole,whole);text_if_changed(r->fraction,fraction);text_if_changed(r->unit,unit);
    int x=r->x;
    const lv_font_t *fonts[]={&clock_home_font_64,&clock_home_font_46,&clock_home_font_34};
    lv_obj_t *labels[]={r->whole,r->fraction,r->unit};const char *parts[]={whole,fraction,unit};
    for(unsigned i=0;i<3;i++){
        if(i==2&&unit[0])x+=2;
        lv_obj_set_pos(labels[i],x,value_row.baseline-(fonts[i]->line_height-fonts[i]->base_line));
        x+=home_text_width(parts[i],fonts[i]);
    }
    snprintf(r->text,sizeof r->text,"%s",text);
}
static lv_obj_t *home_reading_create(home_reading_t *r,int x,int width,const char *text){
    *r=(home_reading_t){.x=x,.width=width};
    r->whole=label(home,"",x,0,&clock_home_font_64,0x9caebc);
    r->fraction=label(home,"",x,0,&clock_home_font_46,0x9caebc);
    r->unit=label(home,"",x,0,&clock_home_font_34,0x9caebc);
    home_reading_set(r,text);return r->whole;
}
static void home_reading_color(home_reading_t *r,uint32_t color){
    lv_obj_t *labels[]={r->whole,r->fraction,r->unit};
    for(unsigned i=0;i<3;i++)lv_obj_set_style_text_color(labels[i],lv_color_hex(color),0);
}
static int weather_text_width(const char *text){
    lv_point_t size;lv_text_get_size(&size,text,&clock_font_28,0,0,1024,LV_TEXT_FLAG_NONE);return size.x;
}
static lv_obj_t *day_label(int i,int y,uint32_t color){
    int x=48+i*928/7,width=(i+1)*928/7-i*928/7;
    lv_obj_t *o=label(weather_page,"",x,y,&clock_font_28,color);
    lv_obj_set_size(o,width,42);lv_label_set_long_mode(o,LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(o,LV_TEXT_ALIGN_CENTER,0);return o;
}
static lv_obj_t *wind_symbol(lv_obj_t *parent){
    static const lv_point_precise_t strokes[][8]={
        {{1,8},{15,8},{18,6},{18,3},{16,1},{13,1}},
        {{1,13},{22,13},{25,10},{25,7},{23,5},{21,5}},
        {{1,18},{13,18},{16,20},{16,23},{14,25},{11,25}}
    };
    lv_obj_t *o=box(parent,0,0,26,26,0);lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,0);
    for(int i=0;i<3;i++){
        lv_obj_t *line=lv_line_create(o);lv_line_set_points(line,strokes[i],6);
        lv_obj_set_style_line_width(line,2,0);lv_obj_set_style_line_rounded(line,true,0);
        lv_obj_set_style_line_color(line,lv_color_hex(0x9caebc),0);lv_obj_set_clickable(line,false);
    }
    lv_obj_set_clickable(o,false);return o;
}
/* Small monochrome line drawings; no font symbol, bitmap cache or animation. */
static lv_obj_t *hint_graphic(lv_obj_t *parent,bool update){
    static const lv_point_precise_t drop[]={
        {8,1},{7,4},{5,8},{3,11},{2,14},{3,17},{5,20},{8,21},{11,20},{13,17},{14,14},{13,11},{11,8},{9,4},{8,1}};
    static const lv_point_precise_t wind[][9]={
        {{1,7},{12,7},{15,6},{16,4},{15,2},{13,1},{11,2},{10,4}},
        {{1,11},{18,11},{20,12},{21,14},{20,17},{18,18},{16,17},{15,15}},
        {{3,16},{9,16},{11,17},{12,19},{11,21},{9,21}}};
    static const lv_point_precise_t refresh[][12]={
        {{19,6},{17,3},{14,2},{10,2},{6,4},{3,7},{2,8}},
        {{2,3},{2,8},{7,8}},
        {{3,16},{5,19},{8,20},{12,20},{16,18},{19,15},{20,14}},
        {{20,19},{20,14},{15,14}}};
    lv_obj_t *o=box(parent,0,0,22,22,0);lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,0);lv_obj_set_clickable(o,false);
    if(update){
        const unsigned counts[]={7,3,7,3};
        for(unsigned i=0;i<4;i++){
            lv_obj_t *line=lv_line_create(o);lv_line_set_points(line,refresh[i],counts[i]);
            lv_obj_set_style_line_width(line,2,0);lv_obj_set_style_line_rounded(line,true,0);lv_obj_set_clickable(line,false);
        }
    }else{
        lv_obj_t *wind_group=box(o,0,0,22,22,0);lv_obj_set_style_bg_opa(wind_group,LV_OPA_TRANSP,0);lv_obj_set_clickable(wind_group,false);
        for(unsigned i=0;i<3;i++){
            lv_obj_t *line=lv_line_create(wind_group);lv_line_set_points(line,wind[i],i==2?6:8);
            lv_obj_set_style_line_width(line,2,0);lv_obj_set_style_line_rounded(line,true,0);lv_obj_set_clickable(line,false);
        }
        lv_obj_t *line=lv_line_create(o);lv_line_set_points(line,drop,sizeof drop/sizeof drop[0]);lv_obj_set_x(line,3);
        lv_obj_set_style_line_width(line,2,0);lv_obj_set_style_line_rounded(line,true,0);lv_obj_set_clickable(line,false);
    }
    return o;
}
static void browse_ink(lv_obj_t *o,uint32_t color){
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    if(lv_obj_check_type(o,&lv_line_class))lv_obj_set_style_line_color(o,lv_color_hex(color),0);
    if(lv_obj_check_type(o,&lv_image_class)){
        lv_obj_set_style_image_recolor(o,lv_color_hex(color),0);lv_obj_set_style_image_recolor_opa(o,LV_OPA_COVER,0);
    }
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++)browse_ink(lv_obj_get_child(o,(int32_t)i),color);
}
static void apply_browse_palette(void){
    static int applied=-1;
    int scene=display_policy.low_light?WB_BLACK:(int)display_policy.frame.scene;
    if(scene<WB_NEUTRAL||scene>WB_CLOUD_MOON)scene=WB_NEUTRAL;
    if(applied==scene)return;
    applied=scene;const am_ui_list_palette *p=&browse_palettes[scene];browse_palette=p;
    lv_obj_set_style_bg_color(weather_page,lv_color_hex(p->bg),0);
    lv_obj_set_style_bg_color(weather_sources,lv_color_hex(p->bg),0);
    lv_obj_set_style_bg_color(weather_source_body,lv_color_hex(p->bg),0);
    browse_ink(weather_page,p->sub);browse_ink(weather_sources,p->sub);
    lv_obj_set_style_text_color(weather_place,lv_color_hex(p->ink),0);
    for(unsigned i=0;i<7;i++){
        uint32_t ink=forecast.slots[i].state==WEATHER_SLOT_USABLE?p->ink:p->sub;
        lv_obj_set_style_text_color(day_kind[i],lv_color_hex(ink),0);
        lv_obj_set_style_text_color(day_range[i],lv_color_hex(ink),0);
        if(!i)lv_obj_set_style_text_color(day_week[i],lv_color_hex(p->ink),0);
    }
    for(int page_index=1;page_index<3;page_index++)for(int i=0;i<3;i++)
        lv_obj_set_style_bg_color(indicators[page_index][i],lv_color_hex(i==page_index?p->ink:p->sub),0);
    am_ui_set_list_palette(p);
    theme_settings(p);wifi_ui_set_palette(p);location_ui_set_palette(p);
}
static void show_page(int next) {
    if(page==5&&next!=5&&(am_ui_editing()||am_ui_saving()))return;
    if(page==7&&next!=7)volume_finish();
    browse_cancel();
    page=next;
    am_ui_visible(page==5);
    browse_activity=uptime();
    lv_obj_set_hidden(home,page!=0);
    lv_obj_set_hidden(weather_page,page!=3);
    lv_obj_set_hidden(weather_sources,page!=4);
    lv_obj_set_hidden(settings,true); lv_obj_set_hidden(preview,true);
    if(page==1) lv_obj_set_hidden(settings,false);
    if(page==2) lv_obj_set_hidden(preview,false);
    if(volume_page)lv_obj_set_hidden(volume_page,page!=7);
    if(first_next)lv_obj_set_hidden(first_next,page!=6);
    pager_refresh(browse_activity);
}
static lv_obj_t *browse_root(int p){return p==0?home:p==3?weather_page:am_ui_root();}
static int browse_neighbor(int p,int direction){
    const int pages[]={0,3,5};int i=p==0?0:p==3?1:2;
    return pages[(i+direction+3)%3];
}
static void browse_place(void *unused,int32_t offset){
    (void)unused;browse_motion.offset=(int)offset;
#if CONFIG_CLOCK_DIAGNOSTICS
    browse_motion.render_pending=true;
#endif
    lv_obj_set_x(browse_root(browse_motion.origin),offset);
    lv_obj_set_x(browse_root(browse_neighbor(browse_motion.origin,-1)),offset-1024);
    lv_obj_set_x(browse_root(browse_neighbor(browse_motion.origin,1)),offset+1024);
}
static void browse_cancel(void){
    if(!browse_moving())return;
    lv_anim_delete(&browse_motion,browse_place);
    browse_motion.owner=NULL;browse_motion.settling=false;browse_motion.offset=0;
#if CONFIG_CLOCK_DIAGNOSTICS
    browse_motion.render_pending=true;
#endif
    physical_browse.eligible=virtual_browse.eligible=false;
    lv_obj_t *roots[]={home,weather_page,am_ui_root()};const int pages[]={0,3,5};
    for(int i=0;i<3;i++)if(roots[i]){
        lv_obj_set_x(roots[i],0);lv_obj_set_hidden(roots[i],page!=pages[i]);
    }
}
static void browse_settled(lv_anim_t *animation){
    (void)animation;int destination=browse_motion.destination;
    /* show_page is also the single cleanup path for direct/idle navigation. */
    show_page(destination);
}
static void browse_release(void){
    int offset=browse_motion.offset;
    int direction=offset<=-BROWSE_COMMIT_PX?1:offset>=BROWSE_COMMIT_PX?-1:0;
    browse_motion.destination=direction?browse_neighbor(browse_motion.origin,direction):browse_motion.origin;
    browse_motion.owner=NULL;browse_motion.settling=true;
    lv_anim_t animation;lv_anim_init(&animation);
    lv_anim_set_var(&animation,&browse_motion);lv_anim_set_exec_cb(&animation,browse_place);
    lv_anim_set_values(&animation,offset,direction?-direction*1024:0);
    lv_anim_set_duration(&animation,BROWSE_SETTLE_MS);lv_anim_set_path_cb(&animation,lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&animation,browse_settled);lv_anim_start(&animation);
}
static void alarm_activity(void *unused){(void)unused;browse_activity=uptime();}
static void alarm_home(void *unused){(void)unused;show_page(0);}
static void alarm_settings(void *unused){
    (void)unused;if(ring_blocks_navigation())return;
    event_seq++;browse_activity=uptime();settings_return=page;show_page(1);
}
static bool alarm_submit(void *unused,const am_job *job){(void)unused;return am_service_submit(&alarm_store,job);}
static bool alarm_take(void *unused,am_reply *reply){(void)unused;return am_service_take(&alarm_store,reply);}
static bool alarm_next(void *unused,am_summary *next,bool *notice,bool *ready){
    (void)unused;*next=alarm_runtime.next;*notice=alarm_runtime.calendar_notice;*ready=alarm_runtime.audio_ready;return true;
}
static void ring_stop_requested(void){am_service_stop(&alarm_store);ring_dismissed=alarm_runtime.phase==AT_AUDIO_FAILED;browse_activity=uptime();}
static void ring_clicked(lv_event_t *event){(void)event;ring_stop_requested();}
static void ring_refresh(void){
    if(!ring_overlay)return;
    static bool was_active;
    if(alarm_runtime.active&&!was_active)ring_dismissed=false;
    was_active=alarm_runtime.active;
    bool show=alarm_runtime.active||(alarm_runtime.phase==AT_AUDIO_FAILED&&!ring_dismissed);
    bool opening=show&&lv_obj_is_hidden(ring_overlay);
    if(opening){
        volume_finish();
        browse_cancel();
        if(physical&&lv_indev_get_state(physical)==LV_INDEV_STATE_PRESSED)lv_indev_wait_release(physical);
        if(virtual_pointer&&lv_indev_get_state(virtual_pointer)==LV_INDEV_STATE_PRESSED)lv_indev_wait_release(virtual_pointer);
    }
    lv_obj_set_hidden(ring_overlay,!show);
    if(!show)return;
    lv_obj_move_foreground(ring_overlay);
    lv_label_set_text(ring_title,alarm_runtime.phase==AT_STOPPING?"正在停止":alarm_runtime.phase==AT_AUDIO_FAILED?"声音暂不可用":"闹铃");
    lv_label_set_text(ring_note,alarm_runtime.phase==AT_STOPPING?"请稍候":alarm_runtime.phase==AT_AUDIO_FAILED?"本次不会重新响铃":"");
    lv_obj_set_style_text_color(ring_title,lv_color_hex(display_policy.low_light?0x9caebc:0xf0f3f3),0);
}
static void wifi_done(bool first,bool saved){
    if(first&&saved){
        saved_location=location_read();
        /* A failed/unready location read is not evidence of a missing place. */
        bool missing=saved_location.initialized&&!saved_location.configured&&
            !saved_location.storage_error&&!saved_location.save_unconfirmed;
        show_page(missing?6:0);
    }else show_page(first?0:1);
}
static void location_done(bool first,bool saved){(void)saved;show_page(first?0:1);}
static void open_wifi_task(void){
    wifi_settings=network_settings_read();
    bool first=page==0&&wifi_settings.initialized&&!wifi_settings.saved_present&&
        !wifi_settings.storage_error&&!wifi_settings.save_unconfirmed;
    if(!first&&page!=1){settings_return=page;show_page(1);}
    browse_cancel();wifi_ui_open_context(first);
}
static void open_location_task(bool first){
    if(!first&&page!=1){settings_return=page;show_page(1);}
    browse_cancel();location_ui_open_context(first);
}
static void brightness_changed(lv_event_t *event){
    (void)event;if(slider_syncing)return;
    if(page!=2||brightness==0||ring_blocks_navigation()){
        lv_slider_set_value(brightness_slider,brightness>0?brightness:1,LV_ANIM_OFF);return;
    }
    int target=lv_slider_get_value(brightness_slider);browse_activity=uptime();
    if(set_brightness(target))lv_label_set_text(preview_note,"");
    else{
        slider_syncing=true;lv_slider_set_value(brightness_slider,brightness,LV_ANIM_OFF);slider_syncing=false;
        lv_label_set_text_fmt(preview_note,"未能调整，仍为%d%%",brightness);
    }
}
static void volume_event(lv_event_t *event){
    lv_event_code_t code=lv_event_get_code(event);
    if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST){
        volume_finish();volume_refresh();return;
    }
    if(volume_syncing)return;
    if(page!=7||brightness==0||ring_blocks_navigation()){
        volume_finish();return;
    }
    am_service_volume_read(&volume_state);
    if(!volume_state.loaded||!volume_state.available){
        volume_finish();return;
    }
    if(code==LV_EVENT_PRESSED){
        /* Entering or merely touching the thumb never starts playback. */
        volume_adjusting=true;volume_preview_failed=false;browse_activity=uptime();
    }else if(code==LV_EVENT_VALUE_CHANGED&&volume_adjusting){
        browse_activity=uptime();
        if(!am_service_volume_preview((unsigned)lv_slider_get_value(volume_slider))){
            volume_preview_failed=true;volume_finish();
        }
        volume_refresh();
    }
}
static void clicked(lv_event_t *e) {
    if(ring_blocks_navigation())return;
    int action=(int)(intptr_t)lv_event_get_user_data(e);
    event_seq++;
    browse_activity=uptime();
    if(action==1) { settings_return=page; show_page(1); }
    else if(action==2) {
        saved_brightness=brightness;brightness_override=true;
        slider_syncing=true;lv_slider_set_value(brightness_slider,brightness>0?brightness:1,LV_ANIM_OFF);slider_syncing=false;
        lv_label_set_text(preview_note,"");show_page(2);
    }
    else if(action==3) { preview_restore_unconfirmed=!set_brightness(saved_brightness);automatic_resume();show_page(1); }
    else if(action==4) show_page(settings_return);
    else if(action==5 || action==6) { int value=brightness+(action==5?-5:5); if(value<1)value=1; if(value>100)value=100; set_brightness(value); }
    else if(action==7) open_wifi_task();
    else if(action==8) open_location_task(page!=1);
    else if(action==9) show_page(3);
    else if(action==10) show_page(0);
    else if(action==11) {lv_obj_scroll_to_y(weather_source_body,0,LV_ANIM_OFF);show_page(4);}
    else if(action==12) show_page(5);
    else if(action==13) open_location_task(true);
    else if(action==14) show_page(0);
    else if(action==15) {volume_preview_failed=false;show_page(7);volume_refresh();}
    else if(action==16) show_page(1);
    lv_label_set_text_fmt(brightness_label,"%d%%",brightness);
    printf("{\"event\":\"ui\",\"request_id\":%"PRIu32",\"boot_id\":%"PRIu32",\"uptime_ms\":%"PRIu64",\"source\":\"%s\",\"action\":%d,\"page\":%d,\"event_seq\":%"PRIu32"}\n",request_id,boot_id,uptime(),lv_event_get_indev(e)==physical?"physical":"virtual",action,page,event_seq);
}
static lv_obj_t *button(lv_obj_t *parent,const char *text,int x,int y,int w,int action) {
    lv_obj_t *o=box(parent,x,y,w,72,0x172a3a); lv_obj_set_clickable(o,true);
    lv_obj_set_style_border_width(o,1,0); lv_obj_set_style_border_color(o,lv_color_hex(0x607787),0);
    lv_obj_set_style_radius(o,12,0); lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,(void *)(intptr_t)action);
    if(text) { lv_obj_t *l=label(o,text,0,0,&clock_font_28,0xc4d1d8); lv_obj_center(l); } return o;
}
static lv_obj_t *setting_row(const char *title,int y,int action,lv_obj_t **summary,lv_obj_t **detail,bool rule){
    lv_obj_t *row=button(settings,NULL,48,y,928,action);lv_obj_set_height(row,104);
    lv_obj_set_style_bg_opa(row,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(row,0,0);lv_obj_set_style_radius(row,0,0);
    label(row,title,8,30,&clock_font_28,0xc4d1d8);
    *summary=label(row,"",336,12,&clock_font_28,0x9caebc);lv_obj_set_size(*summary,536,42);
    *detail=label(row,"",304,54,&clock_font_28,0x778994);lv_obj_set_size(*detail,568,42);
    lv_label_set_long_mode(*summary,LV_LABEL_LONG_DOT);lv_label_set_long_mode(*detail,LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(*summary,LV_TEXT_ALIGN_RIGHT,0);lv_obj_set_style_text_align(*detail,LV_TEXT_ALIGN_RIGHT,0);
    label(row,LV_SYMBOL_RIGHT,892,38,LV_FONT_DEFAULT,0x778994);
    if(rule)lv_obj_set_clickable(box(row,0,103,928,1,0x344653),false);
    return row;
}
static void setting_detail(lv_obj_t *summary,lv_obj_t *detail,const char *text){
    text_if_changed(detail,text);lv_obj_set_y(summary,text[0]?12:30);lv_obj_set_hidden(detail,!text[0]);
}
static void volume_refresh(void){
    if(!volume_page)return;
    am_service_volume_read(&volume_state);
    if(volume_adjusting&&(page!=7||brightness==0||ring_blocks_navigation()||!volume_state.available)){
        volume_finish();am_service_volume_read(&volume_state);
    }
    if(!volume_adjusting){
        volume_syncing=true;lv_slider_set_value(volume_slider,(int)volume_state.level,LV_ANIM_OFF);volume_syncing=false;
    }
    unsigned level=(unsigned)lv_slider_get_value(volume_slider);char text[24];
    if(!volume_state.loaded)snprintf(text,sizeof text,"读取中");
    else if(level==0)snprintf(text,sizeof text,"静音");
    else snprintf(text,sizeof text,"%u%%",level);
    text_if_changed(volume_label,text);text_if_changed(volume_summary,text);
    lv_obj_set_style_text_font(volume_label,volume_state.loaded&&level?&clock_font_44:&clock_font_28,0);
    const char *note=volume_state.save_error?"未能保存，可重试":volume_preview_failed?"声音暂不可用":
        !volume_state.loaded?"读取中":volume_state.saving?"保存中":volume_state.load_error?"设置读取失败":
        !volume_state.available?"声音暂不可用":"";
    text_if_changed(volume_note,note);setting_detail(volume_summary,volume_detail,note);
    if(volume_state.loaded&&volume_state.available)lv_obj_remove_state(volume_slider,LV_STATE_DISABLED);
    else lv_obj_add_state(volume_slider,LV_STATE_DISABLED);
}
static lv_obj_t *gear_button(lv_obj_t *parent){
    lv_obj_t *o=button(parent,NULL,920,32,64,1);
    lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(o,0,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(0xc4d1d8),LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o,20,LV_STATE_PRESSED);
    lv_obj_t *icon=image(o,&clock_gear_icon,16,20);lv_image_set_scale(icon,224);
    return o;
}
static child_header_t child_header(lv_obj_t *parent,const char *back,const char *title,int action){
    lv_obj_t *o=button(parent,NULL,40,32,112,action);
    lv_obj_set_style_bg_opa(o,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(o,0,0);
    label(o,LV_SYMBOL_LEFT,8,28,LV_FONT_DEFAULT,0xc4d1d8);
    label(o,back,32,15,&clock_font_28,0xc4d1d8);
    return (child_header_t){o,label(parent,title,176,47,&clock_font_28,0xc4d1d8)};
}
static void theme_setting_button(lv_obj_t *o,const ui_palette_t *p){
    lv_obj_set_style_bg_color(o,lv_color_hex(p->button),0);
    lv_obj_set_style_border_color(o,lv_color_hex(p->edge),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(p->deep),LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,LV_STATE_PRESSED);
    for(uint32_t i=0;i<lv_obj_get_child_count(o);i++){
        lv_obj_t *child=lv_obj_get_child(o,(int32_t)i);
        if(lv_obj_check_type(child,&lv_label_class))lv_obj_set_style_text_color(child,lv_color_hex(p->ink),0);
    }
}
static void theme_settings(const ui_palette_t *p){
    if(!settings)return;
    lv_obj_t *panels[]={settings,preview,first_next,volume_page};
    for(unsigned i=0;i<sizeof panels/sizeof panels[0];i++)lv_obj_set_style_bg_color(panels[i],lv_color_hex(p->bg),0);
    child_header_t headers[]={settings_header,preview_header,volume_header};
    for(unsigned i=0;i<sizeof headers/sizeof headers[0];i++){
        theme_setting_button(headers[i].back,p);
        lv_obj_set_style_text_color(headers[i].title,lv_color_hex(p->ink),0);
    }
    for(unsigned i=0;i<4;i++){
        lv_obj_t *row=setting_rows[i];
        lv_obj_set_style_bg_color(row,lv_color_hex(p->button),LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row,LV_OPA_COVER,LV_STATE_PRESSED);
        for(unsigned j=0;j<4;j++)lv_obj_set_style_text_color(lv_obj_get_child(row,j),lv_color_hex(j?p->sub:p->ink),0);
        if(lv_obj_get_child_count(row)>4)lv_obj_set_style_bg_color(lv_obj_get_child(row,4),lv_color_hex(p->line),0);
    }
    lv_obj_t *primary[]={brightness_label,preview_note,first_title,volume_label,volume_note};
    for(unsigned i=0;i<sizeof primary/sizeof primary[0];i++)lv_obj_set_style_text_color(primary[i],lv_color_hex(p->ink),0);
    lv_obj_t *secondary[]={preview_min,preview_max,preview_explain,first_caption,first_status,volume_min,volume_max,volume_explain};
    for(unsigned i=0;i<sizeof secondary/sizeof secondary[0];i++)lv_obj_set_style_text_color(secondary[i],lv_color_hex(p->sub),0);
    theme_setting_button(first_location,p);theme_setting_button(first_later,p);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(p->line),0);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(p->on),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(p->on),LV_PART_KNOB);
    lv_obj_set_style_border_color(brightness_slider,lv_color_hex(p->bg),LV_PART_KNOB);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(p->line),0);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(p->on),LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(p->on),LV_PART_KNOB);
    lv_obj_set_style_border_color(volume_slider,lv_color_hex(p->bg),LV_PART_KNOB);
}
static void pager_clicked(lv_event_t *e){
    /* The release which revealed a pager never activates it, even if LVGL
     * rechecks a target after its clickable flag is restored on release. */
    if(!browse_allowed()||pager_gesture_blocked)return;
    clicked(e);
}
static void browse_input(lv_indev_t *indev,lv_indev_data_t *data,browse_pointer_t *p) {
    bool pressed=data->state==LV_INDEV_STATE_PRESSED;
    uint64_t now=uptime();
    if(browse_moving()&&!browse_allowed())browse_cancel();
    if(ring_blocks_navigation()){
        p->eligible=false;p->down=pressed;browse_activity=now;pager_refresh(now);return;
    }
    bool browse=browse_allowed();
    if(!browse)p->eligible=false; /* Overlay/editor preemption cancels an in-flight swipe. */
    if(pressed&&!p->down)pager_gesture_blocked=browse&&now>=browse_activity&&now-browse_activity>=PAGER_IDLE_MS;
    if(pressed || p->down) browse_activity=now;
    if(pressed&&!p->down) {
        p->start=data->point;p->origin=page;p->swiped=false;
        p->eligible=browse&&!browse_moving()&&data->point.y>=120&&data->point.y<550;
        /* A new touch during the short settle cannot activate moving buttons. */
        if(browse_moving()){p->swiped=true;lv_indev_wait_release(indev);}
    }
    if(pressed&&p->down&&p->eligible&&browse&&page==p->origin) {
        int dx=data->point.x-p->start.x,dy=data->point.y-p->start.y;
        if(!p->swiped){
            /* Direction lock precedes the larger release/commit distance.
             * Vertical lists keep their native scroll gesture. */
            if(abs(dx)>=BROWSE_DIRECTION_LOCK_PX&&abs(dx)>abs(dy)*2){
                p->swiped=true;lv_indev_wait_release(indev);
                browse_motion.owner=p;browse_motion.origin=page;
#if CONFIG_CLOCK_DIAGNOSTICS
                browse_motion.render_start_us=0;browse_motion.render_count=0;browse_motion.render_max_us=0;
                browse_motion.max_flush_us=0;browse_motion.max_flush_wait_us=0;
                browse_motion.max_offset=0;browse_motion.max_settling=false;
#endif
                background_runtime_submit(&display_policy.frame,false);
                lv_obj_set_hidden(home,false);lv_obj_set_hidden(weather_page,false);am_ui_visible(true);
            }else if(abs(dy)>=BROWSE_DIRECTION_LOCK_PX)p->eligible=false;
        }
        if(browse_motion.owner==p)browse_place(NULL,dx<-1024?-1024:dx>1024?1024:dx);
    }
    if(!pressed&&p->down&&browse_motion.owner==p)browse_release();
    p->down=pressed;
    pager_refresh(now);
}
static void raw_read(lv_indev_t *indev,lv_indev_data_t *data) {
    physical_read(indev,data);
    static bool down=false;
    bool pressed=data->state==LV_INDEV_STATE_PRESSED;
    if(!wifi_ui_private() && !location_ui_private() && (pressed!=down || (pressed && uptime()-last_touch_ms>=250))) {
      printf("{\"event\":\"touch\",\"boot_id\":%"PRIu32",\"uptime_ms\":%"PRIu64",\"source\":\"physical\",\"state\":\"%s\",\"x\":%d,\"y\":%d}\n",boot_id,uptime(),pressed?"pressed":"released",(int)data->point.x,(int)data->point.y);
      last_touch_ms=uptime();
    }
    down=pressed;
    if(pressed&&!stop_touch&&brightness==0&&alarm_runtime.active){ring_stop_requested();stop_touch=true;}
    if(stop_touch){data->state=LV_INDEV_STATE_RELEASED;if(!pressed)stop_touch=false;}
    if(pressed && brightness==0 && !suppress_touch) {
      suppress_touch=true; set_brightness(resume_brightness);browse_activity=uptime();
      printf("{\"event\":\"wake_only\",\"boot_id\":%"PRIu32",\"uptime_ms\":%"PRIu64"}\n",boot_id,uptime());
    }
    if(suppress_touch) { data->state=LV_INDEV_STATE_RELEASED; if(!pressed)suppress_touch=false; }
    browse_input(indev,data,&physical_browse);
}
static void virtual_read(lv_indev_t *indev,lv_indev_data_t *data) { data->point=virtual_point; data->state=virtual_state; browse_input(indev,data,&virtual_browse); }
static void refresh_cb(lv_event_t *e) {
    if(lv_event_get_code(e)==LV_EVENT_REFR_READY)frames++;
#if CONFIG_CLOCK_DIAGNOSTICS
    if(lv_event_get_code(e)==LV_EVENT_RENDER_START){
        browse_motion.render_start_us=browse_moving()||browse_motion.render_pending?(uint64_t)esp_timer_get_time():0;
        browse_motion.flush_start_us=0;browse_motion.flush_wait_start_us=0;
        browse_motion.render_flush_us=0;browse_motion.render_flush_wait_us=0;
        browse_motion.render_offset=browse_motion.offset;browse_motion.render_settling=browse_motion.settling;
        return;
    }
    if(browse_motion.render_start_us){
        /* Per-render spans only. A driver may wait inside its flush callback;
         * these diagnostics are not independent terms to add into panel time. */
        uint64_t now=(uint64_t)esp_timer_get_time();
        switch(lv_event_get_code(e)){
            case LV_EVENT_FLUSH_START:browse_motion.flush_start_us=now;break;
            case LV_EVENT_FLUSH_FINISH:
                if(browse_motion.flush_start_us)browse_motion.render_flush_us+=(uint32_t)(now-browse_motion.flush_start_us);
                browse_motion.flush_start_us=0;break;
            case LV_EVENT_FLUSH_WAIT_START:browse_motion.flush_wait_start_us=now;break;
            case LV_EVENT_FLUSH_WAIT_FINISH:
                if(browse_motion.flush_wait_start_us)browse_motion.render_flush_wait_us+=(uint32_t)(now-browse_motion.flush_wait_start_us);
                browse_motion.flush_wait_start_us=0;break;
            default:break;
        }
    }
    if(lv_event_get_code(e)==LV_EVENT_RENDER_READY&&browse_motion.render_start_us){
        uint64_t elapsed=(uint64_t)esp_timer_get_time()-browse_motion.render_start_us;
        browse_motion.render_count++;
        if(elapsed>browse_motion.render_max_us){
            browse_motion.render_max_us=(uint32_t)elapsed;
            browse_motion.max_flush_us=browse_motion.render_flush_us;browse_motion.max_flush_wait_us=browse_motion.render_flush_wait_us;
            browse_motion.max_offset=browse_motion.render_offset;browse_motion.max_settling=browse_motion.render_settling;
        }
        browse_motion.render_start_us=0;browse_motion.render_pending=false;
    }
#endif
}
static void text_if_changed(lv_obj_t *obj,const char *text) {
    if(strcmp(lv_label_get_text(obj),text)!=0) lv_label_set_text(obj,text);
}
static void render_clock(const network_state_t *n) {
    if(!clock_label || !date_label) return;
    if(!n->time_valid) {
        text_if_changed(clock_label,"--:--"); if(weather_clock)text_if_changed(weather_clock,"--:--"); text_if_changed(date_label,"时间尚未校准"); rendered_utc_us=0; return;
    }
    rendered_utc_us=app_utc_us(); time_t local=(time_t)(rendered_utc_us/1000000+8*3600);
    struct tm tm; gmtime_r(&local,&tm); char text[80];
    snprintf(text,sizeof text,"%02d:%02d",tm.tm_hour,tm.tm_min); text_if_changed(clock_label,text);
    if(weather_clock)text_if_changed(weather_clock,text);
    const char *week[]={"日","一","二","三","四","五","六"};
    snprintf(text,sizeof text,"%04d-%02d-%02d 星期%s",tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,week[tm.tm_wday]);
    text_if_changed(date_label,text);
}
static const char *kind_text(int code) {
    static const char *names[]={"未知","晴","多云","阴","雾","雨","雪","雷雨"};
    return names[weather_kind(code)];
}
static void weather_hint_text(weather_hint_t hint,bool current,char *out,size_t size) {
    switch(hint.kind) {
    case WEATHER_HINT_WIND: snprintf(out,size,current?"风%d级":"%d级",hint.grade);break;
    case WEATHER_HINT_GUST: snprintf(out,size,"阵风%d级",hint.grade);break;
    case WEATHER_HINT_RAIN_STRONG: snprintf(out,size,"雨较强");break;
    case WEATHER_HINT_FREEZING_RAIN: snprintf(out,size,"冻雨");break;
    case WEATHER_HINT_SNOW_STRONG: snprintf(out,size,"雪较强");break;
    case WEATHER_HINT_AIR_POOR: snprintf(out,size,"空气欠佳");break;
    case WEATHER_HINT_THUNDER: snprintf(out,size,"雷电");break;
    case WEATHER_HINT_HAIL: snprintf(out,size,"冰雹");break;
    case WEATHER_HINT_UV: snprintf(out,size,"UV 强");break;
    case WEATHER_HINT_PRECIPITATION:
        if(hint.probability_present)snprintf(out,size,"%.0f%%",hint.probability_pct);
        else snprintf(out,size,"—");
        break;
    default: snprintf(out,size,"%s",current?"":"—");break;
    }
}
static day_hint_t day_hint_style(weather_hint_kind_t kind) {
    switch(kind) {
    case WEATHER_HINT_WIND: return DAY_HINT_WIND;
    case WEATHER_HINT_GUST: return DAY_HINT_GUST;
    case WEATHER_HINT_UV: return DAY_HINT_UV;
    case WEATHER_HINT_PRECIPITATION: return DAY_HINT_RAIN;
    case WEATHER_HINT_RAIN_STRONG: return DAY_HINT_RAIN_STRONG;
    case WEATHER_HINT_FREEZING_RAIN: return DAY_HINT_FREEZING_RAIN;
    case WEATHER_HINT_SNOW_STRONG: return DAY_HINT_SNOW_STRONG;
    case WEATHER_HINT_THUNDER: return DAY_HINT_THUNDER;
    case WEATHER_HINT_HAIL: return DAY_HINT_HAIL;
    default: return DAY_HINT_UNKNOWN;
    }
}
static const lv_image_dsc_t *kind_icon(int code) {
    /* Outdoor solar phase is independent of the room low-light state. */
    if(weather_kind(code)==1&&display_policy.sun.phase==SOLAR_NIGHT)return &weather_home_moon;
    static const lv_image_dsc_t *icons[]={&weather_home_cloudunknown,&weather_home_sunny,&weather_home_cloudy,&weather_home_overcast,&weather_home_fog,&weather_home_rain,&weather_home_snow,&weather_home_thunder};
    return icons[weather_kind(code)];
}
static const lv_image_dsc_t *forecast_kind_icon(int code) {
    static const lv_image_dsc_t *icons[]={&weather_forecast_cloudunknown,&weather_forecast_sunny,&weather_forecast_cloudy,&weather_forecast_overcast,&weather_forecast_fog,&weather_forecast_rain,&weather_forecast_snow,&weather_forecast_thunder};
    return icons[weather_kind(code)];
}
static void weather_time(int64_t utc,int offset,const char *format,char *out,size_t size){
    time_t seconds=utc+offset;struct tm tm;
    if(!gmtime_r(&seconds,&tm)||!strftime(out,size,format,&tm))snprintf(out,size,"--");
}
static const char *forecast_slot_text(weather_slot_state_t state){
    static const char *names[]={"日期待确认","暂无","已过期","可用"};return names[state];
}
static void home_weather_icon_set(const lv_image_dsc_t *src){
    if(lv_image_get_src(weather_icon)!=src)lv_image_set_src(weather_icon,src);
    int bottom=0;
    for(int y=0;y<src->header.h;y++)for(int x=0;x<src->header.w;x++)
        if(src->data[y*src->header.stride+x])bottom=y+1;
    lv_obj_set_y(weather_icon,value_row.baseline+1-bottom);
}
static void render_weather(void) {
    if(!weather_value)return;
    /* Scratch is UI-owned under the existing LVGL lock, not nested task stack. */
    static weather_state_t w;w=weather_read();network_state_t n=app_network_read();
    uint64_t now=uptime();int64_t utc=app_utc_us()/1000000;char text[256];
    bool cv=weather_current_valid(&w.data.current,utc,now,n.time_valid);
    const char *reason=!w.configured?"设置天气位置":!n.time_valid?"等待校时":!w.data.current.present?(w.fetching?"正在获取天气":!n.connected?"尚未联网":"天气暂不可用"):!cv?"当前天气已过期":w.error!=WX_OK&&w.error!=WX_PARTIAL?"更新失败":!n.connected?"离线缓存":"";
    if(cv)snprintf(text,sizeof text,"天气 · %s%s%s",kind_text(w.data.current.code),reason[0]?" · ":"",reason);
    else snprintf(text,sizeof text,"天气%s%s",w.data.current.present?" · ":"",w.data.current.present?(n.time_valid?"已过期":"待校时"):"");
    text_if_changed(weather_title,text);
    if(cv)snprintf(text,sizeof text,"%.1f°C",w.data.current.temperature_c);else snprintf(text,sizeof text,"--°C");
    home_reading_set(&home_weather,text);
    weather_hint_t current_hint=weather_hint_current(&w.data.current,&w.air_quality,utc,now,n.time_valid);
    bool current_notice=current_hint.kind!=WEATHER_HINT_NONE;
    bool current_windy=current_hint.kind==WEATHER_HINT_WIND||current_hint.kind==WEATHER_HINT_GUST;
    weather_hint_text(current_hint,true,text,sizeof text);
    text_if_changed(weather_current_wind,text);lv_obj_set_hidden(weather_current_wind,!current_notice);
    lv_obj_set_hidden(weather_wind_symbol,!current_windy);lv_obj_set_hidden(weather_wind_separator,!current_notice);
    const lv_image_dsc_t *src=kind_icon(cv?w.data.current.code:999);
    if(lv_image_get_src(weather_icon)!=src)home_weather_icon_set(src);
    char today_date[11];const weather_day_t *td=weather_day_at(&w.data,0,utc,now,n.time_valid,today_date);
    if(td)snprintf(text,sizeof text,"%s%s%.0f～%.0f°",w.injected?"样例 · ":"",td->range_estimated?"约":"",td->low_c,td->high_c);
    else snprintf(text,sizeof text,"%s%s",w.injected?"样例 · ":"",current_notice?"--":reason[0]?reason:"今天高低温暂无");
    text_if_changed(weather_note,text);
    int note_width=home_text_width(text,note_row.font),hint_width=current_notice?home_text_width(lv_label_get_text(weather_current_wind),note_row.font):0;
    int hint_gap=current_windy?66:32;
    int note_limit=current_notice?360-hint_width-hint_gap:360;
    lv_obj_set_width(weather_note,note_width>note_limit?note_width:note_limit);
    int note_scale=note_width>note_limit?note_limit*256/note_width:256;
    lv_obj_set_style_transform_scale_x(weather_note,note_scale,0);
    lv_obj_set_style_transform_pivot_x(weather_note,0,0);
    if(note_width>note_limit)note_width=note_limit;
    lv_obj_set_pos(weather_wind_symbol,HOME_LEFT+note_width+32,lv_obj_get_y(weather_note)+8);
    lv_obj_set_pos(weather_current_wind,HOME_LEFT+note_width+hint_gap,lv_obj_get_y(weather_note));
    lv_obj_set_width(weather_current_wind,current_notice?hint_width:1);
    if(current_notice)home_hint_separator_position(HOME_LEFT+note_width,current_windy);
    weather_forecast_summary(&w.data,utc,now,n.time_valid,&forecast);
    bool estimated_forecast=false;
    for(int i=0;i<WEATHER_DAYS;i++) {
        const weather_forecast_slot_t *slot=&forecast.slots[i];
        if(slot->state==WEATHER_SLOT_USABLE&&w.data.days[slot->data_index].range_estimated)estimated_forecast=true;
    }
    const char *refresh=w.fetching?"更新中":!n.connected?"离线":
        w.error!=WX_OK&&w.error!=WX_PARTIAL?"更新失败":"";
    char acquired[24]="--";
    if(forecast.usable_count)weather_time(forecast.oldest_fetched_utc,w.data.offset_s,"%H:%M",acquired,sizeof acquired);
    if(!w.configured)snprintf(text,sizeof text,"设置天气位置");
    else if(!n.time_valid)snprintf(text,sizeof text,"等待校时");
    else if(!forecast.time_known)snprintf(text,sizeof text,"等待天气日期%s%s",refresh[0]?" ":"",refresh);
    else if(!forecast.usable_count)snprintf(text,sizeof text,"%s%s%s",forecast.expired_count?"预报已过期":"预报暂无",refresh[0]?" ":"",refresh);
    else if(forecast.mixed)snprintf(text,sizeof text,"部分更新 · 最早%s%s",refresh[0]?" · ":"",refresh);
    else if(forecast.usable_count<WEATHER_DAYS)snprintf(text,sizeof text,"预报%u/7%s%s",forecast.usable_count,refresh[0]?" · ":"",refresh);
    else snprintf(text,sizeof text,"%s",refresh);
    if(estimated_forecast) {
        static const char prefix[]="参考温区 · ";size_t length=strlen(text),n=sizeof prefix-1;
        if(!length)snprintf(text,sizeof text,"参考温区");
        else if(length+n<sizeof text){memmove(text+n,text,length+1);memcpy(text,prefix,n);}
    }
    if(w.injected){
        /* Keep sample identity and refresh/coverage visible in the narrow footer.
         * Full acquired range remains in Sources when this short form is used. */
        if(refresh[0]&&forecast.usable_count){
            if(forecast.mixed)snprintf(text,sizeof text,"部分更新 %s",refresh);
            else if(forecast.usable_count<WEATHER_DAYS)snprintf(text,sizeof text,"预报%u/7 %s",forecast.usable_count,refresh);
        }
        static const char sample[]="样例 ";size_t length=strlen(text),prefix=sizeof sample-1;
        if(length+prefix<sizeof text){memmove(text+prefix,text,length+1);memcpy(text,sample,prefix);}
    }
    /* Static acquisition mark, never a refresh action. Preserve coverage and
     * failure text separately, so the time keeps its quiet 24px numeric font. */
    text_if_changed(weather_update_state,text);lv_obj_set_hidden(weather_update_state,!w.configured);
    bool show_acquired=w.configured&&n.time_valid&&forecast.usable_count;
    text_if_changed(weather_update,show_acquired?acquired:"");
    lv_obj_set_hidden(weather_update,!show_acquired);lv_obj_set_hidden(weather_update_icon,!show_acquired);
    int state_width=weather_text_width(text),time_width=0;
    if(show_acquired){
        lv_point_t size;lv_text_get_size(&size,acquired,&lv_font_montserrat_24,0,0,374,LV_TEXT_FLAG_NONE);time_width=size.x;
        int available=374-22-8-time_width-(state_width?8:0);if(state_width>available)state_width=available;
        lv_obj_set_pos(weather_update_icon,48+state_width+(state_width?8:0),550);
        lv_obj_set_pos(weather_update,48+state_width+(state_width?8:0)+30,547);
    }
    lv_obj_set_width(weather_update_state,show_acquired?(state_width?state_width:1):374);
    weather_provider_t daily_provider=WX_PROVIDER_UNKNOWN;
    for(unsigned i=0;i<WEATHER_DAYS;i++)if(w.data.days[i].present){daily_provider=w.data.days[i].provider;break;}
    snprintf(text,sizeof text,"当前 %s\n七日 %s\n天气时区 %s",
        w.data.current.present?weather_provider_name(w.data.current.provider):"暂无",
        daily_provider!=WX_PROVIDER_UNKNOWN?weather_provider_name(daily_provider):"暂无",
        w.data.timezone[0]?w.data.timezone:"尚未取得");
    text_if_changed(weather_zone,text);
    lv_obj_set_hidden(weather_empty,w.configured);
    const char *week[]={"周日","周一","周二","周三","周四","周五","周六"};
    for(int i=0;i<7;i++) {
        const weather_forecast_slot_t *slot=&forecast.slots[i];
        const char *date=slot->date;bool valid=slot->state==WEATHER_SLOT_USABLE;
        const weather_day_t *d=valid?&w.data.days[slot->data_index]:NULL;
        if(forecast.time_known)snprintf(text,sizeof text,"%.2s/%.2s",date+5,date+8);else text[0]=0;
        text_if_changed(day_date[i],text);
        lv_obj_set_hidden(day_date[i],!forecast.time_known);
        if(forecast.time_known){time_t local=utc+w.data.offset_s+i*86400LL;struct tm tm;gmtime_r(&local,&tm);snprintf(text,sizeof text,"%s",i==0?"今天":week[tm.tm_wday]);}
        else snprintf(text,sizeof text,"%s",n.time_valid?"日期待定":"待校时");
        text_if_changed(day_week[i],text);
        const lv_image_dsc_t *icon=forecast_kind_icon(valid?d->code:999);
        if(lv_image_get_src(day_icon[i])!=icon)lv_image_set_src(day_icon[i],icon);
        text_if_changed(day_kind[i],valid?kind_text(d->code):slot->state==WEATHER_SLOT_EXPIRED?"已过期":"暂无");
        uint32_t ink=valid?browse_palette->ink:browse_palette->sub;
        lv_obj_set_style_text_color(day_kind[i],lv_color_hex(ink),0);
        lv_obj_set_style_text_color(day_range[i],lv_color_hex(ink),0);
        if(valid)snprintf(text,sizeof text,"%s%.0f～%.0f°",d->range_estimated?"约":"",d->low_c,d->high_c);else snprintf(text,sizeof text,"--");
        text_if_changed(day_range[i],text);
        int x=48+i*928/7,width=(i+1)*928/7-i*928/7,range_width=weather_text_width(text);
        /* Compress only pathological accepted extremes; ordinary ranges retain
         * the actual 28px font and no transformed layer. */
        lv_obj_set_width(day_range[i],range_width>width?range_width:width);
        lv_obj_set_style_transform_scale_x(day_range[i],range_width>width?width*256/range_width:256,0);
        lv_obj_set_style_transform_pivot_x(day_range[i],0,0);
        lv_obj_set_x(day_range[i],x);
        weather_hint_t selected=weather_hint_day(d,utc,now,n.time_valid);
        day_hint_t hint=day_hint_style(selected.kind);weather_hint_text(selected,false,text,sizeof text);
        day_hint_kind[i]=hint;text_if_changed(day_rain[i],text);
        bool wind_graphic=hint==DAY_HINT_WIND||hint==DAY_HINT_GUST;
        bool graphic=wind_graphic||hint==DAY_HINT_RAIN;
        lv_obj_set_hidden(day_hint_icon[i],!graphic);
        lv_obj_set_hidden(lv_obj_get_child(day_hint_icon[i],0),!wind_graphic);
        lv_obj_set_hidden(lv_obj_get_child(day_hint_icon[i],1),hint!=DAY_HINT_RAIN);
        lv_point_t size;lv_text_get_size(&size,text,&clock_font_hint_24,0,0,1024,LV_TEXT_FLAG_NONE);
        int group_width=size.x+(graphic?28:0),left=x+(width-group_width)/2;
        lv_obj_set_pos(day_hint_icon[i],left,379);
        lv_obj_set_pos(day_rain[i],left+(graphic?28:0),374);lv_obj_set_width(day_rain[i],size.x);
        lv_obj_set_pos(day_kind[i],x,332);lv_obj_set_width(day_kind[i],width);
    }
    /* Detail scratch is static: long metadata is never added to the LVGL stack. */
    static char details[3072];char first[32]="--",last[32]="--",aq_source[24]="--",aq_fetch[24]="--";
    if(forecast.usable_count){
        weather_time(forecast.oldest_fetched_utc,w.data.offset_s,"%Y-%m-%d %H:%M",first,sizeof first);
        weather_time(forecast.newest_fetched_utc,w.data.offset_s,"%Y-%m-%d %H:%M",last,sizeof last);
    }
    bool aq_valid=air_quality_valid(&w.air_quality,utc,now,n.time_valid);
    if(w.air_quality.present) {
        weather_time(w.air_quality.source_utc,0,"%m/%d %H:%M",aq_source,sizeof aq_source);
        weather_time(w.air_quality.fetched_utc,0,"%m/%d %H:%M",aq_fetch,sizeof aq_fetch);
    }
    snprintf(details,sizeof details,"%s当前天气：%s\n预报：可用%u/7，缺失%u，过期%u%s\n%s\n取得范围：%s 至 %s\n静态更新图形与时间表示设备成功取得记录，并非模型发布时间；不是刷新按钮。\n日期依据：%s\n每日一项依次为：更强风(稳风8级/阵风9级起)、冻雨、冰雹、雷雨(显示雷电)、较强雨雪、较强风(稳风6级/阵风7级起)、UV 强、降水概率、普通风、未知。不是官方预警。\n雨滴表示当日最大降水概率，含雨雪；普通雨雪的有效0%%仍保留，未知用横线表示。\nUV 强：当日最大紫外线指数达到6，非当前夜晚实测；缺失不推算。\n风：10米模型参考风级，非实测；各档稳风/阵风过门后取较高级数，同级稳风优先。冻雨含天气码56/57的冻毛毛雨；只按实际天气码提示。\n日最大风：标准源为日最大模型风；MET参考日为离散样本最大风，非连续日最大；阵风独立保留。\n风速以m/s取得，按0.1m/s精度半向上分级。\n参考温区：MET未来完整日采样温度范围，并非标准日高低温；约表示参考温区，今天不完整不补。\n空气质量：%s，CAMS全球模型 / Open-Meteo。\n美标AQI>100显示空气欠佳；非中国AQI、本地测量或官方预警。\n每3小时取数，独立取得有效期%u小时；仅当前小时的模型值可用。\n空气有效时刻UTC %s；取得UTC %s。\n逐日来源与取得状态：",
        w.injected?"开发样例，非实时读数。\n":"",!n.time_valid?"待校时":cv?"可用":w.data.current.present?"已过期":"暂无",
        forecast.usable_count,forecast.missing_count,forecast.expired_count,forecast.mixed?"，部分更新":"",refresh[0]?refresh:"本次未在刷新",
        first,last,forecast.time_known?location_weather_date_basis(w.data.timezone):n.time_valid?"等待天气日期":"等待校时",
        aq_valid?"可用":w.air_quality.present?"已过期":"暂无",(unsigned)(AIR_QUALITY_MAX_AGE/3600),aq_source,aq_fetch);
    for(int i=0;i<WEATHER_DAYS;i++){
        const weather_forecast_slot_t *slot=&forecast.slots[i];char at[32]="--";
        const weather_day_t *d=slot->data_index>=0?&w.data.days[slot->data_index]:NULL;
        if(d)weather_time(d->fetched_utc,w.data.offset_s,"%m/%d %H:%M",at,sizeof at);
        size_t used=strlen(details);snprintf(details+used,sizeof details-used,"\n%s %s %s %s 取得%s",slot->date[0]?slot->date:"--",forecast_slot_text(slot->state),d?weather_provider_name(d->provider):"--",d?(d->range_estimated?"参考温区":"日高低温"):"--",at);
    }
    text_if_changed(weather_notice,details);
}
static void render(lv_timer_t *timer) {
    (void)timer;
    if(am_service_copy_into(&alarm_store,&alarm_runtime)&&alarm_runtime.known&&alarm_model.known&&!alarm_model.busy){
        if(!am_config_equal(&alarm_model.confirmed,&alarm_runtime.config)){
            alarm_model.confirmed=alarm_runtime.config;alarm_model.revision++;
        }
        if(alarm_runtime.blocked&&!alarm_model.blocked){alarm_model.blocked=true;alarm_model.note=AM_NOTE_UNCERTAIN;alarm_model.revision++;}
    }
    sensor_state_t s=app_state_read(); uint64_t now=uptime(); char text[96];
    network_state_t n=app_network_read(); render_clock(&n);
    wifi_settings=network_settings_read();
    lv_obj_set_hidden(wifi_setup,wifi_settings.saved_present&&!wifi_settings.storage_error&&!wifi_settings.save_unconfirmed);
    text_if_changed(lv_obj_get_child(wifi_setup,0),!wifi_settings.initialized?"读取设置中":wifi_settings.save_unconfirmed?"Wi-Fi设置待确认":wifi_settings.storage_error?"设置读取失败":"设置 Wi-Fi");
    text_if_changed(wifi_summary,!wifi_settings.initialized?"读取中":wifi_settings.save_unconfirmed?"未能确认保存":wifi_settings.storage_error?"设置读取失败":wifi_settings.saved_present?(n.connected?"已保存 · 已连接":"已保存 · 正在连接"):"未设置");
    const char *wifi_state_note=wifi_settings.save_unconfirmed?(n.connected?(n.time_valid?"已连接 · 时间已校准":"已连接 · 时间尚未校准"):"尚未连接，可重试"):
        wifi_settings.initialized&&!wifi_settings.storage_error&&wifi_settings.saved_present&&n.connected&&!n.time_valid?"时间尚未校准":"";
    setting_detail(wifi_summary,wifi_detail,wifi_state_note);
    text_if_changed(brightness_summary,"自动调节");
    setting_detail(brightness_summary,brightness_detail,preview_restore_unconfirmed?"亮度恢复未确认，可重试":"");
    snprintf(text,sizeof text,"%s · %s",n.connected?"已连接":"正在连接",n.time_valid?"时间已校准":"时间尚未校准");
    text_if_changed(first_status,text);
    wifi_ui_refresh();
    location_ui_refresh();
    ring_refresh();
    volume_refresh();
    if(browse_moving()&&!browse_allowed())browse_cancel();
    bool overlay=wifi_ui_visible()||location_ui_visible()||ring_blocks_navigation();
    /* Sources remains an idle-returning read-only page, but has no pager/swipe. */
    bool browsing=browse_allowed()||(page==4&&brightness>0&&!overlay);
    if(!browsing||overlay_was_visible||physical_browse.down||virtual_browse.down)browse_activity=now;
    overlay_was_visible=overlay;
    if(browsing&&page!=0&&now-browse_activity>=60000)show_page(0);
    render_weather();
    /* Format full location identity after the weather frame has returned:
     * its by-value location copy + float printf must not nest under that frame. */
    location_ui_saved_weather(weather_place,weather_admin,weather_source_identity,540);
    lv_obj_update_layout(weather_source_body);
    lv_obj_set_y(weather_source_identity,lv_obj_get_height(weather_source_license)+24);
    lv_obj_set_y(weather_notice,lv_obj_get_y(weather_source_identity)+lv_obj_get_height(weather_source_identity)+24);
    lv_obj_set_y(weather_zone,lv_obj_get_y(weather_notice)+lv_obj_get_height(weather_notice)+24);
    location_ui_saved_label(location_name);
    static weather_state_t wx;saved_location=location_read();wx=weather_read();
    if(saved_location.save_unconfirmed)text_if_changed(location_name,"未能确认保存");
    else if(saved_location.storage_error)text_if_changed(location_name,"设置读取失败");
    else if(!saved_location.initialized)text_if_changed(location_name,"读取中");
    else if(!saved_location.configured)text_if_changed(location_name,"未设置");
    const char *place_note=saved_location.save_unconfirmed?"可重试":saved_location.storage_error||!saved_location.initialized||!saved_location.configured?"":
        !n.connected?"地点已保存 · 等待联网":!n.time_valid?"地点已保存 · 等待校时":wx.fetching?"地点已保存 · 正在获取天气":
        weather_current_valid(&wx.data.current,app_utc_us()/1000000,now,n.time_valid)?"":"地点已保存 · 天气暂不可用";
    setting_detail(location_name,location_summary,place_note);
    display_policy_step(&display_policy,&s,&n,&wx,&saved_location,app_utc_us()/1000,now,page==0&&!overlay&&!browse_moving());
    apply_browse_palette();
    if(!brightness_override){
        if(backlight_failed){
            /* Retry a no-change write first. A recovered driver never jumps
             * to a target that drifted while the hardware was unavailable. */
            if(now>=backlight_retry_ms){
                if(set_brightness(brightness)){automatic_resume();backlight_failed=false;}
                else backlight_retry_ms=now+1000;
            }
        }else if(brightness!=display_policy.env.brightness.target_percent){
            if(!set_brightness(display_policy.env.brightness.target_percent)){
                backlight_failed=true;backlight_retry_ms=now+1000;
            }
        }
    }
    background_runtime_submit(&display_policy.frame,page==0&&!overlay&&!browse_moving());
    af_time reference={0};
    if(n.time_valid){time_t seconds=(time_t)(app_utc_us()/1000000+8*3600);struct tm tm;gmtime_r(&seconds,&tm);
        reference=(af_time){tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec};}
    /* Always pump hidden async replies. Formal candidates/calendar notice come
     * from the owner's single approved calendar+ledger generation. */
    am_ui_refresh(&reference,n.time_valid,false,NULL,0,display_policy.low_light);
    am_summary next=alarm_runtime.next;
    lv_obj_set_hidden(alarm_summary,next.status==AM_SUMMARY_HIDDEN);
    alarm_summary_text(next,&reference,app_utc_us()/1000000,text,sizeof text);
    text_if_changed(lv_obj_get_child(alarm_summary,1),text);
    const lv_image_dsc_t *faces[]={&comfort_unknown,&comfort_comfortable,&comfort_hot,&comfort_cold,&comfort_dry,&comfort_humid};
    const lv_image_dsc_t *night_faces[]={&comfort_unknown_night,&comfort_comfortable_night,&comfort_hot_night,&comfort_cold_night,&comfort_dry_night,&comfort_humid_night};
    const lv_image_dsc_t *face=(display_policy.low_light?night_faces:faces)[display_policy.env.comfort.state];
    if(lv_image_get_src(comfort_icon)!=face){
        lv_image_set_src(comfort_icon,face);
        home_icon_position(comfort_icon,&note_row,face,HOME_INDOOR);
    }
    static int applied_low_light=-1;
    if(applied_low_light!=(int)display_policy.low_light){
        applied_low_light=(int)display_policy.low_light;
        uint32_t ink=display_policy.low_light?0x9caebc:0xf0f3f3,muted=display_policy.low_light?0x778994:0xc4d1d8;
        lv_obj_set_style_text_color(clock_label,lv_color_hex(ink),0);
        home_reading_color(&home_temperature,ink);home_reading_color(&home_humidity,ink);home_reading_color(&home_weather,ink);
        lv_obj_set_style_image_recolor(weather_icon,lv_color_hex(muted),0);
        lv_obj_set_style_image_recolor_opa(weather_icon,LV_OPA_COVER,0);
        lv_obj_t *details[]={date_label,weather_title,weather_note,sensor_note,indoor_title,weather_current_wind,weather_wind_separator,lv_obj_get_child(alarm_summary,1)};
        for(unsigned i=0;i<sizeof details/sizeof details[0];i++)lv_obj_set_style_text_color(details[i],lv_color_hex(muted),0);
    }
    if(measurement_valid(&s.sht,now)) {
        snprintf(text,sizeof text,"%.1f°C",s.sht.first); home_reading_set(&home_temperature,text);
        snprintf(text,sizeof text,"%.0f%%",s.sht.second); home_reading_set(&home_humidity,text);
        static const char *reasons[]={"","偏热","偏冷","偏干","偏湿"};
        const comfort_result_t *c=&display_policy.env.comfort;
        if(c->state==COMFORT_PENDING)snprintf(text,sizeof text,"待判断");
        else if(!c->reason_count)snprintf(text,sizeof text,"舒适");
        else snprintf(text,sizeof text,"%s%s%s",reasons[c->reasons[0]],c->reason_count>1?" · ":"",c->reason_count>1?reasons[c->reasons[1]]:"");
        text_if_changed(sensor_note,text);
    } else {
        home_reading_set(&home_temperature,"--°C"); home_reading_set(&home_humidity,"--%");
        text_if_changed(sensor_note,"温湿度暂不可用");
    }
#if CONFIG_CLOCK_DIAGNOSTICS
    /* UI-only samples never enter shared sensor/weather state or storage. */
    lv_obj_set_hidden(layout_sample_note,layout_sample==0);
    if(layout_sample) {
        static const char *temps[]={"","-45.0°C","130.0°C","1.0°C","--°C"};
        static const char *humidities[]={"","100%","100%","9%","--%"};
        home_reading_set(&home_temperature,temps[layout_sample]);
        home_reading_set(&home_humidity,humidities[layout_sample]);
        text_if_changed(sensor_note,layout_sample==4?"温湿度暂不可用":"待判断");
    }
#endif
    rendered_sequence=s.sequence;
    pager_refresh(now);
    if(timer)lvgl_stack_free_min=uxTaskGetStackHighWaterMark(NULL);
}
static void create_ui(void) {
    lv_obj_t *scr=lv_screen_active(); lv_obj_set_scrollable(scr,false);
    lv_obj_set_style_bg_color(scr,lv_color_black(),0);
    home=box(scr,0,0,1024,600,0);
    background_runtime_attach(home);
    alarm_summary=box(home,40,32,272,64,0);lv_obj_set_style_bg_opa(alarm_summary,LV_OPA_TRANSP,0);
    lv_obj_set_clickable(alarm_summary,true);lv_obj_add_event_cb(alarm_summary,clicked,LV_EVENT_CLICKED,(void *)(intptr_t)12);
    image(alarm_summary,&clock_navigation_bell,8,14);
    label(alarm_summary,"--:--",56,7,&clock_home_font_32,0x9caebc);
    lv_obj_set_hidden(alarm_summary,true);
    date_label=label(home,"时间尚未校准",0,32,&clock_home_font_32,0x778994);
    lv_obj_set_width(date_label,1024); lv_obj_set_style_text_align(date_label,LV_TEXT_ALIGN_CENTER,0);
    lv_font_glyph_dsc_t clock_zero;assert(lv_font_get_glyph_dsc(&clock_home_font_324,&clock_zero,'0',0));
    int clock_top=104-(clock_home_font_324.line_height-clock_home_font_324.base_line)+clock_zero.box_h+clock_zero.ofs_y;
    clock_label=label(home,"--:--",0,clock_top,&clock_home_font_324,0x9caebc);
    lv_obj_set_width(clock_label,1024); lv_obj_set_style_text_align(clock_label,LV_TEXT_ALIGN_CENTER,0);
    gear_button(home);
    wifi_setup=button(home,"设置 Wi-Fi",48,300,240,7);
    weather_title=home_label(&title_row,"天气",HOME_LEFT,360,0x778994);
    weather_value=home_reading_create(&home_weather,HOME_LEFT,254,"--°C");
    weather_note=home_label(&note_row,"天气暂不可用",HOME_LEFT,360,0x778994);
    weather_icon=image(home,&weather_home_cloudunknown,318,450);home_weather_icon_set(&weather_home_cloudunknown);
    weather_current_wind=home_label(&note_row,"",HOME_LEFT,1,0x778994);
    lv_label_set_long_mode(weather_current_wind,LV_LABEL_LONG_CLIP);lv_obj_set_hidden(weather_current_wind,true);
    weather_wind_separator=home_label(&note_row,"·",HOME_LEFT,home_text_width("·",note_row.font),0x778994);
    weather_wind_symbol=wind_symbol(home);
    lv_obj_set_hidden(weather_wind_separator,true);lv_obj_set_hidden(weather_wind_symbol,true);
    weather_hit=box(home,40,384,365,184,0);
    lv_obj_set_style_bg_opa(weather_hit,LV_OPA_TRANSP,0);lv_obj_set_clickable(weather_hit,true);
    lv_obj_add_event_cb(weather_hit,clicked,LV_EVENT_CLICKED,(void *)(intptr_t)9);
    box(home,HOME_DIVIDER,390,1,172,0x344653);
    home_icon(&title_row,&clock_house_icon,HOME_INDOOR);
    int reading_x=HOME_INDOOR+HOME_ICON_SIZE+HOME_ICON_GAP;
    int humidity_x=HOME_HUMIDITY+HOME_ICON_SIZE+HOME_ICON_GAP;
    indoor_title=home_label(&title_row,"室内",reading_x,HOME_RIGHT-reading_x,0x778994);
    comfort_icon=home_icon(&note_row,&comfort_unknown_night,HOME_INDOOR);
    sensor_note=home_label(&note_row,"待判断",HOME_INDOOR+48,HOME_RIGHT-HOME_INDOOR-48,0x778994);
    home_icon(&value_row,&clock_softTemp_icon,HOME_INDOOR);
    temperature_label=home_reading_create(&home_temperature,reading_x,HOME_HUMIDITY-reading_x-HOME_ICON_GAP,"--°C");
    home_icon(&value_row,&clock_softDrop_icon,HOME_HUMIDITY);
    humidity_label=home_reading_create(&home_humidity,humidity_x,HOME_RIGHT-humidity_x,"--%");
#if CONFIG_CLOCK_DIAGNOSTICS
    layout_sample_note=label(home,"布局样例 · 非测量",HOME_INDOOR,350,&clock_font_28,0x778994);
#endif
    weather_page=box(scr,0,0,1024,600,0);
    /* The 28px location font contains all GB2312 glyphs; 44px is a UI subset. */
    weather_place=label(weather_page,"未配置",48,46,&clock_font_28,0xc4d1d8);
    lv_obj_set_width(weather_place,540);lv_label_set_long_mode(weather_place,LV_LABEL_LONG_DOT);
    weather_clock=label(weather_page,"--:--",798,46,&clock_font_28,0x778994);
    gear_button(weather_page);
    weather_admin=label(weather_page,"",48,46,&clock_font_28,0x778994);
    lv_obj_set_width(weather_admin,200);lv_label_set_long_mode(weather_admin,LV_LABEL_LONG_DOT);
    for(int i=0;i<7;i++) {
        int x=48+i*928/7,width=(i+1)*928/7-i*928/7;
        day_week[i]=day_label(i,143,i?0x9caebc:0xf0f3f3);
        day_date[i]=day_label(i,186,i?0x778994:0x9caebc);
        lv_obj_set_style_text_font(day_date[i],&lv_font_montserrat_24,0);
        /* Match the previous center and 80px display size without enlarging
         * the 48px home artwork. The shared palette colors this A8 mask. */
        day_icon[i]=image(weather_page,&weather_forecast_cloudunknown,x+(width-80)/2,237);
        day_kind[i]=day_label(i,332,0xc4d1d8);
        day_rain[i]=day_label(i,374,0x9caebc);
        lv_obj_set_style_text_font(day_rain[i],&clock_font_hint_24,0);lv_obj_set_height(day_rain[i],32);
        day_hint_icon[i]=hint_graphic(weather_page,false);
        day_range[i]=day_label(i,418,0xc4d1d8);
        lv_obj_set_style_text_font(day_range[i],&clock_font_hint_28,0);
    }
    weather_update_state=label(weather_page,"",48,540,&clock_font_28,0x778994);
    lv_obj_set_width(weather_update_state,374);lv_label_set_long_mode(weather_update_state,LV_LABEL_LONG_DOT);
    weather_update_icon=hint_graphic(weather_page,true);lv_obj_set_pos(weather_update_icon,48,550);
    weather_update=label(weather_page,"",78,547,&lv_font_montserrat_24,0x778994);
    lv_obj_set_size(weather_update,100,32);lv_label_set_long_mode(weather_update,LV_LABEL_LONG_CLIP);
    lv_obj_t *source_hit=button(weather_page,"来源",804,528,180,11);lv_obj_set_height(source_hit,64);
    lv_obj_set_style_bg_opa(source_hit,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(source_hit,0,0);
    weather_empty=button(weather_page,"设置天气位置",48,536,300,8);lv_obj_set_height(weather_empty,64);
    weather_sources=box(scr,0,0,1024,600,0);child_header(weather_sources,"返回","天气来源与许可",9);
    weather_source_body=box(weather_sources,48,130,928,414,0);lv_obj_set_scrollable(weather_source_body,true);
    lv_obj_set_scroll_dir(weather_source_body,LV_DIR_VER);lv_obj_set_scrollbar_mode(weather_source_body,LV_SCROLLBAR_MODE_AUTO);
    weather_source_license=label(weather_source_body,"Open-Meteo · https://open-meteo.com/\nCAMS ENSEMBLE data provider · https://atmosphere.copernicus.eu/\nMET Norway · https://api.met.no/ · CC BY 4.0\nGeoNames · https://www.geonames.org/ · CC BY 4.0\nhttps://creativecommons.org/licenses/by/4.0/\nWikidata · CC0；内政部地政司2026地名 · OGDL 1.0\nhttps://data.gov.tw/dataset/40281 · https://data.gov.tw/license\n天气为地点参考数据；本机筛选、格式化与图标映射。",0,0,&clock_font_hint_28,0x778994);
    weather_source_identity=label(weather_source_body,"",0,0,&clock_font_28,0x778994);
    weather_notice=label(weather_source_body,"",0,0,&clock_font_hint_28,0x778994);
    weather_zone=label(weather_source_body,"天气时区 尚未取得",0,0,&clock_font_28,0x778994);
    lv_obj_t *source_labels[]={weather_source_license,weather_source_identity,weather_notice,weather_zone};
    for(unsigned i=0;i<4;i++){lv_obj_set_width(source_labels[i],900);lv_label_set_long_mode(source_labels[i],LV_LABEL_LONG_WRAP);}
    am_ui_ports alarm_ports={.submit=alarm_submit,.take=alarm_take,.activity=alarm_activity,.home=alarm_home,.summary=alarm_next,.settings=alarm_settings,.settings_icon=&clock_gear_icon};
    assert(am_ui_create(scr,&alarm_model,&alarm_ports,&clock_font_28,&clock_font_44));
    am_ui_visible(false);
    lv_obj_t *pages[]={home,weather_page,am_ui_root()};
    for(int p=0;p<3;p++){
      pager_groups[p]=box(pages[p],440,552,144,48,0);lv_obj_set_style_bg_opa(pager_groups[p],LV_OPA_TRANSP,0);lv_obj_set_clickable(pager_groups[p],false);
      for(int i=0;i<3;i++) {
        lv_obj_t *hit=box(pager_groups[p],i*48,0,48,48,0);
        lv_obj_set_style_bg_opa(hit,LV_OPA_TRANSP,0);lv_obj_set_clickable(hit,true);
        lv_obj_set_click_focusable(hit,false);
        lv_obj_add_event_cb(hit,pager_clicked,LV_EVENT_CLICKED,(void *)(intptr_t)(i==0?10:i==1?9:12));
        indicators[p][i]=box(hit,i==p?18:22,22,i==p?12:4,4,i==p?0x9caebc:0x778994);
        lv_obj_set_style_bg_opa(indicators[p][i],i==p?191:102,0);lv_obj_set_style_radius(indicators[p][i],2,0);
        lv_obj_set_clickable(indicators[p][i],false);
      }
    }
    settings=box(scr,0,0,1024,600,0x071520);settings_header=child_header(settings,"返回","设置",4);
    setting_rows[0]=setting_row("显示亮度",144,2,&brightness_summary,&brightness_detail,true);
    setting_rows[1]=setting_row("闹铃音量",248,15,&volume_summary,&volume_detail,true);
    setting_rows[2]=setting_row("Wi-Fi",352,7,&wifi_summary,&wifi_detail,true);
    setting_rows[3]=setting_row("天气位置",456,8,&location_name,&location_summary,false);
    preview=box(scr,0,0,1024,600,0x071520);preview_header=child_header(preview,"返回","亮度预览",3);
    brightness_label=label(preview,"50%",0,186,&clock_font_44,0xf0f3f3);
    lv_obj_set_width(brightness_label,1024);lv_obj_set_style_text_align(brightness_label,LV_TEXT_ALIGN_CENTER,0);
    brightness_slider=lv_slider_create(preview);lv_obj_remove_style_all(brightness_slider);
    lv_obj_set_pos(brightness_slider,166,328);lv_obj_set_size(brightness_slider,692,8);
    lv_obj_set_ext_click_area(brightness_slider,28);lv_slider_set_range(brightness_slider,1,100);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(0x344653),0);lv_obj_set_style_bg_opa(brightness_slider,LV_OPA_COVER,0);
    lv_obj_set_style_radius(brightness_slider,4,0);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(0xb4cbd0),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(brightness_slider,LV_OPA_COVER,LV_PART_INDICATOR);lv_obj_set_style_radius(brightness_slider,4,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(brightness_slider,lv_color_hex(0xb4cbd0),LV_PART_KNOB);
    lv_obj_set_style_bg_opa(brightness_slider,LV_OPA_COVER,LV_PART_KNOB);lv_obj_set_style_radius(brightness_slider,22,LV_PART_KNOB);
    lv_obj_set_style_pad_all(brightness_slider,18,LV_PART_KNOB);
    lv_obj_set_style_border_width(brightness_slider,5,LV_PART_KNOB);lv_obj_set_style_border_color(brightness_slider,lv_color_hex(0x071520),LV_PART_KNOB);
    lv_obj_add_event_cb(brightness_slider,brightness_changed,LV_EVENT_VALUE_CHANGED,NULL);
    preview_min=label(preview,"1",144,376,&lv_font_montserrat_24,0x778994);
    preview_max=label(preview,"100",800,376,&lv_font_montserrat_24,0x778994);lv_obj_set_width(preview_max,80);lv_obj_set_style_text_align(preview_max,LV_TEXT_ALIGN_RIGHT,0);
    preview_explain=label(preview,"返回后恢复自动调节",0,448,&clock_font_28,0x9caebc);
    lv_obj_set_width(preview_explain,1024);lv_obj_set_style_text_align(preview_explain,LV_TEXT_ALIGN_CENTER,0);
    preview_note=label(preview,"",48,516,&clock_font_28,0xded0b7);lv_obj_set_width(preview_note,928);lv_obj_set_style_text_align(preview_note,LV_TEXT_ALIGN_CENTER,0);
    volume_page=box(scr,0,0,1024,600,0x071520);volume_header=child_header(volume_page,"返回","闹铃音量",16);
    volume_label=label(volume_page,"读取中",0,186,&clock_font_28,0xf0f3f3);
    lv_obj_set_width(volume_label,1024);lv_obj_set_style_text_align(volume_label,LV_TEXT_ALIGN_CENTER,0);
    volume_slider=lv_slider_create(volume_page);lv_obj_remove_style_all(volume_slider);
    lv_obj_set_pos(volume_slider,166,328);lv_obj_set_size(volume_slider,692,8);
    lv_obj_set_ext_click_area(volume_slider,28);lv_slider_set_range(volume_slider,0,100);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(0x344653),0);lv_obj_set_style_bg_opa(volume_slider,LV_OPA_COVER,0);
    lv_obj_set_style_radius(volume_slider,4,0);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(0xb4cbd0),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(volume_slider,LV_OPA_COVER,LV_PART_INDICATOR);lv_obj_set_style_radius(volume_slider,4,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider,lv_color_hex(0xb4cbd0),LV_PART_KNOB);
    lv_obj_set_style_bg_opa(volume_slider,LV_OPA_COVER,LV_PART_KNOB);lv_obj_set_style_radius(volume_slider,22,LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_slider,18,LV_PART_KNOB);
    lv_obj_set_style_border_width(volume_slider,5,LV_PART_KNOB);lv_obj_set_style_border_color(volume_slider,lv_color_hex(0x071520),LV_PART_KNOB);
    lv_obj_add_event_cb(volume_slider,volume_event,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(volume_slider,volume_event,LV_EVENT_VALUE_CHANGED,NULL);
    lv_obj_add_event_cb(volume_slider,volume_event,LV_EVENT_RELEASED,NULL);
    lv_obj_add_event_cb(volume_slider,volume_event,LV_EVENT_PRESS_LOST,NULL);
    volume_min=label(volume_page,"0",144,376,&lv_font_montserrat_24,0x778994);
    volume_max=label(volume_page,"100",800,376,&lv_font_montserrat_24,0x778994);lv_obj_set_width(volume_max,80);lv_obj_set_style_text_align(volume_max,LV_TEXT_ALIGN_RIGHT,0);
    volume_explain=label(volume_page,"拖动试听，松手停止",0,448,&clock_font_28,0x9caebc);
    lv_obj_set_width(volume_explain,1024);lv_obj_set_style_text_align(volume_explain,LV_TEXT_ALIGN_CENTER,0);
    volume_note=label(volume_page,"",48,516,&clock_font_28,0xded0b7);lv_obj_set_width(volume_note,928);lv_obj_set_style_text_align(volume_note,LV_TEXT_ALIGN_CENTER,0);
    first_next=box(scr,0,0,1024,600,0x071520);first_caption=label(first_next,"Wi-Fi",48,47,&clock_font_28,0xc4d1d8);
    first_title=label(first_next,"Wi-Fi设置已保存",96,168,&clock_font_44,0xf0f3f3);
    first_status=label(first_next,"",96,251,&clock_font_28,0x9caebc);
    first_location=button(first_next,"设置天气位置",96,335,264,13);
    first_later=button(first_next,"先用时钟",384,335,240,14);lv_obj_set_style_bg_opa(first_later,LV_OPA_TRANSP,0);lv_obj_set_style_border_width(first_later,0,0);
    const uint32_t samples[]={0x6e29,0x6e7f,0x4fdd,0x5b58,0x5149,0x7167,0x5ba4,0x5185,0x5f85,0x5224,0x65ad,0xb0,'C','%','-'};
    lv_font_glyph_dsc_t gd;
    for(unsigned i=0;i<sizeof samples/sizeof samples[0];i++) {
        if(!clock_font_28.get_glyph_dsc(&clock_font_28,&gd,samples[i],0)) font_missing++;
        if(!clock_font_44.get_glyph_dsc(&clock_font_44,&gd,samples[i],0)) font_missing++;
    }
    if(!clock_font_240.get_glyph_dsc(&clock_font_240,&gd,'-',0)) font_missing++;
    const uint32_t days[]={0x661f,0x671f,0x65e5,0x4e00,0x4e8c,0x4e09,0x56db,0x4e94,0x516d};
    for(unsigned i=0;i<sizeof days/sizeof days[0];i++) if(!clock_font_28.get_glyph_dsc(&clock_font_28,&gd,days[i],0)) font_missing++;
    if(!clock_font_28.get_glyph_dsc(&clock_font_28,&gd,0xff5e,0))font_missing++;
    for(char c='0';c<='9';c++) if(!clock_font_240.get_glyph_dsc(&clock_font_240,&gd,c,0)) font_missing++;
    wifi_ui_set_palette(browse_palette);location_ui_set_palette(browse_palette);
    wifi_ui_create(scr);location_ui_create(scr);
    wifi_ui_set_done(wifi_done);location_ui_set_done(location_done);
    ring_overlay=box(lv_layer_top(),0,0,1024,600,0x05090d);
    lv_obj_set_style_bg_opa(ring_overlay,LV_OPA_90,0);
    ring_title=label(ring_overlay,"闹铃",120,164,&clock_font_28,0xf0f3f3);lv_obj_set_width(ring_title,784);lv_obj_set_style_text_align(ring_title,LV_TEXT_ALIGN_CENTER,0);
    ring_note=label(ring_overlay,"",120,246,&clock_font_28,0x778994);lv_obj_set_width(ring_note,784);lv_obj_set_style_text_align(ring_note,LV_TEXT_ALIGN_CENTER,0);
    ring_stop=box(ring_overlay,312,340,400,112,0x172a3a);lv_obj_set_clickable(ring_stop,true);
    lv_obj_set_style_border_width(ring_stop,1,0);lv_obj_set_style_border_color(ring_stop,lv_color_hex(0x607787),0);lv_obj_set_style_radius(ring_stop,12,0);
    lv_obj_t *stop_text=label(ring_stop,"停止",0,0,&clock_font_28,0xc4d1d8);lv_obj_center(stop_text);
    lv_obj_add_event_cb(ring_stop,ring_clicked,LV_EVENT_CLICKED,NULL);lv_obj_set_hidden(ring_overlay,true);
    show_page(0); render(NULL); lv_timer_create(render,100,NULL);
}
static uint32_t fnv(const uint8_t *data,size_t len) { uint32_t h=2166136261u; for(size_t i=0;i<len;i++)h=(h^data[i])*16777619u; return h; }
static void snapshot(void) {
    render(NULL);
    lv_draw_buf_t *buf=lv_snapshot_take(lv_screen_active(),LV_COLOR_FORMAT_RGB565);
    if(!buf) { printf("{\"event\":\"error\",\"request_id\":%"PRIu32",\"reason\":\"snapshot_allocation\"}\n",request_id); return; }
    size_t length=buf->header.stride*buf->header.h;
    printf("{\"event\":\"image_begin\",\"request_id\":%"PRIu32",\"boot_id\":%"PRIu32",\"event_seq\":%"PRIu32",\"scene\":\"%s\",\"page\":%d,\"rendered_sequence\":%"PRIu32",\"width\":%u,\"height\":%u,\"stride\":%u,\"format\":\"RGB565LE\",\"length\":%u,\"fnv1a32\":%"PRIu32",\"motion_paused\":true}\n",request_id,boot_id,event_seq,background_scene_name(),page,rendered_sequence,buf->header.w,buf->header.h,buf->header.stride,(unsigned)length,fnv(buf->data,length));
    fflush(stdout);
    // Completed immutable snapshot remains valid while the UI lock is released.
    bsp_display_unlock();
    static const char hex[]="0123456789abcdef"; char line[600];
    for(size_t off=0;off<length;off+=256) {
      size_t n=length-off<256?length-off:256;
      int pos=snprintf(line,sizeof line,"{\"image_chunk\":%u,\"data\":\"",(unsigned)off);
      for(size_t j=0;j<n;j++){line[pos++]=hex[buf->data[off+j]>>4];line[pos++]=hex[buf->data[off+j]&15];}
      line[pos++]='"';line[pos++]='}';line[pos++]='\n';
      // Same stdout stream lock as event logs: physical touch cannot interleave bytes.
      size_t sent=fwrite(line,1,pos,stdout); fflush(stdout);
      if(sent!=(size_t)pos)break;
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    bsp_display_lock(-1); lv_draw_buf_destroy(buf);
    printf("{\"event\":\"image_end\",\"request_id\":%"PRIu32"}\n",request_id);
}
void app_ui_command(char *line) {
    char op[24], extra; unsigned rid; int a=0,b=0,c=0;
    if(sscanf(line,"%u %23s",&rid,op)!=2 || rid==0) return;
    request_id=rid;
    if(!bsp_display_lock(3000)) { printf("{\"event\":\"error\",\"request_id\":%u,\"reason\":\"lock_timeout\"}\n",rid); return; }
    bool ok=true;
    if(strcmp(op,"status")==0) { }
    else if(strcmp(op,"alarm_stop")==0 && sscanf(line,"%u %23s %c",&rid,op,&extra)==2)ring_stop_requested();
    else if(strcmp(op,"brightness")==0 && sscanf(line,"%u %23s %d %c",&rid,op,&a,&extra)==3 && a>=0 && a<=100) { brightness_override=true;ok=set_brightness(a); }
    else if((strcmp(op,"sht")==0 || strcmp(op,"bh")==0) && sscanf(line,"%u %23s %d %c",&rid,op,&a,&extra)==3 && a>=0 && a<=2) { app_state_inject(strcmp(op,"sht")==0,a); render(NULL); }
    else if(strcmp(op,"pointer")==0 && sscanf(line,"%u %23s %d %d %d %c",&rid,op,&a,&b,&c,&extra)==5 && a>=0 && a<1024 && b>=0 && b<600 && c>=0 && c<=1) {
      virtual_point=(lv_point_t){a,b}; virtual_state=c?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED; lv_indev_read(virtual_pointer);
    }
    else if(strcmp(op,"snapshot")==0 && wifi_ui_snapshot_allowed() && location_ui_snapshot_allowed()) snapshot();
#if CONFIG_CLOCK_DIAGNOSTICS
    else if(strcmp(op,"layout")==0 && sscanf(line,"%u %23s %d %c",&rid,op,&a,&extra)==3 && a>=0 && a<=4) { layout_sample=a; render(NULL); }
    else if(strncmp(op,"wifi_",5)==0 || strcmp(op,"savefail")==0) ok=wifi_ui_command(line);
    else if(strcmp(op,"location")==0) ok=location_command(line);
    else if(strcmp(op,"location_fill")==0) ok=location_ui_command(line);
#endif
    else ok=false;
    app_ui_status(ok?"result":"rejected",request_id); request_id=0; bsp_display_unlock();
}
void app_ui_start(void) {
    boot_id=esp_random();
    am_init(&alarm_model);
    if(!am_service_start(&alarm_store))printf("{\"event\":\"component_unavailable\",\"component\":\"alarm_store\"}\n");
    (void)am_reload(&alarm_model);
    if(!background_runtime_start())printf("{\"event\":\"component_unavailable\",\"component\":\"background\"}\n");
    display_policy_init(&display_policy,uptime(),50);
    bsp_display_cfg_t cfg={.lv_adapter_cfg=ESP_LV_ADAPTER_DEFAULT_CONFIG(),.rotation=ESP_LV_ADAPTER_ROTATE_0,.tear_avoid_mode=ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,.touch_flags={.swap_xy=0,.mirror_x=1,.mirror_y=1}};
    cfg.lv_adapter_cfg.task_stack_size=10*1024;
    lv_display_t *display=bsp_display_start_with_config(&cfg); ESP_ERROR_CHECK(display?ESP_OK:ESP_FAIL);
    ESP_ERROR_CHECK(bsp_display_brightness_set(50));
    ESP_ERROR_CHECK(bsp_display_lock(-1)?ESP_OK:ESP_FAIL);
    physical=bsp_display_get_input_dev(); physical_read=lv_indev_get_read_cb(physical); lv_indev_set_read_cb(physical,raw_read);
    if(CONFIG_CLOCK_DIAGNOSTICS) {
        virtual_pointer=lv_indev_create(); lv_indev_set_type(virtual_pointer,LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(virtual_pointer,virtual_read); lv_indev_set_display(virtual_pointer,display);
        lv_indev_set_mode(virtual_pointer,LV_INDEV_MODE_EVENT);
    }
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_REFR_READY,NULL);
#if CONFIG_CLOCK_DIAGNOSTICS
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_RENDER_START,NULL);
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_RENDER_READY,NULL);
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_FLUSH_START,NULL);
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_FLUSH_FINISH,NULL);
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_FLUSH_WAIT_START,NULL);
    lv_display_add_event_cb(display,refresh_cb,LV_EVENT_FLUSH_WAIT_FINISH,NULL);
#endif
    create_ui(); bsp_display_unlock();
}
