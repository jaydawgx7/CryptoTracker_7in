#include "services/scheduler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>

#include "services/coingecko_client.h"
#include "services/display_driver.h"
#include "services/wifi_manager.h"
#include "services/alert_manager.h"
#include "ui/ui_home.h"
#include "ui/ui_alerts.h"

static const char *TAG = "scheduler";

#ifndef CT_SPARKLINE_ENABLE
#define CT_SPARKLINE_ENABLE 0
#endif

static app_state_t *s_state = NULL;
static TaskHandle_t s_task = NULL;
static size_t s_chart_index = 0;
static int64_t s_last_chart_fetch_s = 0;
static int64_t s_last_market_fetch_s = 0;
static int64_t s_rate_limit_until_s = 0;
static int64_t s_last_ui_refresh_s = 0;
static int64_t s_last_header_update_s = 0;
static uint32_t s_min_interval_override_s = 0;
static size_t s_last_watchlist_count = 0;
static double s_last_prices[MAX_WATCHLIST] = {0};
static double s_last_change_1h[MAX_WATCHLIST] = {0};
static double s_last_change_24h[MAX_WATCHLIST] = {0};
static double s_last_change_7d[MAX_WATCHLIST] = {0};

static bool market_data_changed(const app_state_t *state)
{
    if (!state) {
        return false;
    }

    bool changed = false;
    size_t count = state->watchlist_count;
    if (count != s_last_watchlist_count) {
        changed = true;
        s_last_watchlist_count = count;
    }

    size_t max = count < MAX_WATCHLIST ? count : MAX_WATCHLIST;
    for (size_t i = 0; i < max; i++) {
        const coin_t *coin = &state->watchlist[i];
        if (fabs(coin->price - s_last_prices[i]) > 0.0000001 ||
            fabs(coin->change_1h - s_last_change_1h[i]) > 0.0001 ||
            fabs(coin->change_24h - s_last_change_24h[i]) > 0.0001 ||
            fabs(coin->change_7d - s_last_change_7d[i]) > 0.0001) {
            changed = true;
        }

        s_last_prices[i] = coin->price;
        s_last_change_1h[i] = coin->change_1h;
        s_last_change_24h[i] = coin->change_24h;
        s_last_change_7d[i] = coin->change_7d;
    }

    return changed;
}

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

        wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
        wifi_manager_get_state(&wifi_state, NULL);
        if (wifi_state != WIFI_STATE_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int64_t now_s = esp_timer_get_time() / 1000000;
        bool rate_limited_active = (s_rate_limit_until_s > 0 && now_s < s_rate_limit_until_s);

        uint32_t min_interval = base < 20 ? 20 : base;
        if (s_min_interval_override_s > min_interval) {
            min_interval = s_min_interval_override_s;
        }
        bool can_fetch = !rate_limited_active;
        if (s_last_market_fetch_s > 0 && (now_s - s_last_market_fetch_s) < (int64_t)min_interval) {
            can_fetch = false;
        }

        if (can_fetch) {
            esp_err_t err = coingecko_client_fetch_markets(s_state);
            if (err == ESP_OK) {
                failures = 0;
                last_success = esp_timer_get_time();
                s_last_market_fetch_s = now_s;
                s_rate_limit_until_s = 0;
                rate_limited_active = false;
                if (s_min_interval_override_s > 0) {
                    if (s_min_interval_override_s > 25) {
                        s_min_interval_override_s -= 5;
                    } else {
                        s_min_interval_override_s = 0;
                    }
                }
                ESP_LOGI(TAG, "Market refresh ok (%u coins)", (unsigned)s_state->watchlist_count);
                if (market_data_changed(s_state)) {
                    if ((now_s - s_last_ui_refresh_s) >= 5) {
                        if (display_driver_lock(200)) {
                            ui_home_refresh();
                            display_driver_unlock();
                        }
                        s_last_ui_refresh_s = now_s;
                    }
                }
                alert_manager_check();
                if (display_driver_lock(200)) {
                    ui_alerts_refresh();
                    display_driver_unlock();
                }

#if CT_SPARKLINE_ENABLE
            int64_t now_s = esp_timer_get_time() / 1000000;
            if (s_state->watchlist_count > 0 && (now_s - s_last_chart_fetch_s) >= 20) {
                size_t idx = s_chart_index % s_state->watchlist_count;
                const chart_point_t *points = NULL;
                size_t count = 0;
                if (coingecko_client_get_chart(s_state->watchlist[idx].id, 1, &points, &count) == ESP_OK) {
                    if ((now_s - s_last_ui_refresh_s) >= 5) {
                        if (display_driver_lock(200)) {
                            ui_home_refresh();
                            display_driver_unlock();
                        }
                        s_last_ui_refresh_s = now_s;
                    }
                }
                s_chart_index = (idx + 1) % s_state->watchlist_count;
                s_last_chart_fetch_s = now_s;
            }
#endif
            } else {
                failures++;
                if (err == ESP_ERR_TIMEOUT) {
                    if (failures < 3) {
                        failures = 3;
                    }
                    s_rate_limit_until_s = now_s + 120;
                    rate_limited_active = true;
                    if (s_min_interval_override_s == 0) {
                        s_min_interval_override_s = 30;
                    } else if (s_min_interval_override_s < 60) {
                        s_min_interval_override_s = s_min_interval_override_s * 2;
                        if (s_min_interval_override_s > 60) {
                            s_min_interval_override_s = 60;
                        }
                    }
                    ESP_LOGW(TAG, "Rate limited: backing off for 120s");
                }
                ESP_LOGW(TAG, "Market refresh failed: %d", err);
            }
        }

        for (uint32_t i = 0; i < delay_sec; i++) {
            int64_t now = esp_timer_get_time();
            int64_t now_s = now / 1000000;
            uint32_t age_s = last_success > 0 ? (uint32_t)((now - last_success) / 1000000) : 0;
            bool rate_limited = (s_rate_limit_until_s > 0 && now < (s_rate_limit_until_s * 1000000LL));
            bool offline = failures > 0 && !rate_limited;
            wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
            int rssi = 0;
            wifi_manager_get_state(&wifi_state, &rssi);

            if (now_s - s_last_header_update_s >= 2) {
                if (display_driver_lock(100)) {
                    ui_home_update_header(wifi_state, rssi, age_s, offline, rate_limited);
                    display_driver_unlock();
                }
                s_last_header_update_s = now_s;
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
        xTaskCreate(refresh_task, "ct_refresh", 6144, NULL, 3, &s_task);
    }
    ESP_LOGI(TAG, "Scheduler started");
    return ESP_OK;
}
