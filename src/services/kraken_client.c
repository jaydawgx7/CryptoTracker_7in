#include "services/kraken_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/wifi_manager.h"

static const char *TAG = "kraken";

#define KRAKEN_BASE_URL "https://api.kraken.com"
#define KRAKEN_TICKER_ENDPOINT "/0/public/Ticker"

typedef struct {
    const char *symbol;
    const char *request_pair;
    const char *result_key;
} kraken_pair_map_t;

static esp_err_t http_get_json(const char *url, char **out);
static bool json_to_double(const cJSON *item, double *out);

typedef struct {
    const char *symbol;
    const char *request_pair;
    const char *result_key;
    char symbol_buf[12];
    char request_pair_buf[16];
    char result_key_buf[16];
    uint8_t priority;
    bool price_seen;
} kraken_pair_entry_t;

#define KRAKEN_DYNAMIC_MAX 128

static const kraken_pair_map_t s_pair_map[] = {
    {"BTC", "XBTUSD", "XXBTZUSD"},
    {"ETH", "ETHUSD", "XETHZUSD"},
    {"SOL", "SOLUSD", "SOLUSD"},
    {"XRP", "XRPUSD", "XXRPZUSD"},
    {"XLM", "XLMUSD", "XXLMZUSD"},
    {"ADA", "ADAUSD", "ADAUSD"},
    {"DOGE", "DOGEUSD", "XDGUSD"},
    {"AVAX", "AVAXUSD", "AVAXUSD"},
    {"DOT", "DOTUSD", "DOTUSD"},
    {"LINK", "LINKUSD", "LINKUSD"},
    {"LTC", "LTCUSD", "XLTCZUSD"},
    {"BCH", "BCHUSD", "BCHUSD"},
    {"TRX", "TRXUSD", "TRXUSD"},
    {"MATIC", "MATICUSD", "MATICUSD"}
};

static kraken_pair_entry_t s_dynamic_pairs[KRAKEN_DYNAMIC_MAX] = {0};
static size_t s_dynamic_pair_count = 0;
static bool s_dynamic_pairs_loaded = false;
static int64_t s_dynamic_pairs_last_attempt_ms = 0;
static bool s_dynamic_pairs_inflight = false;

#define DYNAMIC_PAIR_RETRY_MS 300000
#define DYNAMIC_PAIR_RETRY_FAIL_MS 15000


typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buffer_t;

typedef struct {
    const char *request_pair;
    const char *result_key;
    coin_t *coin;
} kraken_pair_t;

typedef struct {
    size_t count;
    char symbols[MAX_WATCHLIST][12];
} dynamic_pair_task_ctx_t;

static void normalize_symbol(const char *in, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!in) {
        return;
    }

    char tmp[12];
    size_t len = 0;
    for (size_t i = 0; in[i] != '\0' && len + 1 < sizeof(tmp); i++) {
        if (isalnum((unsigned char)in[i])) {
            tmp[len++] = (char)toupper((unsigned char)in[i]);
        }
    }
    tmp[len] = '\0';

    if (strcmp(tmp, "XBT") == 0) {
        strncpy(out, "BTC", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    if (strcmp(tmp, "XDG") == 0) {
        strncpy(out, "DOGE", out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }
    if (len == 4 && tmp[0] == 'X') {
        strncpy(out, tmp + 1, out_len - 1);
        out[out_len - 1] = '\0';
        return;
    }

    strncpy(out, tmp, out_len - 1);
    out[out_len - 1] = '\0';
}

static bool parse_altname_base_quote(const char *altname, char *base, size_t base_len, uint8_t *priority)
{
    if (!altname || !base || base_len == 0) {
        return false;
    }

    size_t len = strlen(altname);
    if (len > 4 && strcmp(altname + (len - 4), "USDT") == 0) {
        size_t base_count = len - 4;
        if (base_count >= base_len) {
            base_count = base_len - 1;
        }
        strncpy(base, altname, base_count);
        base[base_count] = '\0';
        if (priority) {
            *priority = 1;
        }
        return true;
    }
    if (len > 3 && strcmp(altname + (len - 3), "USD") == 0) {
        size_t base_count = len - 3;
        if (base_count >= base_len) {
            base_count = base_len - 1;
        }
        strncpy(base, altname, base_count);
        base[base_count] = '\0';
        if (priority) {
            *priority = 0;
        }
        return true;
    }

    return false;
}

static const kraken_pair_map_t *map_symbol_to_pair(const char *symbol)
{
    if (!symbol) {
        return NULL;
    }

    char normalized[12];
    normalize_symbol(symbol, normalized, sizeof(normalized));
    if (normalized[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < sizeof(s_pair_map) / sizeof(s_pair_map[0]); i++) {
        if (strcasecmp(normalized, s_pair_map[i].symbol) == 0) {
            return &s_pair_map[i];
        }
    }

    for (size_t i = 0; i < s_dynamic_pair_count; i++) {
        if (strcasecmp(normalized, s_dynamic_pairs[i].symbol) == 0) {
            return (const kraken_pair_map_t *)&s_dynamic_pairs[i];
        }
    }

    return NULL;
}

static kraken_pair_entry_t *get_dynamic_entry(const kraken_pair_map_t *map)
{
    if (!map || s_dynamic_pair_count == 0) {
        return NULL;
    }

    const kraken_pair_entry_t *start = &s_dynamic_pairs[0];
    const kraken_pair_entry_t *end = &s_dynamic_pairs[s_dynamic_pair_count];
    if ((const kraken_pair_entry_t *)map < start || (const kraken_pair_entry_t *)map >= end) {
        return NULL;
    }

    return (kraken_pair_entry_t *)map;
}

static esp_err_t load_dynamic_pairs_from_url(const char *url, const char **symbols, size_t symbol_count, int *out_added)
{
    if (!url || !symbols || symbol_count == 0) {
        if (out_added) {
            *out_added = 0;
        }
        return ESP_ERR_INVALID_ARG;
    }

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        if (out_added) {
            *out_added = 0;
        }
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        if (out_added) {
            *out_added = 0;
        }
        return ESP_FAIL;
    }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsArray(error) && cJSON_GetArraySize(error) > 0) {
        ESP_LOGW(TAG, "AssetPairs lookup returned errors");
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsObject(result)) {
        cJSON_Delete(root);
        if (out_added) {
            *out_added = 0;
        }
        return ESP_OK;
    }

    size_t start_count = s_dynamic_pair_count;

    for (size_t i = 0; i < symbol_count; i++) {
        if (!symbols[i]) {
            continue;
        }
        if (map_symbol_to_pair(symbols[i])) {
            continue;
        }

        char target_symbol[12];
        normalize_symbol(symbols[i], target_symbol, sizeof(target_symbol));
        if (target_symbol[0] == '\0') {
            continue;
        }

        const char *best_key = NULL;
        const char *best_alt = NULL;
        uint8_t best_priority = 255;

        cJSON *entry = NULL;
        cJSON_ArrayForEach(entry, result) {
            if (!cJSON_IsObject(entry)) {
                continue;
            }

            cJSON *altname = cJSON_GetObjectItem(entry, "altname");
            if (!cJSON_IsString(altname)) {
                continue;
            }

            char base[12];
            uint8_t priority = 0;
            if (!parse_altname_base_quote(altname->valuestring, base, sizeof(base), &priority)) {
                continue;
            }

            char normalized_base[12];
            normalize_symbol(base, normalized_base, sizeof(normalized_base));
            if (strcasecmp(normalized_base, target_symbol) != 0) {
                continue;
            }

            if (priority < best_priority) {
                best_priority = priority;
                best_alt = altname->valuestring;
                best_key = entry->string;
                if (best_priority == 0) {
                    break;
                }
            }
        }

        if (!best_key || !best_alt) {
            continue;
        }

        if (s_dynamic_pair_count < KRAKEN_DYNAMIC_MAX) {
            kraken_pair_entry_t *dst = &s_dynamic_pairs[s_dynamic_pair_count++];
            strncpy(dst->symbol_buf, target_symbol, sizeof(dst->symbol_buf) - 1);
            dst->symbol_buf[sizeof(dst->symbol_buf) - 1] = '\0';
            strncpy(dst->request_pair_buf, best_alt, sizeof(dst->request_pair_buf) - 1);
            dst->request_pair_buf[sizeof(dst->request_pair_buf) - 1] = '\0';
            strncpy(dst->result_key_buf, best_key, sizeof(dst->result_key_buf) - 1);
            dst->result_key_buf[sizeof(dst->result_key_buf) - 1] = '\0';
            dst->symbol = dst->symbol_buf;
            dst->request_pair = dst->request_pair_buf;
            dst->result_key = dst->result_key_buf;
            dst->priority = best_priority;
            dst->price_seen = false;
        }
    }

    cJSON_Delete(root);
    if (out_added) {
        *out_added = (int)(s_dynamic_pair_count - start_count);
    }
    return ESP_OK;
}

static esp_err_t load_dynamic_pairs_for_symbols(const char **symbols, size_t symbol_count, bool *out_transient)
{
    if (!symbols || symbol_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool transient = false;
    int total_added = 0;
    char url[256];

    for (size_t i = 0; i < symbol_count; i++) {
        if (!symbols[i]) {
            continue;
        }
        if (map_symbol_to_pair(symbols[i])) {
            continue;
        }

        char base[12];
        normalize_symbol(symbols[i], base, sizeof(base));
        if (base[0] == '\0') {
            continue;
        }

        const char *suffixes[] = {"USD", "USDT"};
        for (size_t s = 0; s < sizeof(suffixes) / sizeof(suffixes[0]); s++) {
            snprintf(url, sizeof(url), "%s/0/public/AssetPairs?pair=%s%s", KRAKEN_BASE_URL, base, suffixes[s]);
            int added = 0;
            esp_err_t err = load_dynamic_pairs_from_url(url, &symbols[i], 1, &added);
            if (err != ESP_OK) {
                if (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_TIMEOUT || err == ESP_ERR_INVALID_STATE) {
                    transient = true;
                }
                continue;
            }
            if (added > 0) {
                total_added += added;
                break;
            }
        }
    }

    s_dynamic_pairs_loaded = (s_dynamic_pair_count > 0);
    if (out_transient) {
        *out_transient = transient;
    }
    if (total_added > 0) {
        return ESP_OK;
    }
    if (transient) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_FAIL;
}

static void dynamic_pairs_task(void *arg)
{
    dynamic_pair_task_ctx_t *ctx = (dynamic_pair_task_ctx_t *)arg;
    if (!ctx) {
        s_dynamic_pairs_inflight = false;
        vTaskDelete(NULL);
        return;
    }

    const char *symbols[MAX_WATCHLIST] = {0};
    for (size_t i = 0; i < ctx->count && i < MAX_WATCHLIST; i++) {
        symbols[i] = ctx->symbols[i];
    }

    bool transient = false;
    esp_err_t err = load_dynamic_pairs_for_symbols(symbols, ctx->count, &transient);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Dynamic pair load failed: %s", esp_err_to_name(err));
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (transient) {
            int64_t retry_delay = DYNAMIC_PAIR_RETRY_MS - DYNAMIC_PAIR_RETRY_FAIL_MS;
            s_dynamic_pairs_last_attempt_ms = now_ms - (retry_delay > 0 ? retry_delay : 0);
        } else {
            s_dynamic_pairs_last_attempt_ms = now_ms;
        }
    }

    free(ctx);
    s_dynamic_pairs_inflight = false;
    vTaskDelete(NULL);
}

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

    config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP GET failed: %s url=%s", esp_err_to_name(err), url);
        } else {
            ESP_LOGW(TAG, "HTTP GET status=%d url=%s", status, url);
        }
        free(buffer.buf);
        esp_http_client_cleanup(client);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    *out = buffer.buf;
    return ESP_OK;
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

esp_err_t kraken_client_fetch_prices(app_state_t *state, bool *out_any_missing)
{
    if (!state || state->watchlist_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_state_t wifi_state = WIFI_STATE_DISCONNECTED;
    wifi_manager_get_state(&wifi_state, NULL);
    if (wifi_state != WIFI_STATE_CONNECTED) {
        if (out_any_missing) {
            *out_any_missing = true;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (s_dynamic_pairs_inflight) {
        if (out_any_missing) {
            *out_any_missing = true;
        }
        return ESP_ERR_INVALID_STATE;
    }

    kraken_pair_t pairs[MAX_WATCHLIST] = {0};
    size_t pair_count = 0;
    bool any_missing = false;

    const char *missing_symbols[MAX_WATCHLIST] = {0};
    size_t missing_count = 0;

    for (size_t i = 0; i < state->watchlist_count; i++) {
        coin_t *coin = &state->watchlist[i];
        const kraken_pair_map_t *map = map_symbol_to_pair(coin->symbol);
        if (!map) {
            any_missing = true;
            if (missing_count < MAX_WATCHLIST) {
                missing_symbols[missing_count++] = coin->symbol;
            }
            continue;
        }
        pairs[pair_count].request_pair = map->request_pair;
        pairs[pair_count].result_key = map->result_key;
        pairs[pair_count].coin = coin;
        pair_count++;
    }

    if (missing_count > 0 && !s_dynamic_pairs_inflight) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (s_dynamic_pairs_last_attempt_ms == 0 || (now_ms - s_dynamic_pairs_last_attempt_ms) > DYNAMIC_PAIR_RETRY_MS) {
            dynamic_pair_task_ctx_t *ctx = calloc(1, sizeof(*ctx));
            if (ctx) {
                ctx->count = missing_count;
                for (size_t i = 0; i < missing_count && i < MAX_WATCHLIST; i++) {
                    strncpy(ctx->symbols[i], missing_symbols[i], sizeof(ctx->symbols[i]) - 1);
                }
                s_dynamic_pairs_last_attempt_ms = now_ms;
                s_dynamic_pairs_inflight = true;
                xTaskCreate(dynamic_pairs_task, "kraken_pairs", 6144, ctx, 4, NULL);
            } else {
                s_dynamic_pairs_last_attempt_ms = now_ms;
            }
        }
    }

    if (pair_count == 0) {
        if (out_any_missing) {
            *out_any_missing = true;
        }
        return ESP_ERR_INVALID_ARG;
    }

    size_t csv_len = 0;
    for (size_t i = 0; i < pair_count; i++) {
        csv_len += strlen(pairs[i].request_pair) + 1;
    }

    char *csv = malloc(csv_len + 1);
    if (!csv) {
        return ESP_ERR_NO_MEM;
    }

    csv[0] = '\0';
    for (size_t i = 0; i < pair_count; i++) {
        if (i > 0) {
            strcat(csv, ",");
        }
        strcat(csv, pairs[i].request_pair);
    }

    char url[256];
    snprintf(url, sizeof(url), "%s%s?pair=%s", KRAKEN_BASE_URL, KRAKEN_TICKER_ENDPOINT, csv);
    free(csv);

    char *json = NULL;
    esp_err_t err = http_get_json(url, &json);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsArray(error) && cJSON_GetArraySize(error) > 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!result || !cJSON_IsObject(result)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    bool any_updated = false;

    for (size_t i = 0; i < pair_count; i++) {
        cJSON *entry = cJSON_GetObjectItem(result, pairs[i].result_key);
        if (!entry) {
            entry = cJSON_GetObjectItem(result, pairs[i].request_pair);
        }
        if (!entry) {
            any_missing = true;
            ESP_LOGW(TAG, "Missing mapping for: %s", pairs[i].request_pair);
            continue;
        }

        cJSON *last = cJSON_GetObjectItem(entry, "c");
        cJSON *open = cJSON_GetObjectItem(entry, "o");
        double price = 0.0;
        double open_value = 0.0;

        if (cJSON_IsArray(last) && cJSON_GetArraySize(last) > 0) {
            cJSON *last_item = cJSON_GetArrayItem(last, 0);
            json_to_double(last_item, &price);
        }
        json_to_double(open, &open_value);

        if (price > 0.0) {
            pairs[i].coin->price = price;
            if (open_value > 0.0) {
                pairs[i].coin->change_24h = ((price - open_value) / open_value) * 100.0;
            }
            any_updated = true;
            kraken_pair_entry_t *dyn = get_dynamic_entry(pairs[i].coin ? map_symbol_to_pair(pairs[i].coin->symbol) : NULL);
            if (dyn) {
                dyn->price_seen = true;
            }
        }
    }

    cJSON_Delete(root);

    if (out_any_missing) {
        *out_any_missing = any_missing;
    }

    return any_updated ? ESP_OK : ESP_FAIL;
}

