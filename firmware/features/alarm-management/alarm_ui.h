#ifndef ALARM_UI_H
#define ALARM_UI_H
#include "alarm_controller.h"
#include "lvgl.h"
#include "../../main/ui_palette.h"
typedef struct {
    bool (*submit)(void *ctx,const am_job *job);
    bool (*take)(void *ctx,am_reply *reply);
    void (*activity)(void *ctx);
    void (*home)(void *ctx);
    void *ctx;
    /* Optional for isolated component tests. Formal UI uses one owner copy. */
    bool (*summary)(void *ctx,am_summary *,bool *calendar_notice,bool *audio_ready);
    void (*settings)(void *ctx);
    const lv_image_dsc_t *settings_icon;
} am_ui_ports;
/* Shared scene colors cover the list, editor and delete confirmation. Keep the
 * original type/setter names for callers; focus and selection use these roles. */
typedef ui_palette_t am_ui_list_palette;
void am_ui_set_list_palette(const am_ui_list_palette *palette);
/* One app-lifetime component; all calls under the existing LVGL lock. */
bool am_ui_create(lv_obj_t *parent,am_controller *controller,const am_ui_ports *ports,
                  const lv_font_t *body,const lv_font_t *digits);
void am_ui_visible(bool visible);
void am_ui_refresh(const af_time *reference,bool trusted,bool include_reference,
                    const af_year *years,size_t year_count,bool night);
bool am_ui_editing(void);
bool am_ui_saving(void);
lv_obj_t *am_ui_root(void);
/* Stable names aid same-widget host input/accessibility inspection; no UART hook.
 * repeat names the inline segment group; mode(id) selects its actual buttons.
 * repeat_back returns NULL: the repeat subpage is no longer reachable in UI.
 * hour_error/minute_error expose adjacent feedback; retry is the list write retry. */
lv_obj_t *am_ui_control(const char *name,uint32_t id);
#endif
