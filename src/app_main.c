#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"

#include "models/app_state.h"
#include "services/alert_manager.h"
#include "services/control_mcu.h"
#include "services/coingecko_client.h"
#include "services/display_driver.h"
#include "services/fng_service.h"
#include "services/http_server.h"
#include "services/i2c_bus.h"
#include "services/nvs_store.h"
#include "services/scheduler.h"
#include "services/touch_driver.h"
#include "services/wifi_manager.h"
#include "ui/ui.h"
#include "ui/ui_alerts.h"

#ifndef CT_WIFI_ENABLE
#define CT_WIFI_ENABLE 1
#endif

#ifndef CT_UI_DISABLE
#define CT_UI_DISABLE 0
#endif

#ifndef CT_LVGL_TASK_ENABLE
#define CT_LVGL_TASK_ENABLE 1
#endif

static const char *TAG = "app_main";
static app_state_t s_app_state;

static void log_ota_partitions(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    if (running) {
        ESP_LOGI(TAG, "Running partition: %s (0x%08X, 0x%X)", running->label, running->address,
                 running->size);
    }
    if (next) {
        ESP_LOGI(TAG, "Next update partition: %s (0x%08X, 0x%X)", next->label, next->address,
                 next->size);
    }
}

static void ota_mark_valid_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(10000));
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA app marked valid");
    } else {
        ESP_LOGW(TAG, "OTA mark valid failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void alert_trigger_cb(const alert_log_t *entry)
{
    if (!entry) {
        return;
    }

    if (display_driver_lock(200)) {
        char msg[64];
        snprintf(msg, sizeof(msg), "%s %s", entry->symbol, entry->is_high ? "High" : "Low");
        ui_alerts_show_toast(msg);
        display_driver_unlock();
    }

    if (s_app_state.prefs.buzzer_enabled) {
        control_mcu_buzzer_beep(120);
    }
}

static void ui_init_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "UI init task start");
    ESP_ERROR_CHECK(touch_driver_init());
    ESP_LOGI(TAG, "Touch init done");
    if (!CT_UI_DISABLE) {
        ui_init();
        ESP_LOGI(TAG, "UI init done");
        ui_set_app_state(&s_app_state);
        ESP_LOGI(TAG, "UI state applied");
    } else {
        ESP_LOGW(TAG, "UI disabled by config");
    }

    if (CT_LVGL_TASK_ENABLE) {
        display_driver_start();
        ESP_LOGI(TAG, "LVGL task started");
    } else {
        ESP_LOGW(TAG, "LVGL task disabled by config");
    }

    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_ERROR);

    log_ota_partitions();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(nvs_store_init());
    ESP_ERROR_CHECK(control_mcu_init());
    vTaskDelay(pdMS_TO_TICKS(200));

    uint8_t i2c_addrs[16] = {0};
    size_t i2c_count = i2c_bus_scan(i2c_addrs, sizeof(i2c_addrs));
    ESP_LOGI(TAG, "I2C devices found: %u", (unsigned)i2c_count);
    for (size_t i = 0; i < i2c_count && i < sizeof(i2c_addrs); i++) {
        ESP_LOGI(TAG, "I2C device: 0x%02X", i2c_addrs[i]);
    }

    control_mcu_force_backlight();
    control_mcu_touch_enable();

    if (nvs_store_load_app_state(&s_app_state) == ESP_OK) {
        control_mcu_set_brightness(s_app_state.prefs.brightness);
    } else {
        control_mcu_set_brightness(60);
    }

    ESP_ERROR_CHECK(display_driver_init());

    if (CT_WIFI_ENABLE) {
        ESP_ERROR_CHECK(wifi_manager_init());
        ESP_ERROR_CHECK(coingecko_client_init());
        ESP_ERROR_CHECK(fng_service_init());
        ESP_ERROR_CHECK(http_server_init());
        http_server_set_state(&s_app_state);
    } else {
        ESP_LOGW(TAG, "Wi-Fi disabled by config");
    }

    BaseType_t task_ok = xTaskCreate(ui_init_task, "ui_init", 24576, NULL, 5, NULL);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to start ui_init task");
    }

    ESP_ERROR_CHECK(alert_manager_init());
    alert_manager_set_state(&s_app_state);
    alert_manager_set_callback(alert_trigger_cb);
    ESP_ERROR_CHECK(scheduler_init(&s_app_state));

    BaseType_t mark_ok = xTaskCreate(ota_mark_valid_task, "ota_mark_valid", 3072, NULL, 3, NULL);
    if (mark_ok != pdPASS) {
        ESP_LOGW(TAG, "Failed to start OTA mark valid task");
    }

    ESP_LOGI(TAG, "CryptoTracker initialized");
}