#include "services/scheduler.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>

#include "services/coingecko_client.h"
#include "services/display_driver.h"
#include "services/fng_service.h"
#include "services/kraken_client.h"
#include "services/wifi_manager.h"
#include "services/alert_manager.h"
#include "ui/ui_dashboard.h"
#include "ui/ui_home.h"
#include "ui/ui_alerts.h"

static const char *TAG = "scheduler";

#ifndef CT_SPARKLINE_ENABLE
#define CT_SPARKLINE_ENABLE 0
#endif

static app_state_t *s_state = NULL;
static TaskHandle_t s_task = NULL;
#if CT_SPARKLINE_ENABLE
static size_t s_chart_index = 0;
static int64_t s_last_chart_fetch_s = 0;
#endif
static int64_t s_last_market_fetch_s = 0;
static int64_t s_last_coingecko_fetch_s = 0;
static int64_t s_last_dashboard_metrics_fetch_s = 0;
static int64_t s_rate_limit_until_s = 0;
static int64_t s_last_ui_refresh_s = 0;
static int64_t s_last_header_update_s = 0;
static bool s_paused = false;
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
        double last_price = s_last_prices[i];
        double price_diff = fabs(coin->price - last_price);
        double price_threshold = fmax(1e-12, fabs(last_price) * 0.000001);
        if (price_diff > price_threshold ||
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

static uint32_t clamp_refresh_seconds(uint32_t value, data_source_t source)
{
    (void)source;
    uint32_t min = 5;
    uint32_t max = 120;

    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static void refresh_task(void *arg)
{
    (void)arg;
    uint32_t failures = 0;
    int64_t last_success = 0;

    while (true) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (!s_state || s_state->watchlist_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        data_source_t source = s_state->prefs.data_source;
        uint32_t user_pref = clamp_refresh_seconds(s_state->prefs.refresh_seconds, source);
        uint32_t base = user_pref;
        uint32_t delay_sec = base;

        wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
        wifi_manager_get_state(&wifi_state, NULL);
        if (wifi_state != WIFI_STATE_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        char ip[16] = {0};
        if (!wifi_manager_get_ip(ip, sizeof(ip))) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        int64_t now_s = esp_timer_get_time() / 1000000;
        bool rate_limited_active = (s_rate_limit_until_s > 0 && now_s < s_rate_limit_until_s);
        bool fetched = false;

        bool metrics_due = (s_last_dashboard_metrics_fetch_s == 0) ||
                   ((now_s - s_last_dashboard_metrics_fetch_s) >= 3600);
        if (metrics_due) {
            (void)fng_service_refresh(3600);
            s_last_dashboard_metrics_fetch_s = now_s;
            if (display_driver_lock(200)) {
                ui_dashboard_refresh();
                display_driver_unlock();
            }
        }

        if (s_state->watchlist_count == 0) {
            for (uint32_t i = 0; i < delay_sec; i++) {
                int64_t now = esp_timer_get_time();
                int64_t now_loop_s = now / 1000000;
                uint32_t age_s = last_success > 0 ? (uint32_t)((now - last_success) / 1000000) : 0;
                bool rate_limited = (s_rate_limit_until_s > 0 && now < (s_rate_limit_until_s * 1000000LL));
                bool offline = failures > 0 && !rate_limited;
                wifi_state_t wifi_loop_state = WIFI_STATE_DISCONNECTED;
                int rssi = 0;
                wifi_manager_get_state(&wifi_loop_state, &rssi);

                if (now_loop_s - s_last_header_update_s >= 2) {
                    if (display_driver_lock(100)) {
                        ui_home_update_header(wifi_loop_state, rssi, age_s, offline, rate_limited);
                        display_driver_unlock();
                    }
                    s_last_header_update_s = now_loop_s;
                }

                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            continue;
        }

        if (source == DATA_SOURCE_KRAKEN) {
            bool any_missing = false;
            esp_err_t err = ESP_OK;
            bool kraken_ok = false;

            if (s_last_market_fetch_s == 0 || (now_s - s_last_market_fetch_s) >= (int64_t)base) {
                err = kraken_client_fetch_prices(s_state, &any_missing);
                if (err == ESP_OK) {
                    failures = 0;
                    last_success = esp_timer_get_time();
                    s_last_market_fetch_s = now_s;
                    fetched = true;
                    kraken_ok = true;
                } else {
                    failures++;
                    ESP_LOGW(TAG, "Kraken refresh failed: %d", err);
                }
            }

            if (kraken_ok) {
                if (s_last_coingecko_fetch_s == 0 || (now_s - s_last_coingecko_fetch_s) >= 300) {
                    esp_err_t cg_err = coingecko_client_fetch_markets_mode(s_state, false);
                    if (cg_err == ESP_OK) {
                        s_last_coingecko_fetch_s = now_s;
                        fetched = true;
                    } else {
                        ESP_LOGW(TAG, "CoinGecko percent sync failed: %d", cg_err);
                    }
                }
            }

            if ((any_missing || err != ESP_OK) && !rate_limited_active) {
                if (s_last_coingecko_fetch_s == 0 || (now_s - s_last_coingecko_fetch_s) >= 60) {
                    esp_err_t cg_err = coingecko_client_fetch_markets_mode(s_state, true);
                    if (cg_err == ESP_OK) {
                        s_last_coingecko_fetch_s = now_s;
                        s_rate_limit_until_s = 0;
                        rate_limited_active = false;
                        fetched = true;
                    } else {
                        if (cg_err == ESP_ERR_TIMEOUT) {
                            s_rate_limit_until_s = now_s + 120;
                            rate_limited_active = true;
                            ESP_LOGW(TAG, "Rate limited: backing off for 120s");
                        }
                        ESP_LOGW(TAG, "CoinGecko fallback failed: %d", cg_err);
                    }
                }
            }
        } else {
            uint32_t min_interval = base;
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
                    s_last_coingecko_fetch_s = now_s;
                    s_rate_limit_until_s = 0;
                    rate_limited_active = false;
                    fetched = true;
                } else {
                    failures++;
                    if (err == ESP_ERR_TIMEOUT) {
                        if (failures < 3) {
                            failures = 3;
                        }
                        s_rate_limit_until_s = now_s + 120;
                        rate_limited_active = true;
                        ESP_LOGW(TAG, "Rate limited: backing off for 120s");
                    }
                    ESP_LOGW(TAG, "Market refresh failed: %d", err);
                }
            }
        }

        if (fetched) {
            bool changed = market_data_changed(s_state);
            if (changed && (now_s - s_last_ui_refresh_s) >= 5) {
                if (display_driver_lock(200)) {
                    ui_home_refresh();
                    ui_dashboard_refresh();
                    display_driver_unlock();
                }
                s_last_ui_refresh_s = now_s;
            }
            alert_manager_check();
            if (display_driver_lock(200)) {
                ui_alerts_refresh();
                ui_dashboard_refresh();
                display_driver_unlock();
            }
        }

#if CT_SPARKLINE_ENABLE
        if (source != DATA_SOURCE_KRAKEN) {
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
        }
#endif

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
    s_last_dashboard_metrics_fetch_s = 0;
    if (!s_task) {
        xTaskCreate(refresh_task, "ct_refresh", 6144, NULL, 3, &s_task);
    }
    ESP_LOGI(TAG, "Scheduler started");
    return ESP_OK;
}

void scheduler_set_paused(bool paused)
{
    s_paused = paused;
    ESP_LOGI(TAG, "Scheduler %s", paused ? "paused" : "resumed");
}
