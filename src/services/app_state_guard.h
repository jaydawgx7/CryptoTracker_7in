#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct app_state_t app_state_t;

esp_err_t app_state_guard_init(void);
bool app_state_guard_lock(uint32_t timeout_ms);
void app_state_guard_unlock(void);
esp_err_t app_state_guard_copy(app_state_t *dest, const app_state_t *src, uint32_t timeout_ms);