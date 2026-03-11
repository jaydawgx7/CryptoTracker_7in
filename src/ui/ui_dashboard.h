#pragma once

#include "lvgl.h"

#include "models/app_state.h"

lv_obj_t *ui_dashboard_screen_create(void);
void ui_dashboard_set_state(const app_state_t *state);
void ui_dashboard_refresh(void);
void ui_dashboard_prepare_for_show(void);
