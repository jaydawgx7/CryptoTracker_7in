#include "services/nvs_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_store";

#define NVS_NAMESPACE "ct"
#define NVS_KEY_WATCHLIST "watchlist"
#define NVS_KEY_PREFS "prefs"
#define NVS_KEY_CACHE_TS "coin_cache_ts"
#define NVS_KEY_CACHE_LEN "coin_cache_len"
#define NVS_KEY_CACHE_CHUNKS "coin_cache_chunks"
#define NVS_CACHE_CHUNK_SIZE 1800

static void set_default_prefs(ui_prefs_t *prefs)
{
    prefs->dark_mode = true;
    prefs->sort_field = SORT_SYMBOL;
    prefs->sort_desc = false;
    prefs->refresh_seconds = 10;
    prefs->brightness = 60;
    prefs->buzzer_enabled = true;
}

static void set_default_watchlist(app_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->watchlist_count = 2;

    strncpy(state->watchlist[0].id, "bitcoin", sizeof(state->watchlist[0].id) - 1);
    strncpy(state->watchlist[0].symbol, "BTC", sizeof(state->watchlist[0].symbol) - 1);
    strncpy(state->watchlist[0].name, "Bitcoin", sizeof(state->watchlist[0].name) - 1);

    strncpy(state->watchlist[1].id, "ripple", sizeof(state->watchlist[1].id) - 1);
    strncpy(state->watchlist[1].symbol, "XRP", sizeof(state->watchlist[1].symbol) - 1);
    strncpy(state->watchlist[1].name, "XRP", sizeof(state->watchlist[1].name) - 1);

    set_default_prefs(&state->prefs);
}

esp_err_t nvs_store_init(void)
{
    ESP_LOGI(TAG, "NVS store initialized");
    return ESP_OK;
}

static esp_err_t nvs_open_namespace(nvs_handle_t *handle)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
}

static esp_err_t save_blob(nvs_handle_t handle, const char *key, const void *data, size_t len)
{
    esp_err_t err = nvs_set_blob(handle, key, data, len);
    if (err != ESP_OK) {
        return err;
    }
    return nvs_commit(handle);
}

static esp_err_t load_blob(nvs_handle_t handle, const char *key, void **data, size_t *len)
{
    esp_err_t err = nvs_get_blob(handle, key, NULL, len);
    if (err != ESP_OK) {
        return err;
    }

    void *buf = malloc(*len);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, key, buf, len);
    if (err != ESP_OK) {
        free(buf);
        return err;
    }

    *data = buf;
    return ESP_OK;
}

static cJSON *watchlist_to_json(const app_state_t *state)
{
    cJSON *array = cJSON_CreateArray();
    if (!array) {
        return NULL;
    }

    for (size_t i = 0; i < state->watchlist_count; i++) {
        const coin_t *coin = &state->watchlist[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(array);
            return NULL;
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
    if (!cJSON_IsArray(array)) {
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

        count++;
    }

    state->watchlist_count = count;
    return count > 0;
}

static cJSON *prefs_to_json(const ui_prefs_t *prefs)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }

    cJSON_AddBoolToObject(obj, "dark_mode", prefs->dark_mode);
    cJSON_AddNumberToObject(obj, "sort_field", prefs->sort_field);
    cJSON_AddBoolToObject(obj, "sort_desc", prefs->sort_desc);
    cJSON_AddNumberToObject(obj, "refresh_seconds", prefs->refresh_seconds);
    cJSON_AddNumberToObject(obj, "brightness", prefs->brightness);
    cJSON_AddBoolToObject(obj, "buzzer_enabled", prefs->buzzer_enabled);

    return obj;
}

static void json_to_prefs(ui_prefs_t *prefs, const cJSON *obj)
{
    if (!cJSON_IsObject(obj)) {
        return;
    }

    cJSON *dark_mode = cJSON_GetObjectItem(obj, "dark_mode");
    cJSON *sort_field = cJSON_GetObjectItem(obj, "sort_field");
    cJSON *sort_desc = cJSON_GetObjectItem(obj, "sort_desc");
    cJSON *refresh_seconds = cJSON_GetObjectItem(obj, "refresh_seconds");
    cJSON *brightness = cJSON_GetObjectItem(obj, "brightness");
    cJSON *buzzer_enabled = cJSON_GetObjectItem(obj, "buzzer_enabled");

    if (cJSON_IsBool(dark_mode)) {
        prefs->dark_mode = cJSON_IsTrue(dark_mode);
    }
    if (cJSON_IsNumber(sort_field)) {
        prefs->sort_field = (sort_field_t)sort_field->valueint;
    }
    if (cJSON_IsBool(sort_desc)) {
        prefs->sort_desc = cJSON_IsTrue(sort_desc);
    }
    if (cJSON_IsNumber(refresh_seconds)) {
        prefs->refresh_seconds = (uint16_t)refresh_seconds->valueint;
    }
    if (cJSON_IsNumber(brightness)) {
        prefs->brightness = (uint8_t)brightness->valueint;
    }
    if (cJSON_IsBool(buzzer_enabled)) {
        prefs->buzzer_enabled = cJSON_IsTrue(buzzer_enabled);
    }
}

esp_err_t nvs_store_load_app_state(app_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(state, 0, sizeof(*state));
    set_default_prefs(&state->prefs);

    nvs_handle_t handle;
    esp_err_t err = nvs_open_namespace(&handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %d", err);
        set_default_watchlist(state);
        return err;
    }

    void *watchlist_blob = NULL;
    size_t watchlist_len = 0;
    err = load_blob(handle, NVS_KEY_WATCHLIST, &watchlist_blob, &watchlist_len);
    if (err == ESP_OK && watchlist_blob) {
        cJSON *array = cJSON_ParseWithLength((const char *)watchlist_blob, watchlist_len);
        if (!json_to_watchlist(state, array)) {
            set_default_watchlist(state);
        }
        cJSON_Delete(array);
    } else {
        set_default_watchlist(state);
    }
    free(watchlist_blob);

    void *prefs_blob = NULL;
    size_t prefs_len = 0;
    err = load_blob(handle, NVS_KEY_PREFS, &prefs_blob, &prefs_len);
    if (err == ESP_OK && prefs_blob) {
        cJSON *obj = cJSON_ParseWithLength((const char *)prefs_blob, prefs_len);
        if (obj) {
            json_to_prefs(&state->prefs, obj);
        }
        cJSON_Delete(obj);
    }
    free(prefs_blob);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t nvs_store_save_app_state(const app_state_t *state)
{
    if (!state) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *watchlist = watchlist_to_json(state);
    cJSON *prefs = prefs_to_json(&state->prefs);
    if (!watchlist || !prefs) {
        cJSON_Delete(watchlist);
        cJSON_Delete(prefs);
        return ESP_ERR_NO_MEM;
    }

    char *watchlist_str = cJSON_PrintUnformatted(watchlist);
    char *prefs_str = cJSON_PrintUnformatted(prefs);
    cJSON_Delete(watchlist);
    cJSON_Delete(prefs);

    if (!watchlist_str || !prefs_str) {
        free(watchlist_str);
        free(prefs_str);
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_namespace(&handle);
    if (err != ESP_OK) {
        free(watchlist_str);
        free(prefs_str);
        return err;
    }

    err = save_blob(handle, NVS_KEY_WATCHLIST, watchlist_str, strlen(watchlist_str) + 1);
    if (err == ESP_OK) {
        err = save_blob(handle, NVS_KEY_PREFS, prefs_str, strlen(prefs_str) + 1);
    }

    nvs_close(handle);
    free(watchlist_str);
    free(prefs_str);
    return err;
}

esp_err_t nvs_store_save_coin_cache(const char *json, int64_t timestamp)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_namespace(&handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t len = strlen(json) + 1;
    uint32_t chunks = (len + NVS_CACHE_CHUNK_SIZE - 1) / NVS_CACHE_CHUNK_SIZE;

    err = nvs_set_i64(handle, NVS_KEY_CACHE_TS, timestamp);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    err = nvs_set_u32(handle, NVS_KEY_CACHE_LEN, (uint32_t)len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    err = nvs_set_u32(handle, NVS_KEY_CACHE_CHUNKS, chunks);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    for (uint32_t i = 0; i < chunks; i++) {
        char key[20];
        snprintf(key, sizeof(key), "coin_cache_%lu", (unsigned long)i);
        size_t offset = i * NVS_CACHE_CHUNK_SIZE;
        size_t remaining = len - offset;
        size_t chunk_len = remaining > NVS_CACHE_CHUNK_SIZE ? NVS_CACHE_CHUNK_SIZE : remaining;
        err = nvs_set_blob(handle, key, json + offset, chunk_len);
        if (err != ESP_OK) {
            nvs_close(handle);
            return err;
        }
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t nvs_store_load_coin_cache(char **json, int64_t *timestamp)
{
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    *json = NULL;

    nvs_handle_t handle;
    esp_err_t err = nvs_open_namespace(&handle);
    if (err != ESP_OK) {
        return err;
    }

    int64_t ts = 0;
    uint32_t len = 0;
    uint32_t chunks = 0;
    if (nvs_get_i64(handle, NVS_KEY_CACHE_TS, &ts) != ESP_OK ||
        nvs_get_u32(handle, NVS_KEY_CACHE_LEN, &len) != ESP_OK ||
        nvs_get_u32(handle, NVS_KEY_CACHE_CHUNKS, &chunks) != ESP_OK) {
        nvs_close(handle);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    char *buffer = malloc(len);
    if (!buffer) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t i = 0; i < chunks; i++) {
        char key[20];
        snprintf(key, sizeof(key), "coin_cache_%lu", (unsigned long)i);
        size_t chunk_len = 0;
        err = nvs_get_blob(handle, key, NULL, &chunk_len);
        if (err != ESP_OK) {
            free(buffer);
            nvs_close(handle);
            return err;
        }
        err = nvs_get_blob(handle, key, buffer + (i * NVS_CACHE_CHUNK_SIZE), &chunk_len);
        if (err != ESP_OK) {
            free(buffer);
            nvs_close(handle);
            return err;
        }
    }

    buffer[len - 1] = '\0';
    if (timestamp) {
        *timestamp = ts;
    }
    *json = buffer;

    nvs_close(handle);
    return ESP_OK;
}
