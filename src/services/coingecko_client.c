#include "services/coingecko_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "services/nvs_store.h"

static const char *TAG = "coingecko";

#ifndef CT_DEV_SKIP_TLS
#define CT_DEV_SKIP_TLS 0
#endif

#define COINGECKO_BASE_URL "https://api.coingecko.com"
#define COINGECKO_LIST_ENDPOINT "/api/v3/coins/list"
#define COINGECKO_MARKETS_ENDPOINT "/api/v3/coins/markets"
#define COINGECKO_CHART_ENDPOINT "/api/v3/coins"
#define CHART_CACHE_TTL_S 60
#define CHART_MAX_POINTS 300

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buffer_t;

typedef struct {
    char id[32];
    int days;
    int64_t timestamp_s;
    chart_point_t *points;
    size_t count;
} chart_cache_entry_t;

static chart_cache_entry_t s_chart_cache[6] = {0};

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_buffer_t *buffer = (http_buffer_t *)evt->user_data;
    if (!buffer) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        size_t required = buffer->len + evt->data_len + 1;
        if (required > buffer->cap) {
            size_t new_cap = buffer->cap == 0 ? 2048 : buffer->cap * 2;
            while (new_cap < required) {
                new_cap *= 2;
            }
            char *new_buf = realloc(buffer->buf, new_cap);
            if (!new_buf) {
                return ESP_ERR_NO_MEM;
            }
            buffer->buf = new_buf;
            buffer->cap = new_cap;
        }

        memcpy(buffer->buf + buffer->len, evt->data, evt->data_len);
        buffer->len += evt->data_len;
        buffer->buf[buffer->len] = '\0';
    }

    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, char **out)
{
    http_buffer_t buffer = {0};

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 10000
    };

#if CT_DEV_SKIP_TLS
    config.skip_cert_common_name_check = true;
#else
    config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    int64_t content_len = esp_http_client_get_content_length(client);

    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP GET failed: %s url=%s", esp_err_to_name(err), url);
        } else {
            ESP_LOGW(TAG, "HTTP GET status=%d url=%s len=%lld", status, url, (long long)content_len);
        }
        free(buffer.buf);
        esp_http_client_cleanup(client);
        if (err == ESP_OK && status == 429) {
            return ESP_ERR_TIMEOUT;
        }
        return err != ESP_OK ? err : ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    *out = buffer.buf;
    return ESP_OK;
}

static esp_err_t parse_coin_list(const char *json, coin_list_t *list)
{
    if (!json || !list) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t total = cJSON_GetArraySize(root);
    if (total > COINGECKO_MAX_COINS) {
        total = COINGECKO_MAX_COINS;
    }

    coin_meta_t *items = calloc(total, sizeof(coin_meta_t));
    if (!items) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (count >= total) {
            break;
        }

        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *symbol = cJSON_GetObjectItem(item, "symbol");
        cJSON *name = cJSON_GetObjectItem(item, "name");

        if (!cJSON_IsString(id) || !cJSON_IsString(symbol) || !cJSON_IsString(name)) {
            continue;
        }

        strncpy(items[count].id, id->valuestring, sizeof(items[count].id) - 1);
        strncpy(items[count].symbol, symbol->valuestring, sizeof(items[count].symbol) - 1);
        strncpy(items[count].name, name->valuestring, sizeof(items[count].name) - 1);
        count++;
    }

    cJSON_Delete(root);
    list->items = items;
    list->count = count;
    return ESP_OK;
}

static char *url_encode(const char *src)
{
    if (!src) {
        return NULL;
    }

    size_t len = strlen(src);
    size_t cap = len * 3 + 1;
    char *out = malloc(cap);
    if (!out) {
        return NULL;
    }

    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[pos++] = (char)c;
        } else if (c == ' ') {
            out[pos++] = '%';
            out[pos++] = '2';
            out[pos++] = '0';
        } else {
            static const char hex[] = "0123456789ABCDEF";
            out[pos++] = '%';
            out[pos++] = hex[(c >> 4) & 0xF];
            out[pos++] = hex[c & 0xF];
        }
    }

    out[pos] = '\0';
    return out;
}

static esp_err_t parse_search_results(const char *json, coin_list_t *list, size_t limit)
{
    if (!json || !list || limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *coins = cJSON_GetObjectItem(root, "coins");
    if (!coins || !cJSON_IsArray(coins)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t total = cJSON_GetArraySize(coins);
    if (total > limit) {
        total = limit;
    }

    coin_meta_t *items = calloc(total, sizeof(coin_meta_t));
    if (!items) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t count = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, coins) {
        if (count >= total) {
            break;
        }

        cJSON *id = cJSON_GetObjectItem(item, "id");
        cJSON *symbol = cJSON_GetObjectItem(item, "symbol");
        cJSON *name = cJSON_GetObjectItem(item, "name");

        if (!cJSON_IsString(id) || !cJSON_IsString(symbol) || !cJSON_IsString(name)) {
            continue;
        }

        strncpy(items[count].id, id->valuestring, sizeof(items[count].id) - 1);
        strncpy(items[count].symbol, symbol->valuestring, sizeof(items[count].symbol) - 1);
        strncpy(items[count].name, name->valuestring, sizeof(items[count].name) - 1);
        count++;
    }

    cJSON_Delete(root);
    list->items = items;
    list->count = count;
    return ESP_OK;
}

static char *build_ids_query(const coin_list_t *list)
{
    if (!list || !list->items || list->count == 0) {
        return NULL;
    }

    size_t cap = list->count * 40;
    if (cap < 64) {
        cap = 64;
    }

    char *ids = malloc(cap);
    if (!ids) {
        return NULL;
    }

    size_t pos = 0;
    ids[0] = '\0';
    for (size_t i = 0; i < list->count; i++) {
        const char *id = list->items[i].id;
        if (!id || id[0] == '\0') {
            continue;
        }

        size_t id_len = strlen(id);
        size_t needed = pos + id_len + ((pos > 0) ? 1 : 0) + 1;
        if (needed > cap) {
            size_t new_cap = cap * 2;
            while (new_cap < needed) {
                new_cap *= 2;
            }
            char *grown = realloc(ids, new_cap);
            if (!grown) {
                free(ids);
                return NULL;
            }
            ids = grown;
            cap = new_cap;
        }

        if (pos > 0) {
            ids[pos++] = ',';
        }
        memcpy(ids + pos, id, id_len);
        pos += id_len;
        ids[pos] = '\0';
    }

    if (pos == 0) {
        free(ids);
        return NULL;
    }

    return ids;
}

static void enrich_search_prices(coin_list_t *list)
{
    if (!list || !list->items || list->count == 0) {
        return;
    }

    char *ids_csv = build_ids_query(list);
    if (!ids_csv) {
        return;
    }

    char *ids_encoded = url_encode(ids_csv);
    free(ids_csv);
    if (!ids_encoded) {
        return;
    }

    char url[1024];
    snprintf(url,
             sizeof(url),
             "%s/api/v3/simple/price?ids=%s&vs_currencies=usd",
             COINGECKO_BASE_URL,
             ids_encoded);
    free(ids_encoded);

    char *json = NULL;
    if (http_get_json(url, &json) != ESP_OK || !json) {
        free(json);
        return;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    for (size_t i = 0; i < list->count; i++) {
        list->items[i].usd_price = 0.0;
        cJSON *coin_obj = cJSON_GetObjectItem(root, list->items[i].id);
        if (!coin_obj || !cJSON_IsObject(coin_obj)) {
            continue;
        }

        cJSON *usd = cJSON_GetObjectItem(coin_obj, "usd");
        if (cJSON_IsNumber(usd)) {
            list->items[i].usd_price = usd->valuedouble;
        }
    }

    cJSON_Delete(root);
}

static chart_cache_entry_t *get_cache_entry(const char *coin_id, int days)
{
    for (size_t i = 0; i < sizeof(s_chart_cache) / sizeof(s_chart_cache[0]); i++) {
        if (strcasecmp(s_chart_cache[i].id, coin_id) == 0 && s_chart_cache[i].days == days) {
            return &s_chart_cache[i];
        }
    }
    return NULL;
}

static chart_cache_entry_t *get_cache_slot(void)
{
    chart_cache_entry_t *oldest = &s_chart_cache[0];
    for (size_t i = 0; i < sizeof(s_chart_cache) / sizeof(s_chart_cache[0]); i++) {
        if (s_chart_cache[i].id[0] == '\0') {
            return &s_chart_cache[i];
        }
        if (s_chart_cache[i].timestamp_s < oldest->timestamp_s) {
            oldest = &s_chart_cache[i];
        }
    }
    return oldest;
}

static void free_cache_entry(chart_cache_entry_t *entry)
{
    if (!entry) {
        return;
    }
    free(entry->points);
    entry->points = NULL;
    entry->count = 0;
    entry->id[0] = '\0';
    entry->days = 0;
    entry->timestamp_s = 0;
}

static esp_err_t parse_chart_points(const char *json, chart_point_t **out_points, size_t *out_count)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *prices = cJSON_GetObjectItem(root, "prices");
    if (!prices || !cJSON_IsArray(prices)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t total = cJSON_GetArraySize(prices);
    if (total == 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t step = 1;
    if (total > CHART_MAX_POINTS) {
        step = total / CHART_MAX_POINTS;
        if (step == 0) {
            step = 1;
        }
    }

    size_t count = (total + step - 1) / step;
    chart_point_t *points = calloc(count, sizeof(chart_point_t));
    if (!points) {
        cJSON_Delete(root);
        return ESP_ERR_NO_MEM;
    }

    size_t idx = 0;
    for (size_t i = 0; i < total; i += step) {
        cJSON *pair = cJSON_GetArrayItem(prices, (int)i);
        if (!pair || !cJSON_IsArray(pair)) {
            continue;
        }

        cJSON *ts = cJSON_GetArrayItem(pair, 0);
        cJSON *price = cJSON_GetArrayItem(pair, 1);
        if (!cJSON_IsNumber(ts) || !cJSON_IsNumber(price)) {
            continue;
        }

        points[idx].ts_ms = (int64_t)ts->valuedouble;
        points[idx].price = price->valuedouble;
        idx++;
        if (idx >= count) {
            break;
        }
    }

    cJSON_Delete(root);
    *out_points = points;
    *out_count = idx;
    return ESP_OK;
}

static coin_t *find_coin_by_id(app_state_t *state, const char *id)
{
    if (!state || !id) {
        return NULL;
    }

    for (size_t i = 0; i < state->watchlist_count; i++) {
        if (strcasecmp(state->watchlist[i].id, id) == 0) {
            return &state->watchlist[i];
        }
    }
    return NULL;
}

static char *build_ids_csv(const app_state_t *state)
{
    if (!state || state->watchlist_count == 0) {
        return NULL;
    }

    size_t total = 0;
    for (size_t i = 0; i < state->watchlist_count; i++) {
        total += strlen(state->watchlist[i].id) + 1;
    }

    char *csv = malloc(total + 1);
    if (!csv) {
        return NULL;
    }

    csv[0] = '\0';
    for (size_t i = 0; i < state->watchlist_count; i++) {
        if (i > 0) {
            strcat(csv, ",");
        }
        strcat(csv, state->watchlist[i].id);
    }

    return csv;
}

static bool json_to_double(const cJSON *item, double *out)
{
    if (!item || !out) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        char *end = NULL;
        double value = strtod(item->valuestring, &end);
        if (end && end != item->valuestring) {
            *out = value;
            return true;
        }
    }
    return false;
}

static void update_coin_from_json(coin_t *coin, const cJSON *item, bool update_price)
{
    cJSON *price = cJSON_GetObjectItem(item, "current_price");
    cJSON *pct1h = cJSON_GetObjectItem(item, "price_change_percentage_1h_in_currency");
    cJSON *pct24h = cJSON_GetObjectItem(item, "price_change_percentage_24h_in_currency");
    cJSON *pct7d = cJSON_GetObjectItem(item, "price_change_percentage_7d_in_currency");
    cJSON *pct30d = cJSON_GetObjectItem(item, "price_change_percentage_30d_in_currency");
    cJSON *pct1y = cJSON_GetObjectItem(item, "price_change_percentage_1y_in_currency");
    cJSON *high24 = cJSON_GetObjectItem(item, "high_24h");
    cJSON *low24 = cJSON_GetObjectItem(item, "low_24h");
    cJSON *market_cap = cJSON_GetObjectItem(item, "market_cap");
    cJSON *volume = cJSON_GetObjectItem(item, "total_volume");

    double value = 0.0;
    if (update_price) {
        if (json_to_double(price, &value)) {
            coin->price = value;
        }
    }
    if (json_to_double(pct1h, &value)) {
        coin->change_1h = value;
    }
    if (json_to_double(pct24h, &value)) {
        coin->change_24h = value;
    }
    if (json_to_double(pct7d, &value)) {
        coin->change_7d = value;
    }
    if (json_to_double(pct30d, &value)) {
        coin->change_30d = value;
    }
    if (json_to_double(pct1y, &value)) {
        coin->change_1y = value;
    }
    if (update_price) {
        if (json_to_double(high24, &value)) {
            coin->high_24h = value;
        }
        if (json_to_double(low24, &value)) {
            coin->low_24h = value;
        }
        if (json_to_double(market_cap, &value)) {
            coin->market_cap = value;
        }
        if (json_to_double(volume, &value)) {
            coin->volume_24h = value;
        }
    }
}

esp_err_t coingecko_client_init(void)
{
    ESP_LOGI(TAG, "CoinGecko client ready");
    return ESP_OK;
}

esp_err_t coingecko_client_fetch_coin_list(coin_list_t *list)
{
    if (!list) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[128];
    snprintf(url, sizeof(url), "%s%s", COINGECKO_BASE_URL, COINGECKO_LIST_ENDPOINT);

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_coin_list(json, list);
    if (err == ESP_OK) {
        int64_t now = esp_timer_get_time() / 1000000;
        nvs_store_save_coin_cache(json, now);
    }

    free(json);
    return err;
}

esp_err_t coingecko_client_search_coins(const char *query, coin_list_t *list, size_t limit)
{
    if (!query || !list || limit == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *encoded = url_encode(query);
    if (!encoded) {
        return ESP_ERR_NO_MEM;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s/api/v3/search?query=%s", COINGECKO_BASE_URL, encoded);
    free(encoded);

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    err = parse_search_results(json, list, limit);
    free(json);
    if (err == ESP_OK) {
        enrich_search_prices(list);
    }
    return err;
}

esp_err_t coingecko_client_fetch_markets_mode(app_state_t *state, bool update_price)
{
    if (!state || state->watchlist_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char *ids = build_ids_csv(state);
    if (!ids) {
        return ESP_ERR_NO_MEM;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "%s%s?vs_currency=usd&ids=%s&price_change_percentage=1h,24h,7d,30d,1y&sparkline=false&precision=full",
             COINGECKO_BASE_URL,
             COINGECKO_MARKETS_ENDPOINT,
             ids);

    free(ids);

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        free(json);
        return ESP_FAIL;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        cJSON *id = cJSON_GetObjectItem(item, "id");
        if (!cJSON_IsString(id)) {
            continue;
        }

        coin_t *coin = find_coin_by_id(state, id->valuestring);
        if (!coin) {
            continue;
        }

        update_coin_from_json(coin, item, update_price);
    }

    cJSON_Delete(root);
    free(json);
    return ESP_OK;
}

esp_err_t coingecko_client_fetch_markets(app_state_t *state)
{
    return coingecko_client_fetch_markets_mode(state, true);
}

esp_err_t coingecko_client_get_chart(const char *coin_id, int days, const chart_point_t **points, size_t *count)
{
    if (!coin_id || !points || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    chart_cache_entry_t *entry = get_cache_entry(coin_id, days);
    if (entry && (now_s - entry->timestamp_s) < CHART_CACHE_TTL_S && entry->points) {
        *points = entry->points;
        *count = entry->count;
        return ESP_OK;
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s/%s/market_chart?vs_currency=usd&days=%d",
             COINGECKO_BASE_URL,
             COINGECKO_CHART_ENDPOINT,
             coin_id,
             days);

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    chart_point_t *new_points = NULL;
    size_t new_count = 0;
    err = parse_chart_points(json, &new_points, &new_count);
    free(json);
    if (err != ESP_OK) {
        return err;
    }

    if (!entry) {
        entry = get_cache_slot();
        free_cache_entry(entry);
        strncpy(entry->id, coin_id, sizeof(entry->id) - 1);
        entry->days = days;
    } else {
        free(entry->points);
    }

    entry->points = new_points;
    entry->count = new_count;
    entry->timestamp_s = now_s;

    *points = entry->points;
    *count = entry->count;
    return ESP_OK;
}

esp_err_t coingecko_client_get_chart_cached(const char *coin_id, int days, const chart_point_t **points, size_t *count)
{
    if (!coin_id || !points || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    chart_cache_entry_t *entry = get_cache_entry(coin_id, days);
    if (!entry || !entry->points) {
        return ESP_ERR_NOT_FOUND;
    }

    if ((now_s - entry->timestamp_s) >= CHART_CACHE_TTL_S) {
        return ESP_ERR_NOT_FOUND;
    }

    *points = entry->points;
    *count = entry->count;
    return ESP_OK;
}

esp_err_t coingecko_client_load_cached_list(coin_list_t *list)
{
    if (!list) {
        return ESP_ERR_INVALID_ARG;
    }

    char *json = NULL;
    int64_t ts = 0;
    esp_err_t err = nvs_store_load_coin_cache(&json, &ts);
    if (err != ESP_OK || !json) {
        return err;
    }

    err = parse_coin_list(json, list);
    free(json);
    return err;
}

esp_err_t coingecko_client_cache_list(const coin_list_t *list)
{
    if (!list || !list->items || list->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_CreateArray();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < list->count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddStringToObject(item, "id", list->items[i].id);
        cJSON_AddStringToObject(item, "symbol", list->items[i].symbol);
        cJSON_AddStringToObject(item, "name", list->items[i].name);
        cJSON_AddItemToArray(root, item);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return ESP_ERR_NO_MEM;
    }

    int64_t now = esp_timer_get_time() / 1000000;
    esp_err_t err = nvs_store_save_coin_cache(json, now);
    free(json);
    return err;
}

const coin_meta_t *coingecko_client_find_symbol(const coin_list_t *list, const char *symbol)
{
    if (!list || !list->items || !symbol) {
        return NULL;
    }

    for (size_t i = 0; i < list->count; i++) {
        if (strcasecmp(list->items[i].symbol, symbol) == 0) {
            return &list->items[i];
        }
    }

    return NULL;
}

void coingecko_client_free_list(coin_list_t *list)
{
    if (!list) {
        return;
    }

    free(list->items);
    list->items = NULL;
    list->count = 0;
}
