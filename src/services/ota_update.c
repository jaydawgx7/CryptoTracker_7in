#include "services/ota_update.h"

#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "services/scheduler.h"

static const char *TAG = "ota_update";

#define OTA_URL_MAX_LEN 256
#define OTA_MIN_INTERVAL_MS 5000
#define OTA_HTTP_BUFFER_SIZE 8192

static SemaphoreHandle_t s_mutex = NULL;
static ota_status_t s_status = {0};
static int64_t s_last_start_ms = 0;
static TaskHandle_t s_task = NULL;

static void log_ota_url(const char *url)
{
    if (!url) {
        return;
    }

    char masked[128];
    const char *query = strchr(url, '?');
    size_t len = query ? (size_t)(query - url) : strlen(url);
    if (len >= sizeof(masked)) {
        len = sizeof(masked) - 1;
    }

    memcpy(masked, url, len);
    masked[len] = '\0';

    if (query && len + 4 < sizeof(masked)) {
        strcat(masked, "?...");
    }

    ESP_LOGI(TAG, "OTA URL: %s", masked);
}

static void set_status(ota_state_t state, int percent, int err, const char *message)
{
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_status.state = state;
        s_status.percent = percent;
        s_status.last_error = err;
        if (message) {
            strncpy(s_status.message, message, sizeof(s_status.message) - 1);
            s_status.message[sizeof(s_status.message) - 1] = '\0';
        }
        xSemaphoreGive(s_mutex);
    }
}

static void ota_task(void *arg)
{
    char url[OTA_URL_MAX_LEN];
    strncpy(url, (const char *)arg, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    free(arg);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (running) {
        ESP_LOGI(TAG, "Running partition: %s (0x%08X, 0x%X)", running->label, running->address,
                 running->size);
    }
    if (next) {
        ESP_LOGI(TAG, "Updating partition: %s (0x%08X, 0x%X)", next->label, next->address,
                 next->size);
    }

    scheduler_set_paused(true);
    vTaskDelay(pdMS_TO_TICKS(300));

    set_status(OTA_STATE_DOWNLOADING, 0, 0, "Starting");

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OTA_HTTP_BUFFER_SIZE,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
        .disable_auto_redirect = false
    };

    if (strstr(url, "github.com") || strstr(url, "githubusercontent.com")) {
        http_cfg.user_agent = "CryptoTracker_7in";
    }

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        set_status(OTA_STATE_FAILED, 0, err, "Begin failed");
        scheduler_set_paused(false);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        err = esp_https_ota_perform(ota_handle);
        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            int64_t read = esp_https_ota_get_image_len_read(ota_handle);
            int64_t total = esp_https_ota_get_image_size(ota_handle);
            int percent = 0;
            if (total > 0) {
                percent = (int)((read * 100) / total);
            }
            if (percent > 100) {
                percent = 100;
            }
            set_status(OTA_STATE_DOWNLOADING, percent, 0, "Downloading");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);
        set_status(OTA_STATE_FAILED, 0, err, "Download failed");
        scheduler_set_paused(false);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(err));
        set_status(OTA_STATE_FAILED, 0, err, "Finish failed");
        scheduler_set_paused(false);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    set_status(OTA_STATE_SUCCESS, 100, 0, "Rebooting");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

static bool url_is_valid(const char *url)
{
    if (!url) {
        return false;
    }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        return true;
    }
    return false;
}

esp_err_t ota_update_start(const char *url)
{
    if (!url || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (!url_is_valid(url)) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_start_ms > 0 && (now_ms - s_last_start_ms) < OTA_MIN_INTERVAL_MS) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }

    char *copy = calloc(1, OTA_URL_MAX_LEN);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    strncpy(copy, url, OTA_URL_MAX_LEN - 1);

    s_last_start_ms = now_ms;
    set_status(OTA_STATE_DOWNLOADING, 0, 0, "Queued");

    log_ota_url(url);

    BaseType_t ok = xTaskCreate(ota_task, "ota_update", 6144, copy, 5, &s_task);
    if (ok != pdPASS) {
        free(copy);
        s_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void ota_update_get_status(ota_status_t *out)
{
    if (!out) {
        return;
    }

    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        *out = s_status;
        xSemaphoreGive(s_mutex);
    }
}
