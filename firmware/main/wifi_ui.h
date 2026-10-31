#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "ui_palette.h"
void wifi_ui_set_palette(const ui_palette_t *palette);
void wifi_ui_create(lv_obj_t *screen);
void wifi_ui_open(void);
/* One app-lifetime completion callback; called once, after closing, under the
 * existing LVGL lock. saved means this attempt's independent save confirmation. */
void wifi_ui_set_done(void (*done)(bool first,bool saved));
void wifi_ui_open_context(bool first);
void wifi_ui_refresh(void);
bool wifi_ui_private(void);
bool wifi_ui_visible(void);
bool wifi_ui_snapshot_allowed(void);
void wifi_ui_status(void);
#if CONFIG_CLOCK_DIAGNOSTICS
bool wifi_ui_command(char *line);
#endif
