#pragma once

#include "esp_err.h"

#include "models/app_state.h"

esp_err_t http_server_init(void);
void http_server_deinit(void);
void http_server_set_state(app_state_t *state);
