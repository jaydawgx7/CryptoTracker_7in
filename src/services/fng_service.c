#include "services/fng_service.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

static const char *TAG = "fng_service";

#ifndef CT_DEV_SKIP_TLS
#define CT_DEV_SKIP_TLS 0
#endif

#ifndef CT_CMC_API_KEY
#define CT_CMC_API_KEY ""
#endif

#define CMC_FNG_URL "https://pro-api.coinmarketcap.com/v3/fear-and-greed/latest"
#define ALTME_FNG_URL "https://api.alternative.me/fng/?limit=1"
#define CMC_GLOBAL_METRICS_URL "https://pro-api.coinmarketcap.com/v1/global-metrics/quotes/latest"
#define CMC_BTC_QUOTE_URL "https://pro-api.coinmarketcap.com/v2/cryptocurrency/quotes/latest?symbol=BTC"
#define CG_GLOBAL_URL "https://api.coingecko.com/api/v3/global"
#define CG_BTC_PRICE_URL "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd&include_24hr_change=true"
#define CMC_FNG_MIN_INTERVAL_S 3600
#define CMC_RATE_LIMIT_BACKOFF_S 3600
#define CMC_CONNECT_BACKOFF_S 600
#define CMC_GENERIC_BACKOFF_S 900
#define FNG_PERSIST_NAMESPACE "ct"
#define FNG_PERSIST_KEY "fng_cache_v1"
#define FNG_PERSIST_MAGIC 0x31474E46u

typedef struct {
    uint32_t magic;
    int64_t last_cmc_fng_attempt_s;
    int64_t cmc_rate_limit_until_s;
    fng_snapshot_t snapshot;
} fng_persist_blob_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buffer_t;

static fng_snapshot_t s_snapshot = {
    .has_value = false,
    .stale = false,
    .error = false,
    .using_fallback = false,
    .value = 0,
    .has_btc_dominance = false,
    .btc_dominance = 0.0,
    .has_eth_dominance = false,
    .eth_dominance = 0.0,
    .has_total_market_cap = false,
    .total_market_cap = 0.0,
    .total_market_cap_change_24h = 0.0,
    .has_btc_price = false,
    .btc_price = 0.0,
    .btc_price_change_24h = 0.0,
    .btc_dominance_change_24h = 0.0,
    .eth_dominance_change_24h = 0.0,
    .has_altcoin_season_index = false,
    .altcoin_season_index = 0,
    .classification = "N/A",
    .source_ts_s = 0,
    .source_age_s_at_fetch = 0,
    .fetched_at_s = 0,
    .last_error = ESP_OK,
};

static int64_t s_last_attempt_s = 0;
static int64_t s_last_cmc_fng_attempt_s = 0;
static int64_t s_cmc_rate_limit_until_s = 0;
static uint32_t s_cmc_fng_attempt_count = 0;

static void persist_runtime_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FNG_PERSIST_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist open failed: %s", esp_err_to_name(err));
        return;
    }

    fng_persist_blob_t blob = {
        .magic = FNG_PERSIST_MAGIC,
        .last_cmc_fng_attempt_s = s_last_cmc_fng_attempt_s,
        .cmc_rate_limit_until_s = s_cmc_rate_limit_until_s,
        .snapshot = s_snapshot,
    };

    err = nvs_set_blob(handle, FNG_PERSIST_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Persist save failed: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
}

static void load_persisted_runtime_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(FNG_PERSIST_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return;
    }

    fng_persist_blob_t blob = {0};
    size_t blob_len = sizeof(blob);
    err = nvs_get_blob(handle, FNG_PERSIST_KEY, &blob, &blob_len);
    nvs_close(handle);
    if (err != ESP_OK || blob_len != sizeof(blob) || blob.magic != FNG_PERSIST_MAGIC) {
        return;
    }

    s_last_cmc_fng_attempt_s = blob.last_cmc_fng_attempt_s;
    s_cmc_rate_limit_until_s = blob.cmc_rate_limit_until_s;
    s_snapshot = blob.snapshot;
    ESP_LOGI(TAG,
             "Loaded persisted FNG state: has_value=%d fallback=%d last_attempt=%lld backoff_until=%lld",
             s_snapshot.has_value,
             s_snapshot.using_fallback,
             (long long)s_last_cmc_fng_attempt_s,
             (long long)s_cmc_rate_limit_until_s);
}

static const char *classification_from_value(int value)
{
    if (value < 20) {
        return "Extreme Fear";
    }
    if (value < 40) {
        return "Fear";
    }
    if (value < 60) {
        return "Neutral";
    }
    if (value < 80) {
        return "Greed";
    }
    return "Extreme Greed";
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
            size_t new_cap = buffer->cap == 0 ? 1024 : buffer->cap * 2;
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

static bool digits_only(const char *s)
{
    if (!s || s[0] == '\0') {
        return false;
    }

    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static int64_t parse_iso8601_utc_s(const char *s)
{
    if (!s || strlen(s) < 19) {
        return 0;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    int matched = sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second);
    if (matched != 6) {
        return 0;
    }

    int y = year;
    unsigned m = (unsigned)month;
    unsigned d = (unsigned)day;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;

    return days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

static int64_t parse_epoch_field(const cJSON *field, int64_t fallback)
{
    if (cJSON_IsNumber(field)) {
        int64_t v = (int64_t)field->valuedouble;
        if (v > 0) {
            return v;
        }
    }

    if (cJSON_IsString(field) && field->valuestring && digits_only(field->valuestring)) {
        int64_t v = atoll(field->valuestring);
        if (v > 0) {
            return v;
        }
    }

    if (cJSON_IsString(field) && field->valuestring) {
        int64_t v = parse_iso8601_utc_s(field->valuestring);
        if (v > 0) {
            return v;
        }
    }

    return fallback;
}

static void log_cmc_error_body(const char *json)
{
    if (!json || json[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "CMC error body (raw): %.160s", json);
        return;
    }

    cJSON *status_obj = cJSON_GetObjectItem(root, "status");
    cJSON *error_code = cJSON_IsObject(status_obj) ? cJSON_GetObjectItem(status_obj, "error_code") : NULL;
    cJSON *error_message = cJSON_IsObject(status_obj) ? cJSON_GetObjectItem(status_obj, "error_message") : NULL;
    cJSON *credit_count = cJSON_IsObject(status_obj) ? cJSON_GetObjectItem(status_obj, "credit_count") : NULL;

    if (cJSON_IsNumber(error_code) || cJSON_IsString(error_message)) {
        int code = cJSON_IsNumber(error_code) ? (int)error_code->valuedouble : -1;
        const char *message = cJSON_IsString(error_message) && error_message->valuestring ?
                              error_message->valuestring : "(none)";
        int credits = cJSON_IsNumber(credit_count) ? (int)credit_count->valuedouble : -1;
        if (credits >= 0) {
            ESP_LOGW(TAG, "CMC error detail: code=%d message=%s credits=%d", code, message, credits);
        } else {
            ESP_LOGW(TAG, "CMC error detail: code=%d message=%s", code, message);
        }
    } else {
        ESP_LOGW(TAG, "CMC error body (raw): %.160s", json);
    }

    cJSON_Delete(root);
}

static esp_err_t http_get_json_with_key(const char *url, const char *api_key, char **out)
{
    if (!url || !out || !api_key || api_key[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    http_buffer_t buffer = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 10000,
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

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "X-CMC_PRO_API_KEY", api_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "HTTP status=%d", status);
            log_cmc_error_body(buffer.buf);
            if (status == 429) {
                err = ESP_ERR_TIMEOUT;
            } else {
                err = ESP_FAIL;
            }
        }

        free(buffer.buf);
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_cleanup(client);
    *out = buffer.buf;
    return ESP_OK;
}

static esp_err_t http_get_json(const char *url, char **out)
{
    if (!url || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    http_buffer_t buffer = {0};
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &buffer,
        .timeout_ms = 10000,
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

    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err != ESP_OK || status != 200) {
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "HTTP GET failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "HTTP status=%d", status);
            if (status == 429) {
                err = ESP_ERR_TIMEOUT;
            } else {
                err = ESP_FAIL;
            }
        }

        free(buffer.buf);
        esp_http_client_cleanup(client);
        return err;
    }

    esp_http_client_cleanup(client);
    *out = buffer.buf;
    return ESP_OK;
}

static esp_err_t parse_cmc_fng(const char *json,
                               int *out_value,
                               char *out_classification,
                               size_t out_classification_len,
                               int64_t *out_source_ts_s,
                               int64_t *out_source_age_s_at_fetch)
{
    if (!json || !out_value || !out_classification || out_classification_len == 0 ||
        !out_source_ts_s || !out_source_age_s_at_fetch) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *value = cJSON_GetObjectItem(data, "value");

    int parsed_value = 0;
    if (cJSON_IsString(value) && value->valuestring) {
        parsed_value = atoi(value->valuestring);
    } else if (cJSON_IsNumber(value)) {
        parsed_value = (int)value->valuedouble;
    } else {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (parsed_value < 0) {
        parsed_value = 0;
    } else if (parsed_value > 100) {
        parsed_value = 100;
    }

    cJSON *classification = cJSON_GetObjectItem(data, "value_classification");

    int64_t now_s = esp_timer_get_time() / 1000000;
    cJSON *timestamp = cJSON_GetObjectItem(data, "update_time");
    int64_t source_ts_s = parse_epoch_field(timestamp, 0);

    int64_t status_ts_s = 0;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (cJSON_IsObject(status)) {
        cJSON *status_timestamp = cJSON_GetObjectItem(status, "timestamp");
        status_ts_s = parse_epoch_field(status_timestamp, 0);
    }

    if (source_ts_s <= 0) {
        source_ts_s = (status_ts_s > 0) ? status_ts_s : now_s;
    }

    int64_t source_age_s_at_fetch = 0;
    if (status_ts_s > 0 && source_ts_s > 0 && status_ts_s >= source_ts_s) {
        source_age_s_at_fetch = status_ts_s - source_ts_s;
    }

    if (!timestamp) {
        timestamp = cJSON_GetObjectItem(data, "update_time");
    }

    *out_source_ts_s = source_ts_s;
    *out_source_age_s_at_fetch = source_age_s_at_fetch;
    *out_value = parsed_value;

    if (cJSON_IsString(classification) && classification->valuestring && classification->valuestring[0] != '\0') {
        strncpy(out_classification, classification->valuestring, out_classification_len - 1);
        out_classification[out_classification_len - 1] = '\0';
    } else {
        strncpy(out_classification, classification_from_value(parsed_value), out_classification_len - 1);
        out_classification[out_classification_len - 1] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_altme_fng(const char *json,
                                 int *out_value,
                                 char *out_classification,
                                 size_t out_classification_len,
                                 int64_t *out_source_ts_s,
                                 int64_t *out_source_age_s_at_fetch)
{
    if (!json || !out_value || !out_classification || out_classification_len == 0 ||
        !out_source_ts_s || !out_source_age_s_at_fetch) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *entry = (cJSON_IsArray(data) && cJSON_GetArraySize(data) > 0) ? cJSON_GetArrayItem(data, 0) : NULL;
    if (!cJSON_IsObject(entry)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *value = cJSON_GetObjectItem(entry, "value");
    cJSON *classification = cJSON_GetObjectItem(entry, "value_classification");
    cJSON *timestamp = cJSON_GetObjectItem(entry, "timestamp");

    int parsed_value = 0;
    if (cJSON_IsNumber(value)) {
        parsed_value = (int)value->valuedouble;
    } else if (cJSON_IsString(value) && value->valuestring) {
        parsed_value = atoi(value->valuestring);
    } else {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    if (parsed_value < 0) {
        parsed_value = 0;
    } else if (parsed_value > 100) {
        parsed_value = 100;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    int64_t source_ts_s = parse_epoch_field(timestamp, now_s);
    int64_t age = (source_ts_s > 0 && now_s >= source_ts_s) ? (now_s - source_ts_s) : 0;

    *out_value = parsed_value;
    *out_source_ts_s = source_ts_s;
    *out_source_age_s_at_fetch = age;

    if (cJSON_IsString(classification) && classification->valuestring && classification->valuestring[0] != '\0') {
        strncpy(out_classification, classification->valuestring, out_classification_len - 1);
        out_classification[out_classification_len - 1] = '\0';
    } else {
        strncpy(out_classification, classification_from_value(parsed_value), out_classification_len - 1);
        out_classification[out_classification_len - 1] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_cg_global_metrics(const char *json,
                                         double *out_btc_dom,
                                         double *out_eth_dom,
                                         double *out_total_market_cap,
                                         double *out_total_market_cap_change_24h)
{
    if (!json || !out_btc_dom || !out_eth_dom || !out_total_market_cap || !out_total_market_cap_change_24h) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *mcap_pct = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "market_cap_percentage") : NULL;
    cJSON *mcap = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "total_market_cap") : NULL;
    cJSON *btc = cJSON_IsObject(mcap_pct) ? cJSON_GetObjectItem(mcap_pct, "btc") : NULL;
    cJSON *eth = cJSON_IsObject(mcap_pct) ? cJSON_GetObjectItem(mcap_pct, "eth") : NULL;
    cJSON *usd_mcap = cJSON_IsObject(mcap) ? cJSON_GetObjectItem(mcap, "usd") : NULL;
    cJSON *chg = cJSON_IsObject(data) ? cJSON_GetObjectItem(data, "market_cap_change_percentage_24h_usd") : NULL;

    if (!cJSON_IsNumber(btc) || !cJSON_IsNumber(eth) || !cJSON_IsNumber(usd_mcap) || !cJSON_IsNumber(chg)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_btc_dom = btc->valuedouble;
    *out_eth_dom = eth->valuedouble;
    *out_total_market_cap = usd_mcap->valuedouble;
    *out_total_market_cap_change_24h = chg->valuedouble;

    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t parse_cg_btc_quote(const char *json, double *out_price, double *out_change_24h)
{
    if (!json || !out_price || !out_change_24h) {
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *btc = cJSON_GetObjectItem(root, "bitcoin");
    cJSON *usd = cJSON_IsObject(btc) ? cJSON_GetObjectItem(btc, "usd") : NULL;
    cJSON *chg = cJSON_IsObject(btc) ? cJSON_GetObjectItem(btc, "usd_24h_change") : NULL;

    if (!cJSON_IsNumber(usd) || !cJSON_IsNumber(chg)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *out_price = usd->valuedouble;
    *out_change_24h = chg->valuedouble;

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t fng_service_init(void)
{
    s_last_attempt_s = 0;
    s_last_cmc_fng_attempt_s = 0;
    s_cmc_rate_limit_until_s = 0;
    s_cmc_fng_attempt_count = 0;
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.last_error = ESP_OK;
    strncpy(s_snapshot.classification, "N/A", sizeof(s_snapshot.classification) - 1);
    load_persisted_runtime_state();
    return ESP_OK;
}

esp_err_t fng_service_refresh(uint32_t min_interval_s)
{
    if (min_interval_s == 0) {
        min_interval_s = 1;
    }

    int64_t now_s = esp_timer_get_time() / 1000000;
    if (s_last_attempt_s > 0 && (now_s - s_last_attempt_s) < (int64_t)min_interval_s) {
        return ESP_ERR_INVALID_STATE;
    }
    s_last_attempt_s = now_s;

    char *fng_json = NULL;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    bool fng_attempted = false;

    if (CT_CMC_API_KEY[0] != '\0') {
        bool interval_ok = (s_last_cmc_fng_attempt_s == 0) ||
                           ((now_s - s_last_cmc_fng_attempt_s) >= CMC_FNG_MIN_INTERVAL_S);
        bool rate_limit_ok = (s_cmc_rate_limit_until_s == 0) || (now_s >= s_cmc_rate_limit_until_s);
        if (interval_ok && rate_limit_ok) {
            fng_attempted = true;
            s_last_cmc_fng_attempt_s = now_s;
            s_cmc_fng_attempt_count++;
            ESP_LOGI(TAG, "CMC FNG attempt #%lu", (unsigned long)s_cmc_fng_attempt_count);
            err = http_get_json_with_key(CMC_FNG_URL, CT_CMC_API_KEY, &fng_json);
            if (err == ESP_OK) {
                s_cmc_rate_limit_until_s = 0;
            } else if (err == ESP_ERR_TIMEOUT) {
                s_cmc_rate_limit_until_s = now_s + CMC_RATE_LIMIT_BACKOFF_S;
            } else if (err == ESP_ERR_HTTP_CONNECT || err == ESP_ERR_HTTP_EAGAIN) {
                s_cmc_rate_limit_until_s = now_s + CMC_CONNECT_BACKOFF_S;
            } else {
                s_cmc_rate_limit_until_s = now_s + CMC_GENERIC_BACKOFF_S;
            }
            persist_runtime_state();
        } else {
            if (!interval_ok) {
                ESP_LOGD(TAG,
                         "Skipping CMC FNG fetch: interval gate active (%llds remaining)",
                         (long long)(CMC_FNG_MIN_INTERVAL_S - (now_s - s_last_cmc_fng_attempt_s)));
            }
            if (!rate_limit_ok) {
                ESP_LOGW(TAG,
                         "Skipping CMC FNG fetch: backoff active (%llds remaining)",
                         (long long)(s_cmc_rate_limit_until_s - now_s));
            }
        }
    }

    int fng_value = 0;
    int64_t source_ts_s = now_s;
    int64_t source_age_s_at_fetch = 0;
    char classification[24] = {0};
    bool got_fng = false;
    bool used_fallback = false;
    if (fng_json) {
        err = parse_cmc_fng(fng_json, &fng_value, classification, sizeof(classification), &source_ts_s, &source_age_s_at_fetch);
        free(fng_json);
        got_fng = (err == ESP_OK);
    }

    if (!got_fng && (fng_attempted || CT_CMC_API_KEY[0] == '\0')) {
        char *alt_json = NULL;
        esp_err_t alt_err = http_get_json(ALTME_FNG_URL, &alt_json);
        if (alt_err == ESP_OK) {
            alt_err = parse_altme_fng(alt_json,
                                      &fng_value,
                                      classification,
                                      sizeof(classification),
                                      &source_ts_s,
                                      &source_age_s_at_fetch);
        }
        free(alt_json);
        if (alt_err == ESP_OK) {
            got_fng = true;
            used_fallback = true;
            ESP_LOGW(TAG, "Using alternative.me fallback for FNG");
        }
    }

    if (got_fng) {
        s_snapshot.has_value = true;
        s_snapshot.value = fng_value;
        s_snapshot.source_ts_s = source_ts_s;
        s_snapshot.source_age_s_at_fetch = source_age_s_at_fetch;
        s_snapshot.fetched_at_s = now_s;
        strncpy(s_snapshot.classification, classification, sizeof(s_snapshot.classification) - 1);
        s_snapshot.classification[sizeof(s_snapshot.classification) - 1] = '\0';
        s_snapshot.stale = false;
        s_snapshot.error = false;
        s_snapshot.using_fallback = used_fallback;
        s_snapshot.last_error = ESP_OK;
        persist_runtime_state();
    } else if (fng_attempted) {
        s_snapshot.has_value = false;
        s_snapshot.value = 0;
        s_snapshot.source_ts_s = 0;
        s_snapshot.source_age_s_at_fetch = 0;
        s_snapshot.fetched_at_s = 0;
        s_snapshot.error = true;
        s_snapshot.using_fallback = false;
        s_snapshot.stale = false;
        strncpy(s_snapshot.classification, "N/A", sizeof(s_snapshot.classification) - 1);
        s_snapshot.classification[sizeof(s_snapshot.classification) - 1] = '\0';
        s_snapshot.last_error = err;
        persist_runtime_state();
    }

    char *global_json = NULL;
    double btc_dom = 0.0;
    double eth_dom = 0.0;
    double total_market_cap = 0.0;
    double total_market_cap_change_24h = 0.0;
    double btc_dom_chg = 0.0;
    double eth_dom_chg = 0.0;
    esp_err_t global_err = http_get_json(CG_GLOBAL_URL, &global_json);
    if (global_err == ESP_OK) {
        esp_err_t parse_err = parse_cg_global_metrics(global_json,
                                                      &btc_dom,
                                                      &eth_dom,
                                                      &total_market_cap,
                                                      &total_market_cap_change_24h);
        free(global_json);
        global_json = NULL;
        if (parse_err == ESP_OK) {
            btc_dom_chg = 0.0;
            eth_dom_chg = 0.0;
        } else {
            global_err = parse_err;
        }
    }

    if (global_err == ESP_OK) {
        s_snapshot.has_btc_dominance = true;
        s_snapshot.btc_dominance = btc_dom;
        s_snapshot.has_eth_dominance = true;
        s_snapshot.eth_dominance = eth_dom;
        s_snapshot.has_total_market_cap = true;
        s_snapshot.total_market_cap = total_market_cap;
        s_snapshot.total_market_cap_change_24h = total_market_cap_change_24h;
        s_snapshot.btc_dominance_change_24h = btc_dom_chg;
        s_snapshot.eth_dominance_change_24h = eth_dom_chg;
    } else {
        s_snapshot.has_btc_dominance = false;
        s_snapshot.btc_dominance = 0.0;
        s_snapshot.has_eth_dominance = false;
        s_snapshot.eth_dominance = 0.0;
        s_snapshot.has_total_market_cap = false;
        s_snapshot.total_market_cap = 0.0;
        s_snapshot.total_market_cap_change_24h = 0.0;
        s_snapshot.btc_dominance_change_24h = 0.0;
        s_snapshot.eth_dominance_change_24h = 0.0;
    }

    char *btc_json = NULL;
    double btc_price = 0.0;
    double btc_price_change_24h = 0.0;
    esp_err_t btc_err = http_get_json(CG_BTC_PRICE_URL, &btc_json);
    if (btc_err == ESP_OK) {
        esp_err_t parse_err = parse_cg_btc_quote(btc_json, &btc_price, &btc_price_change_24h);
        free(btc_json);
        btc_json = NULL;
        if (parse_err != ESP_OK) {
            btc_err = parse_err;
        }
    }

    if (btc_err == ESP_OK) {
        s_snapshot.has_btc_price = true;
        s_snapshot.btc_price = btc_price;
        s_snapshot.btc_price_change_24h = btc_price_change_24h;
    } else {
        s_snapshot.has_btc_price = false;
        s_snapshot.btc_price = 0.0;
        s_snapshot.btc_price_change_24h = 0.0;
    }

    s_snapshot.has_altcoin_season_index = false;
    s_snapshot.altcoin_season_index = 0;

    if (got_fng || global_err == ESP_OK || btc_err == ESP_OK) {
        return ESP_OK;
    }

    return err;
}

void fng_service_get_snapshot(fng_snapshot_t *out)
{
    if (!out) {
        return;
    }
    *out = s_snapshot;
}
