#include "background_runtime.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"
#include <string.h>
typedef struct { wb_frame_t frame; uint32_t revision; bool visible; } request_t;
static QueueHandle_t jobs;
static wb_workspace_t *workspace;
static uint8_t *pixels[2];
static lv_image_dsc_t images[2];
static lv_obj_t *image;
static unsigned front;
static uint32_t revision,rendered;
/* Only a completed publication can satisfy a later visibility restore. These
 * keys, like the image front, are read/written under the display lock. */
static bool published_valid;
static wb_scene_t published_scene;
static uint64_t published_animation_ms;
static void composite_veil(uint8_t *buffer){
    /* The home presentation's fixed veil, baked once into a completed back
     * frame instead of alpha-blending the whole screen on every drag refresh.
     * Match LVGL's RGB565 quantization exactly; no RGB888 approximation. */
    uint16_t color=lv_color_to_u16(lv_color_hex(0x041321));
    uint16_t *p=(uint16_t *)buffer;
    for(unsigned i=0;i<WB_OUTPUT_BYTES/2u;i++)p[i]=lv_color_16_16_mix(color,p[i],LV_OPA_10);
}
static void worker(void *unused){
    (void)unused;request_t request;
    for(;;){
        if(xQueueReceive(jobs,&request,portMAX_DELAY)!=pdTRUE||!request.visible)continue;
        unsigned back=1u-front;
        if(!wb_render(&request.frame,workspace,sizeof *workspace,pixels[back],WB_OUTPUT_BYTES))continue;
        /* Black, including the initial empty front, is exactly invariant under
         * this veil. Keep this work outside the display lock. */
        if(request.frame.scene!=WB_BLACK)composite_veil(pixels[back]);
        if(!bsp_display_lock(-1))continue;
        /* The lock excludes refresh and draw dispatch. LVGL 9.6 drains image
         * draw tasks before its refresh returns; panel DMA uses display draw
         * buffers, not these image sources. Never overwrite a live front. */
        if(image&&request.revision==revision){
            lv_image_cache_drop(&images[back]);lv_image_set_src(image,&images[back]);
            front=back;rendered++;
            published_scene=request.frame.scene;published_animation_ms=request.frame.animation_ms;published_valid=true;
        }
        bsp_display_unlock();
    }
}
bool background_runtime_start(void){
    if(jobs)return true;
    workspace=heap_caps_malloc(sizeof *workspace,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    for(unsigned i=0;i<2;i++){
        pixels[i]=heap_caps_calloc(1,WB_OUTPUT_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        images[i]=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,
            .w=WB_WIDTH,.h=WB_HEIGHT,.stride=WB_WIDTH*2},.data_size=WB_OUTPUT_BYTES,.data=pixels[i]};
    }
    jobs=xQueueCreate(1,sizeof(request_t));
    if(workspace&&pixels[0]&&pixels[1]&&jobs&&xTaskCreate(worker,"weather_draw",8192,NULL,2,NULL)==pdPASS)return true;
    if(jobs)vQueueDelete(jobs);
    jobs=NULL;
    heap_caps_free(workspace);workspace=NULL;
    for(unsigned i=0;i<2;i++){heap_caps_free(pixels[i]);pixels[i]=NULL;}
    return false;
}
void background_runtime_attach(lv_obj_t *parent){
    if(!jobs)return; /* Decorative allocation failure keeps the base clock. */
    image=lv_image_create(parent);lv_image_set_src(image,&images[front]);
    lv_obj_set_clickable(image,false);lv_obj_move_to_index(image,0);
}
void background_runtime_submit(const wb_frame_t *frame,bool visible){
    if(!jobs)return;
    /* Visibility/scene changes cancel a stale completion, including a hidden
     * frame. Phase-only updates coalesce without starving a slow renderer. */
    static wb_scene_t scene;static bool was_visible;
    static uint64_t submitted_ms;
    uint64_t now=(uint64_t)esp_timer_get_time()/1000u;
    bool changed=scene!=frame->scene||was_visible!=visible;
    if(changed){revision++;scene=frame->scene;was_visible=visible;}
    /* Keep the existing pixels when restoring the exact published frame.
     * Revision changes above still discard a stale/hidden worker result.
     * Never deduplicate against queued work: it may fail or be cancelled. */
    if(visible&&published_valid&&published_scene==frame->scene&&published_animation_ms==frame->animation_ms)return;
    if(changed||(frame->needs_redraw&&now-submitted_ms>=500u)){
        request_t r={*frame,revision,visible};(void)xQueueOverwrite(jobs,&r);submitted_ms=now;
    }
}
uint32_t background_runtime_frames(void){return rendered;}
