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
void ui_nav_set_alert_badge(bool visible);
