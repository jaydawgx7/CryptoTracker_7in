#include "services/scheduler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "services/coingecko_client.h"
#include "services/display_driver.h"
#include "services/wifi_manager.h"
#include "services/alert_manager.h"
#include "ui/ui_home.h"
#include "ui/ui_alerts.h"

static const char *TAG = "scheduler";

static app_state_t *s_state = NULL;
static TaskHandle_t s_task = NULL;
static size_t s_chart_index = 0;
static int64_t s_last_chart_fetch_s = 0;

static uint32_t adaptive_base_seconds(size_t count)
{
    if (count <= 10) {
        return 10;
    }
    if (count <= 30) {
        return 15;
    }
    return 30;
}

static uint32_t clamp_refresh_seconds(uint32_t value)
{
    if (value < 5) {
        return 5;
    }
    if (value > 60) {
        return 60;
    }
    return value;
}

static void refresh_task(void *arg)
{
    (void)arg;
    uint32_t failures = 0;
    int64_t last_success = 0;

    while (true) {
        if (!s_state || s_state->watchlist_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t user_pref = clamp_refresh_seconds(s_state->prefs.refresh_seconds);
        uint32_t adaptive = adaptive_base_seconds(s_state->watchlist_count);
        uint32_t base = user_pref > adaptive ? user_pref : adaptive;

        uint32_t delay_sec = base;
        if (failures > 0) {
            uint32_t backoff = base << (failures > 5 ? 5 : failures);
            if (backoff > 300) {
                backoff = 300;
            }
            delay_sec = backoff;
        }

        esp_err_t err = coingecko_client_fetch_markets(s_state);
        if (err == ESP_OK) {
            failures = 0;
            last_success = esp_timer_get_time();
            if (display_driver_lock(200)) {
                ui_home_refresh();
                display_driver_unlock();
            }
            alert_manager_check();
            if (display_driver_lock(200)) {
                ui_alerts_refresh();
                display_driver_unlock();
            }

            int64_t now_s = esp_timer_get_time() / 1000000;
            if (s_state->watchlist_count > 0 && (now_s - s_last_chart_fetch_s) >= 20) {
                size_t idx = s_chart_index % s_state->watchlist_count;
                const chart_point_t *points = NULL;
                size_t count = 0;
                if (coingecko_client_get_chart(s_state->watchlist[idx].id, 1, &points, &count) == ESP_OK) {
                    if (display_driver_lock(200)) {
                        ui_home_refresh();
                        display_driver_unlock();
                    }
                }
                s_chart_index = (idx + 1) % s_state->watchlist_count;
                s_last_chart_fetch_s = now_s;
            }
        } else {
            failures++;
            ESP_LOGW(TAG, "Market refresh failed: %d", err);
        }

        for (uint32_t i = 0; i < delay_sec; i++) {
            int64_t now = esp_timer_get_time();
            uint32_t age_s = last_success > 0 ? (uint32_t)((now - last_success) / 1000000) : 0;
            bool offline = failures > 0;
            wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
            int rssi = 0;
            wifi_manager_get_state(&wifi_state, &rssi);

            if (display_driver_lock(100)) {
                ui_home_update_header(wifi_state, rssi, age_s, offline);
                display_driver_unlock();
            }

            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t scheduler_init(app_state_t *state)
{
#if !CT_SCHEDULER_ENABLE
    (void)state;
    ESP_LOGW(TAG, "Scheduler disabled by config");
    return ESP_OK;
#endif
    s_state = state;
    if (!s_task) {
        xTaskCreate(refresh_task, "ct_refresh", 6144, NULL, 5, &s_task);
    }
    ESP_LOGI(TAG, "Scheduler started");
    return ESP_OK;
}
