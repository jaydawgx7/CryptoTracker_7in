#pragma once

#include <stddef.h>

#include "lvgl.h"

#include "models/app_state.h"

lv_obj_t *ui_coin_detail_screen_create(void);
void ui_coin_detail_set_state(app_state_t *state);
void ui_coin_detail_show_index(size_t index);
void ui_coin_detail_refresh_if_active(void);
void ui_coin_detail_release_resources(void);
