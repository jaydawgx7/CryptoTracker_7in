#pragma once

#include "lvgl.h"
#include "models/app_state.h"

lv_obj_t *ui_settings_screen_create(void);
void ui_settings_set_state(app_state_t *state);
