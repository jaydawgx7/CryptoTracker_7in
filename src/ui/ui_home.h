#pragma once

#include "lvgl.h"

#include "models/app_state.h"
#include "services/wifi_manager.h"

lv_obj_t *ui_home_screen_create(void);
void ui_home_set_state(const app_state_t *state);
void ui_home_refresh(void);
void ui_home_update_header(wifi_state_t wifi_state, int rssi, uint32_t updated_age_s, bool offline);
