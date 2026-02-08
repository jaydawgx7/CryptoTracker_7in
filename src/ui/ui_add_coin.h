#pragma once

#include "lvgl.h"

#include "models/app_state.h"

lv_obj_t *ui_add_coin_screen_create(void);
void ui_add_coin_set_state(app_state_t *state);
void ui_add_coin_refresh(void);
