#include "services/http_server.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/app_state_guard.h"
#include "services/display_driver.h"
#include "services/screenshot.h"
#include "services/nvs_store.h"
#include "services/ota_update.h"
#include "services/wifi_manager.h"

static const char *TAG = "http_server";

#define SCREENSHOT_RATE_LIMIT_MS 2000
#define SCREENSHOT_CHUNK_SIZE 4096

static httpd_handle_t s_server = NULL;
static TaskHandle_t s_task = NULL;
static int64_t s_last_shot_ms = 0;
static app_state_t *s_state = NULL;

#define WATCHLIST_MAX_BODY 32768

static void write_le16(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
}

static void write_le32(uint8_t *buf, uint32_t value)
{
    buf[0] = (uint8_t)(value & 0xFF);
    buf[1] = (uint8_t)((value >> 8) & 0xFF);
    buf[2] = (uint8_t)((value >> 16) & 0xFF);
    buf[3] = (uint8_t)((value >> 24) & 0xFF);
}

static esp_err_t handle_root(httpd_req_t *req)
{
    static const char *html =
        "<!doctype html>"
        "<html><head><meta charset=\"utf-8\">"
        "<title>CryptoTracker Screenshot</title>"
        "<style>body{font-family:sans-serif;background:#111;color:#ddd;padding:16px}"
        "img{max-width:100%;height:auto;border:1px solid #333}</style>"
        "</head><body>"
        "<h2>CryptoTracker</h2>"
        "<p><button onclick=\"refreshShot()\">Refresh</button></p>"
        "<img id=\"shot\" src=\"/screenshot.bmp?ts=0\" alt=\"screenshot\">"
        "<p><a href=\"/watchlist.json\">Download watchlist JSON</a></p>"
        "<form method=\"post\" action=\"/watchlist.json\" enctype=\"application/json\">"
        "<textarea name=\"payload\" rows=\"6\" cols=\"40\" placeholder=\"Paste watchlist JSON array here\"></textarea><br>"
        "<button type=\"submit\">Upload watchlist</button>"
        "</form>"
        "<script>function refreshShot(){document.getElementById('shot').src='/screenshot.bmp?ts='+Date.now();}</script>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static cJSON *watchlist_to_json(const app_state_t *state)
{
    cJSON *array = cJSON_CreateArray();
    if (!array || !state) {
        return array;
    }

    for (size_t i = 0; i < state->watchlist_count; i++) {
        const coin_t *coin = &state->watchlist[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "id", coin->id);
        cJSON_AddStringToObject(item, "symbol", coin->symbol);
        cJSON_AddStringToObject(item, "name", coin->name);
        cJSON_AddNumberToObject(item, "holdings", coin->holdings);
        cJSON_AddNumberToObject(item, "alert_low", coin->alert_low);
        cJSON_AddNumberToObject(item, "alert_high", coin->alert_high);
        cJSON_AddBoolToObject(item, "pinned", coin->pinned);
        cJSON_AddItemToArray(array, item);
    }

    return array;
}

static bool json_to_watchlist(app_state_t *state, const cJSON *array)
{
    if (!state || !cJSON_IsArray(array)) {
        return false;
    }

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (count >= MAX_WATCHLIST) {
            break;
        }

        coin_t *coin = &state->watchlist[count];
        memset(coin, 0, sizeof(*coin));

        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *symbol = cJSON_GetObjectItem(item, "symbol");
        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *holdings = cJSON_GetObjectItem(item, "holdings");
        cJSON *alert_low = cJSON_GetObjectItem(item, "alert_low");
        cJSON *alert_high = cJSON_GetObjectItem(item, "alert_high");
        cJSON *pinned = cJSON_GetObjectItem(item, "pinned");

        if (cJSON_IsString(id)) {
            strncpy(coin->id, id->valuestring, sizeof(coin->id) - 1);
        }
        if (cJSON_IsString(symbol)) {
            strncpy(coin->symbol, symbol->valuestring, sizeof(coin->symbol) - 1);
        }
        if (cJSON_IsString(name)) {
            strncpy(coin->name, name->valuestring, sizeof(coin->name) - 1);
        }
        if (cJSON_IsNumber(holdings)) {
            coin->holdings = holdings->valuedouble;
        }
        if (cJSON_IsNumber(alert_low)) {
            coin->alert_low = alert_low->valuedouble;
        }
        if (cJSON_IsNumber(alert_high)) {
            coin->alert_high = alert_high->valuedouble;
        }
        if (cJSON_IsBool(pinned)) {
            coin->pinned = cJSON_IsTrue(pinned);
        }

        if (coin->id[0] == '\0' || coin->symbol[0] == '\0' || coin->name[0] == '\0') {
            continue;
        }

        count++;
    }

    state->watchlist_count = count;
    return count > 0;
}

static esp_err_t handle_watchlist_get(httpd_req_t *req)
{
    if (!s_state) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "State unavailable", HTTPD_RESP_USE_STRLEN);
    }

    if (!app_state_guard_lock(250)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "State busy", HTTPD_RESP_USE_STRLEN);
    }

    cJSON *array = watchlist_to_json(s_state);
    app_state_guard_unlock();
    if (!array) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
    }

    char *json = cJSON_PrintUnformatted(array);
    cJSON_Delete(array);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t handle_watchlist_post(httpd_req_t *req)
{
    if (!s_state) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "State unavailable", HTTPD_RESP_USE_STRLEN);
    }

    size_t len = req->content_len;
    if (len == 0 || len > WATCHLIST_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, "Payload too large", HTTPD_RESP_USE_STRLEN);
    }

    char *body = malloc(len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, body + received, len - received);
        if (ret <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        }
        received += (size_t)ret;
    }
    body[len] = '\0';

    const char *payload = body;
    if (strncmp(body, "payload=", 8) == 0) {
        payload = body + 8;
    }

    cJSON *array = cJSON_Parse(payload);
    if (!array) {
        free(body);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    if (!app_state_guard_lock(250)) {
        cJSON_Delete(array);
        free(body);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "State busy", HTTPD_RESP_USE_STRLEN);
    }

    bool ok = json_to_watchlist(s_state, array);
    cJSON_Delete(array);
    free(body);
    if (!ok) {
        app_state_guard_unlock();
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid watchlist");
    }

    s_state->needs_restore = false;

    esp_err_t save_err = nvs_store_save_app_state(s_state);
    app_state_guard_unlock();
    if (save_err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
    }

    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_ota_post(httpd_req_t *req)
{
    size_t len = req->content_len;
    if (len == 0 || len > WATCHLIST_MAX_BODY) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_send(req, "Payload too large", HTTPD_RESP_USE_STRLEN);
    }

    char *body = malloc(len + 1);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    }

    size_t received = 0;
    while (received < len) {
        int ret = httpd_req_recv(req, body + received, len - received);
        if (ret <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        }
        received += (size_t)ret;
    }
    body[len] = '\0';

    cJSON *obj = cJSON_Parse(body);
    free(body);
    if (!obj) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    cJSON *url = cJSON_GetObjectItem(obj, "url");
    if (!cJSON_IsString(url)) {
        cJSON_Delete(obj);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing url");
    }

    esp_err_t err = ota_update_start(url->valuestring);
    cJSON_Delete(obj);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "OTA start failed");
    }

    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_ota_status(httpd_req_t *req)
{
    ota_status_t status = {0};
    ota_update_get_status(&status);

    const char *state = "idle";
    if (status.state == OTA_STATE_DOWNLOADING) {
        state = "downloading";
    } else if (status.state == OTA_STATE_SUCCESS) {
        state = "success";
    } else if (status.state == OTA_STATE_FAILED) {
        state = "failed";
    }

    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON alloc failed");
    }

    cJSON_AddStringToObject(obj, "state", state);
    cJSON_AddNumberToObject(obj, "percent", status.percent);
    cJSON_AddNumberToObject(obj, "error", status.last_error);
    cJSON_AddStringToObject(obj, "message", status.message);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON encode failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static esp_err_t handle_screenshot(httpd_req_t *req)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_last_shot_ms) < SCREENSHOT_RATE_LIMIT_MS) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_send(req, "Rate limit", HTTPD_RESP_USE_STRLEN);
    }
    s_last_shot_ms = now_ms;

    (void)display_driver_capture_screenshot();

    uint16_t width = 0;
    uint16_t height = 0;
    if (!screenshot_get_size(&width, &height)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_send(req, "No framebuffer", HTTPD_RESP_USE_STRLEN);
    }

    size_t row_size = ((size_t)width * 3 + 3) & ~3U;
    size_t image_size = row_size * height;
    size_t file_size = 54 + image_size;

    uint8_t header[54] = {0};
    header[0] = 'B';
    header[1] = 'M';
    write_le32(&header[2], (uint32_t)file_size);
    write_le32(&header[10], 54);
    write_le32(&header[14], 40);
    write_le32(&header[18], width);
    write_le32(&header[22], (uint32_t)(-(int32_t)height));
    write_le16(&header[26], 1);
    write_le16(&header[28], 24);
    write_le32(&header[34], (uint32_t)image_size);

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");

    esp_err_t err = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
    if (err != ESP_OK) {
        return err;
    }

    uint16_t *row_pixels = malloc(width * sizeof(uint16_t));
    uint8_t *row_buf = malloc(row_size);
    if (!row_pixels || !row_buf) {
        free(row_pixels);
        free(row_buf);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_ERR_NO_MEM;
    }

    if (!screenshot_lock(250)) {
        free(row_pixels);
        free(row_buf);
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_ERR_TIMEOUT;
    }

    for (uint16_t y = 0; y < height; y++) {
        if (!screenshot_read_row_locked(y, row_pixels, width)) {
            err = ESP_FAIL;
            break;
        }

        size_t pos = 0;
        for (uint16_t x = 0; x < width; x++) {
            uint16_t px = row_pixels[x];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            uint8_t r8 = (uint8_t)((r5 * 255U) / 31U);
            uint8_t g8 = (uint8_t)((g6 * 255U) / 63U);
            uint8_t b8 = (uint8_t)((b5 * 255U) / 31U);
            row_buf[pos++] = b8;
            row_buf[pos++] = g8;
            row_buf[pos++] = r8;
        }

        while (pos < row_size) {
            row_buf[pos++] = 0;
        }

        size_t sent = 0;
        while (sent < row_size) {
            size_t chunk = row_size - sent;
            if (chunk > SCREENSHOT_CHUNK_SIZE) {
                chunk = SCREENSHOT_CHUNK_SIZE;
            }
            err = httpd_resp_send_chunk(req, (const char *)(row_buf + sent), chunk);
            if (err != ESP_OK) {
                break;
            }
            sent += chunk;
        }
        if (err != ESP_OK) {
            break;
        }
    }

    screenshot_unlock();

    free(row_pixels);
    free(row_buf);
    httpd_resp_send_chunk(req, NULL, 0);
    return err;
}

static esp_err_t start_server(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root,
        .user_ctx = NULL
    };
    httpd_uri_t shot = {
        .uri = "/screenshot.bmp",
        .method = HTTP_GET,
        .handler = handle_screenshot,
        .user_ctx = NULL
    };
    httpd_uri_t watchlist_get = {
        .uri = "/watchlist.json",
        .method = HTTP_GET,
        .handler = handle_watchlist_get,
        .user_ctx = NULL
    };
    httpd_uri_t watchlist_post = {
        .uri = "/watchlist.json",
        .method = HTTP_POST,
        .handler = handle_watchlist_post,
        .user_ctx = NULL
    };
    httpd_uri_t ota_post = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = handle_ota_post,
        .user_ctx = NULL
    };
    httpd_uri_t ota_status = {
        .uri = "/ota/status",
        .method = HTTP_GET,
        .handler = handle_ota_status,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &shot);
    httpd_register_uri_handler(s_server, &watchlist_get);
    httpd_register_uri_handler(s_server, &watchlist_post);
    httpd_register_uri_handler(s_server, &ota_post);
    httpd_register_uri_handler(s_server, &ota_status);

    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

static void stop_server(void)
{
    if (!s_server) {
        return;
    }
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "HTTP server stopped");
}

static void http_server_task(void *arg)
{
    (void)arg;
    bool was_connected = false;

    while (true) {
        wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
        wifi_manager_get_state(&wifi_state, NULL);
        bool connected = (wifi_state == WIFI_STATE_CONNECTED);

        if (connected && !was_connected) {
            start_server();
        } else if (!connected && was_connected) {
            stop_server();
        }

        was_connected = connected;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t http_server_init(void)
{
    if (s_task) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(http_server_task, "http_server", 4096, NULL, 4, &s_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create HTTP server task");
        s_task = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

void http_server_deinit(void)
{
    stop_server();
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
}

void http_server_set_state(app_state_t *state)
{
    s_state = state;
}
