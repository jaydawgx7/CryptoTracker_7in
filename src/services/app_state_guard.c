#include "services/app_state_guard.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "models/app_state.h"

static SemaphoreHandle_t s_app_state_mutex = NULL;

esp_err_t app_state_guard_init(void)
{
    if (s_app_state_mutex) {
        return ESP_OK;
    }

    s_app_state_mutex = xSemaphoreCreateMutex();
    if (!s_app_state_mutex) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool app_state_guard_lock(uint32_t timeout_ms)
{
    if (!s_app_state_mutex && app_state_guard_init() != ESP_OK) {
        return false;
    }

    TickType_t timeout_ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_app_state_mutex, timeout_ticks) == pdTRUE;
}

void app_state_guard_unlock(void)
{
    if (s_app_state_mutex) {
        xSemaphoreGive(s_app_state_mutex);
    }
}

esp_err_t app_state_guard_copy(app_state_t *dest, const app_state_t *src, uint32_t timeout_ms)
{
    if (!dest || !src) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!app_state_guard_lock(timeout_ms)) {
        return ESP_ERR_TIMEOUT;
    }

    memcpy(dest, src, sizeof(*dest));
    app_state_guard_unlock();
    return ESP_OK;
}