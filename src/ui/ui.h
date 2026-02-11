#pragma once

#include <stddef.h>

#include "lvgl.h"

#include "models/app_state.h"

void ui_init(void);
void ui_set_app_state(const app_state_t *state);
void ui_apply_theme(bool dark_mode);
void ui_show_add_coin(void);
void ui_show_alerts(void);
void ui_show_coin_detail(size_t index);
void ui_show_home(void);
void ui_show_settings(void);
