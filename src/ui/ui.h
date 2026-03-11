#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"

#include "lvgl.h"

#include "models/app_state.h"

#include "services/wifi_manager.h"

void ui_init(void);
void ui_set_app_state(const app_state_t *state);
void ui_apply_theme(bool dark_mode);
void ui_show_add_coin(void);
void ui_show_alerts(void);
void ui_show_coin_detail(size_t index);
void ui_show_dashboard(void);
void ui_show_home(void);
void ui_show_settings(void);
void ui_show_touch_calibration(void);
void ui_request_home_header_update(wifi_state_t wifi_state, int rssi, uint32_t updated_age_s, bool offline, bool rate_limited);
void ui_request_home_refresh(void);
void ui_request_dashboard_refresh(void);
void ui_request_alerts_refresh(void);
void ui_request_coin_detail_refresh(void);
void ui_request_alert_toast(const char *text);
void ui_process_pending(void);
