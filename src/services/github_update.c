#include "services/github_update.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "app_version.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "github_update";

#define GITHUB_RELEASE_URL "https://api.github.com/repos/jaydawgx7/CryptoTracker_7in/releases/latest"
#define GITHUB_BODY_MAX 8192
#define GITHUB_TAG_MAX 31
#define GITHUB_NOTES_MAX 200
#define GITHUB_CHECK_MIN_MS 5000

static SemaphoreHandle_t s_mutex = NULL;
static github_update_status_t s_status = {0};
static TaskHandle_t s_task = NULL;
static int64_t s_last_check_ms = 0;

static void set_status(github_update_state_t state)
{
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_status.state = state;
        xSemaphoreGive(s_mutex);
    }
}

static void set_status_fields(const char *tag, const char *url, const char *notes, int http, int err)
{
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        if (tag) {
            strncpy(s_status.latest_tag, tag, sizeof(s_status.latest_tag) - 1);
            s_status.latest_tag[sizeof(s_status.latest_tag) - 1] = '\0';
        }
        if (url) {
            strncpy(s_status.download_url, url, sizeof(s_status.download_url) - 1);
            s_status.download_url[sizeof(s_status.download_url) - 1] = '\0';
        }
        if (notes) {
            strncpy(s_status.notes, notes, sizeof(s_status.notes) - 1);
            s_status.notes[sizeof(s_status.notes) - 1] = '\0';
        }
        s_status.last_http = http;
        s_status.last_error = err;
        s_status.last_checked_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_mutex);
    }
}

static void clear_status_fields(void)
{
    if (!s_mutex) {
        return;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        s_status.latest_tag[0] = '\0';
        s_status.download_url[0] = '\0';
        s_status.notes[0] = '\0';
        s_status.last_http = 0;
        s_status.last_error = 0;
        s_status.last_checked_ms = esp_timer_get_time() / 1000;
        xSemaphoreGive(s_mutex);
    }
}

static bool parse_semver(const char *tag, int *major, int *minor, int *patch)
{
    if (!tag || !major || !minor || !patch) {
        return false;
    }

    const char *p = tag;
    if (*p == 'v' || *p == 'V') {
        p++;
    }

    char *end = NULL;
    long m1 = strtol(p, &end, 10);
    if (!end || *end != '.') {
        return false;
    }
    p = end + 1;

    long m2 = strtol(p, &end, 10);
    if (!end || *end != '.') {
        return false;
    }
    p = end + 1;

    long m3 = strtol(p, &end, 10);
    if (!end || (*end != '\0' && *end != '\n' && *end != '\r')) {
        return false;
    }

    *major = (int)m1;
    *minor = (int)m2;
    *patch = (int)m3;
    return true;
}

int semver_compare(const char *a, const char *b, bool *ok)
{
    int a1 = 0;
    int a2 = 0;
    int a3 = 0;
    int b1 = 0;
    int b2 = 0;
    int b3 = 0;

    bool parsed_a = parse_semver(a, &a1, &a2, &a3);
    bool parsed_b = parse_semver(b, &b1, &b2, &b3);

    if (ok) {
        *ok = (parsed_a && parsed_b);
    }

    if (!parsed_a || !parsed_b) {
        return 0;
    }

    if (a1 != b1) {
        return (a1 > b1) ? 1 : -1;
    }
    if (a2 != b2) {
        return (a2 > b2) ? 1 : -1;
    }
    if (a3 != b3) {
        return (a3 > b3) ? 1 : -1;
    }
    return 0;
}

static bool contains_rate_limit(const char *body)
{
    if (!body) {
        return false;
    }

    for (const char *p = body; *p; p++) {
        if ((*p == 'r' || *p == 'R') && strncasecmp(p, "rate limit", 10) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_asset_match(const cJSON *asset)
{
    const cJSON *name = cJSON_GetObjectItem(asset, "name");
    const cJSON *content = cJSON_GetObjectItem(asset, "content_type");

    if (cJSON_IsString(name)) {
        const char *value = name->valuestring;
        size_t len = strlen(value);
        if (len >= 4 && strcasecmp(value + len - 4, ".bin") == 0) {
            return true;
        }
    }

    if (cJSON_IsString(content)) {
        if (strcmp(content->valuestring, "application/octet-stream") == 0) {
            return true;
        }
    }

    return false;
}

static esp_err_t read_body(esp_http_client_handle_t client, char *out, size_t max_len)
{
    size_t total = 0;
    while (total < max_len - 1) {
        int read = esp_http_client_read(client, out + total, (int)(max_len - 1 - total));
        if (read <= 0) {
            break;
        }
        total += (size_t)read;
    }
    out[total] = '\0';
    return ESP_OK;
}

static void github_check_task(void *arg)
{
    (void)arg;

    set_status(GITHUB_UPDATE_CHECKING);
    clear_status_fields();

    esp_http_client_config_t cfg = {
        .url = GITHUB_RELEASE_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        set_status_fields(NULL, NULL, NULL, 0, ESP_ERR_NO_MEM);
        set_status(GITHUB_UPDATE_FAILED);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_set_header(client, "User-Agent", "CryptoTracker_7in");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");

    esp_err_t err = esp_http_client_open(client, 0);
    int http_status = esp_http_client_get_status_code(client);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
    }

    char *body = malloc(GITHUB_BODY_MAX);
    if (!body) {
        esp_http_client_cleanup(client);
        set_status_fields(NULL, NULL, NULL, http_status, ESP_ERR_NO_MEM);
        set_status(GITHUB_UPDATE_FAILED);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (err == ESP_OK) {
        read_body(client, body, GITHUB_BODY_MAX);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GitHub check failed: %s", esp_err_to_name(err));
        set_status_fields(NULL, NULL, NULL, http_status, err);
        set_status(GITHUB_UPDATE_FAILED);
        free(body);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "GitHub check HTTP status: %d", http_status);

    if (http_status == 403 && contains_rate_limit(body)) {
        set_status_fields(NULL, NULL, NULL, http_status, ESP_OK);
        set_status(GITHUB_UPDATE_RATE_LIMITED);
        free(body);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    if (http_status != 200) {
        set_status_fields(NULL, NULL, NULL, http_status, ESP_FAIL);
        set_status(GITHUB_UPDATE_FAILED);
        free(body);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        set_status_fields(NULL, NULL, NULL, http_status, ESP_FAIL);
        set_status(GITHUB_UPDATE_FAILED);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    const cJSON *assets = cJSON_GetObjectItem(root, "assets");
    const cJSON *notes = cJSON_GetObjectItem(root, "body");

    const char *tag_value = cJSON_IsString(tag) ? tag->valuestring : NULL;
    char notes_buf[GITHUB_NOTES_MAX + 1] = {0};

    if (cJSON_IsString(notes)) {
        size_t nlen = strlen(notes->valuestring);
        if (nlen > GITHUB_NOTES_MAX) {
            nlen = GITHUB_NOTES_MAX;
        }
        memcpy(notes_buf, notes->valuestring, nlen);
        notes_buf[nlen] = '\0';
    }

    char url_buf[256] = {0};
    if (cJSON_IsArray(assets)) {
        const cJSON *asset = NULL;
        cJSON_ArrayForEach(asset, assets) {
            if (!cJSON_IsObject(asset)) {
                continue;
            }
            if (!is_asset_match(asset)) {
                continue;
            }
            const cJSON *url = cJSON_GetObjectItem(asset, "browser_download_url");
            if (cJSON_IsString(url)) {
                strncpy(url_buf, url->valuestring, sizeof(url_buf) - 1);
                break;
            }
        }
    }

    if (!tag_value || tag_value[0] == '\0') {
        set_status_fields(NULL, NULL, notes_buf, http_status, ESP_FAIL);
        set_status(GITHUB_UPDATE_FAILED);
        cJSON_Delete(root);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "GitHub latest tag: %s", tag_value);

    bool parsed = false;
    int cmp = semver_compare(tag_value, APP_VERSION, &parsed);
    bool newer = parsed ? (cmp > 0) : (strcmp(tag_value, APP_VERSION) != 0);

    set_status_fields(tag_value, url_buf, notes_buf, http_status, ESP_OK);

    if (newer && url_buf[0] != '\0') {
        set_status(GITHUB_UPDATE_AVAILABLE);
    } else if (newer) {
        set_status(GITHUB_UPDATE_FAILED);
    } else {
        set_status(GITHUB_UPDATE_UP_TO_DATE);
    }

    cJSON_Delete(root);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t github_update_start_check(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_check_ms > 0 && (now_ms - s_last_check_ms) < GITHUB_CHECK_MIN_MS) {
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

    s_last_check_ms = now_ms;

    BaseType_t ok = xTaskCreate(github_check_task, "github_check", 6144, NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void github_update_get_status(github_update_status_t *out)
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
