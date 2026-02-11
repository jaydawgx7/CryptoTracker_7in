#pragma once

#include "lvgl.h"

#define UI_NAV_HEIGHT 56

typedef enum {
	UI_NAV_HOME = 0,
	UI_NAV_ADD,
	UI_NAV_ALERTS,
	UI_NAV_SETTINGS
} ui_nav_page_t;

void ui_nav_attach(lv_obj_t *screen, ui_nav_page_t active_page);
void ui_nav_attach_with_home_label(lv_obj_t *screen, ui_nav_page_t active_page, const char *home_label);
void ui_nav_attach_back_only(lv_obj_t *screen);
void ui_nav_set_alert_badge(bool visible);
