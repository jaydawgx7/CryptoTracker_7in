#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "models/app_state.h"

esp_err_t nvs_store_init(void);
esp_err_t nvs_store_load_app_state(app_state_t *state);
esp_err_t nvs_store_save_app_state(const app_state_t *state);
esp_err_t nvs_store_set_demo_portfolio(app_state_t *state, bool enabled);
esp_err_t nvs_store_load_coin_cache(char **json, int64_t *timestamp);
esp_err_t nvs_store_save_coin_cache(const char *json, int64_t timestamp);
