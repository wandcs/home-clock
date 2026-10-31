#pragma once
#include "sdkconfig.h"
#include "lvgl.h"
#include "ui_palette.h"
void location_ui_set_palette(const ui_palette_t *palette);
void location_ui_create(lv_obj_t *screen);
void location_ui_open(void);
void location_ui_set_done(void (*done)(bool first,bool saved));
void location_ui_open_context(bool first);
void location_ui_refresh(void);
bool location_ui_private(void);
bool location_ui_visible(void);
bool location_ui_snapshot_allowed(void);
void location_ui_status(void);
void location_ui_saved_label(lv_obj_t *label);
void location_ui_saved_context(lv_obj_t *label);
/* One saved snapshot, two inline identity labels and complete read-only detail.
 * Width is presentation only; persisted/source identity remains unchanged. */
void location_ui_saved_weather(lv_obj_t *name,lv_obj_t *context,lv_obj_t *details,int width);
#if CONFIG_CLOCK_DIAGNOSTICS
bool location_ui_command(const char *line);
#endif
