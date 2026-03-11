#pragma once

#include "lvgl.h"

lv_obj_t *ui_alerts_screen_create(void);
void ui_alerts_refresh(void);
void ui_alerts_prepare_for_show(void);
void ui_alerts_show_toast(const char *text);
