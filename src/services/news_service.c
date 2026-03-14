#include "services/news_service.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define NEWS_RSS_URL "https://cointelegraph.com/rss"
#define NEWS_HTTP_RETRY_COUNT 2
#define NEWS_REQUEST_TIMEOUT_MS 12000
#define NEWS_REFRESH_TASK_STACK_SIZE 7168
#define NEWS_RAW_DESC_CAP 1600
#define NEWS_REFRESH_TASK_CORE 0

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buffer_t;

static const char *TAG = "news_service";
static SemaphoreHandle_t s_news_mutex = NULL;
static news_snapshot_t s_snapshot = {0};
static TaskHandle_t s_news_task = NULL;
static StaticTask_t s_news_task_buf;
static StackType_t s_news_task_stack[NEWS_REFRESH_TASK_STACK_SIZE / sizeof(StackType_t)];

static void trim_in_place(char *text)
{
    if (!text) {
        return;
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[--len] = '\0';
    }

    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
}

static void decode_entities_in_place(char *text)
{
    if (!text) {
        return;
    }

    char *src = text;
    char *dst = text;
    while (*src) {
        if (strncmp(src, "&amp;", 5) == 0) {
            *dst++ = '&';
            src += 5;
        } else if (strncmp(src, "&quot;", 6) == 0) {
            *dst++ = '"';
            src += 6;
        } else if (strncmp(src, "&#39;", 5) == 0 || strncmp(src, "&apos;", 6) == 0) {
            *dst++ = '\'';
            src += (src[1] == '#') ? 5 : 6;
        } else if (strncmp(src, "&lt;", 4) == 0) {
            *dst++ = '<';
            src += 4;
        } else if (strncmp(src, "&gt;", 4) == 0) {
            *dst++ = '>';
            src += 4;
        } else if (strncmp(src, "&#8217;", 7) == 0 || strncmp(src, "&#8216;", 7) == 0) {
            *dst++ = '\'';
            src += 7;
        } else if (strncmp(src, "&#8220;", 7) == 0 || strncmp(src, "&#8221;", 7) == 0) {
            *dst++ = '"';
            src += 7;
        } else if (strncmp(src, "&#8211;", 7) == 0 || strncmp(src, "&#8212;", 7) == 0) {
            *dst++ = '-';
            src += 7;
        } else if (strncmp(src, "&#038;", 6) == 0) {
            *dst++ = '&';
            src += 6;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void normalize_whitespace_in_place(char *text)
{
    if (!text) {
        return;
    }

    char *src = text;
    char *dst = text;
    bool last_space = true;
    while (*src) {
        char c = *src++;
        bool is_space = isspace((unsigned char)c) != 0;
        if (is_space) {
            if (!last_space) {
                *dst++ = ' ';
                last_space = true;
            }
        } else {
            *dst++ = c;
            last_space = false;
        }
    }
    if (dst > text && dst[-1] == ' ') {
        dst--;
    }
    *dst = '\0';
}

static void normalize_utf8_punctuation_in_place(char *text)
{
    if (!text) {
        return;
    }

    unsigned char *src = (unsigned char *)text;
    char *dst = text;
    while (*src) {
        if (src[0] == 0xE2 && src[1] == 0x80) {
            unsigned char third = src[2];
            if (third == 0x98 || third == 0x99) {
                *dst++ = '\'';
                src += 3;
                continue;
            }
            if (third == 0x9C || third == 0x9D) {
                *dst++ = '"';
                src += 3;
                continue;
            }
            if (third == 0x93 || third == 0x94 || third == 0x95) {
                *dst++ = '-';
                src += 3;
                continue;
            }
            if (third == 0xA2) {
                *dst++ = '-';
                src += 3;
                continue;
            }
            if (third == 0xA6) {
                *dst++ = '.';
                *dst++ = '.';
                *dst++ = '.';
                src += 3;
                continue;
            }
        }

        if (src[0] == 0xC2 && src[1] == 0xA0) {
            *dst++ = ' ';
            src += 2;
            continue;
        }

        *dst++ = (char)*src++;
    }

    *dst = '\0';
}

static void strip_html_in_place(char *text)
{
    if (!text) {
        return;
    }

    char *src = text;
    char *dst = text;
    bool in_tag = false;
    while (*src) {
        char c = *src++;
        if (c == '<') {
            in_tag = true;
            continue;
        }
        if (c == '>') {
            in_tag = false;
            *dst++ = ' ';
            continue;
        }
        if (!in_tag) {
            *dst++ = c;
        }
    }
    *dst = '\0';
    decode_entities_in_place(text);
    normalize_utf8_punctuation_in_place(text);
    normalize_whitespace_in_place(text);
    trim_in_place(text);
}

static bool copy_tag_value_bounded(const char *start, const char *limit,
                                   const char *open_tag, const char *close_tag,
                                   char *out, size_t out_len)
{
    if (!start || !limit || !open_tag || !close_tag || !out || out_len == 0) {
        return false;
    }

    const char *open = strstr(start, open_tag);
    if (!open || open >= limit) {
        out[0] = '\0';
        return false;
    }
    open += strlen(open_tag);

    const char *close = strstr(open, close_tag);
    if (!close || close > limit) {
        out[0] = '\0';
        return false;
    }

    size_t copy_len = (size_t)(close - open);
    if (copy_len >= out_len) {
        copy_len = out_len - 1;
    }
    memcpy(out, open, copy_len);
    out[copy_len] = '\0';
    trim_in_place(out);

    if (strncmp(out, "<![CDATA[", 9) == 0) {
        size_t len = strlen(out);
        if (len >= 12 && strcmp(out + len - 3, "]]>") == 0) {
            memmove(out, out + 9, len - 12 + 1);
            out[len - 12] = '\0';
        }
    }

    trim_in_place(out);
    return out[0] != '\0';
}

static void sanitize_link_in_place(char *link)
{
    trim_in_place(link);
    char *q = strstr(link, "?utm_");
    if (q) {
        *q = '\0';
    }
    normalize_whitespace_in_place(link);
}

static void extract_summary(const char *raw_description, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!raw_description || raw_description[0] == '\0') {
        return;
    }

    const char *scan = raw_description;
    size_t used = 0;
    while (1) {
        const char *p = strstr(scan, "<p");
        if (!p) {
            break;
        }
        const char *gt = strchr(p, '>');
        const char *end = gt ? strstr(gt + 1, "</p>") : NULL;
        if (!gt || !end) {
            break;
        }

        if (strstr(p, "<img") && strstr(p, "</p>") == end) {
            scan = end + 4;
            continue;
        }

        char paragraph[512] = {0};
        size_t copy_len = (size_t)(end - (gt + 1));
        if (copy_len >= sizeof(paragraph)) {
            copy_len = sizeof(paragraph) - 1;
        }
        memcpy(paragraph, gt + 1, copy_len);
        paragraph[copy_len] = '\0';
        strip_html_in_place(paragraph);
        if (paragraph[0] != '\0') {
            size_t paragraph_len = strlen(paragraph);
            size_t separator_len = used > 0 ? 2 : 0;
            if (used + separator_len >= out_len - 1) {
                break;
            }

            size_t remaining = (out_len - 1) - used - separator_len;
            if (paragraph_len > remaining) {
                paragraph_len = remaining;
            }

            if (separator_len == 2) {
                out[used++] = '\n';
                out[used++] = '\n';
            }
            memcpy(out + used, paragraph, paragraph_len);
            used += paragraph_len;
            out[used] = '\0';

            if (paragraph_len < strlen(paragraph)) {
                break;
            }
        }
        scan = end + 4;
    }

    if (out[0] != '\0') {
        return;
    }

    strncpy(out, raw_description, out_len - 1);
    out[out_len - 1] = '\0';
    strip_html_in_place(out);
}

static void simplify_author_in_place(char *author)
{
    if (!author) {
        return;
    }
    const char *prefix = "Cointelegraph by ";
    if (strncmp(author, prefix, strlen(prefix)) == 0) {
        memmove(author, author + strlen(prefix), strlen(author + strlen(prefix)) + 1);
    }
    trim_in_place(author);
}

static void format_pub_label(const char *pub_date, char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!pub_date) {
        return;
    }

    char weekday[4] = {0};
    char month[4] = {0};
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(pub_date, "%3[^,], %d %3s %d %d:%d:%d", weekday, &day, month, &year, &hour, &minute, &second) == 7) {
        snprintf(out, out_len, "%s %d %02d:%02d UTC", month, day, hour, minute);
        return;
    }

    strncpy(out, pub_date, out_len - 1);
    out[out_len - 1] = '\0';
    trim_in_place(out);
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
            size_t new_cap = buffer->cap == 0 ? 4096 : buffer->cap * 2;
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

static esp_err_t http_get_text(const char *url, char **out)
{
    if (!url || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = NULL;
    esp_err_t last_err = ESP_FAIL;
    for (int attempt = 0; attempt < NEWS_HTTP_RETRY_COUNT; attempt++) {
        http_buffer_t buffer = {0};
        esp_http_client_config_t config = {
            .url = url,
            .event_handler = http_event_handler,
            .user_data = &buffer,
            .timeout_ms = NEWS_REQUEST_TIMEOUT_MS,
            .keep_alive_enable = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            return ESP_FAIL;
        }

        esp_http_client_set_header(client, "Accept", "application/rss+xml, application/xml, text/xml");
        esp_http_client_set_header(client, "User-Agent", "CryptoTracker_7in");

        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        if (err == ESP_OK && status == 200) {
            esp_http_client_cleanup(client);
            *out = buffer.buf;
            return ESP_OK;
        }

        last_err = (err != ESP_OK) ? err : (status == 429 ? ESP_ERR_TIMEOUT : ESP_FAIL);
        free(buffer.buf);
        esp_http_client_cleanup(client);
        if (status == 429) {
            return last_err;
        }
    }

    return last_err;
}

static esp_err_t parse_feed(const char *xml, news_snapshot_t *parsed)
{
    if (!xml || !parsed) {
        return ESP_ERR_INVALID_ARG;
    }

    char *raw_desc = calloc(1, NEWS_RAW_DESC_CAP);
    if (!raw_desc) {
        return ESP_ERR_NO_MEM;
    }

    memset(parsed, 0, sizeof(*parsed));
    parsed->last_error = ESP_OK;

    const char *cursor = xml;
    while (parsed->count < NEWS_SERVICE_MAX_ITEMS) {
        const char *item_start = strstr(cursor, "<item>");
        if (!item_start) {
            break;
        }
        const char *item_end = strstr(item_start, "</item>");
        if (!item_end) {
            break;
        }

        news_item_t *item = &parsed->items[parsed->count];
        char raw_pub[96] = {0};

    raw_desc[0] = '\0';

        copy_tag_value_bounded(item_start, item_end, "<title>", "</title>", item->title, sizeof(item->title));
        copy_tag_value_bounded(item_start, item_end, "<link>", "</link>", item->link, sizeof(item->link));
        copy_tag_value_bounded(item_start, item_end, "<dc:creator>", "</dc:creator>", item->author, sizeof(item->author));
        copy_tag_value_bounded(item_start, item_end, "<description>", "</description>", raw_desc, NEWS_RAW_DESC_CAP);
        copy_tag_value_bounded(item_start, item_end, "<pubDate>", "</pubDate>", raw_pub, sizeof(raw_pub));

        decode_entities_in_place(item->title);
        normalize_utf8_punctuation_in_place(item->title);
        trim_in_place(item->title);
        sanitize_link_in_place(item->link);
        normalize_utf8_punctuation_in_place(item->author);
        simplify_author_in_place(item->author);
        extract_summary(raw_desc, item->summary, sizeof(item->summary));
        format_pub_label(raw_pub, item->published_label, sizeof(item->published_label));

        if (item->title[0] != '\0' && item->link[0] != '\0') {
            parsed->count++;
        }

        cursor = item_end + 7;
    }

    if (parsed->count == 0) {
        free(raw_desc);
        return ESP_ERR_NOT_FOUND;
    }

    parsed->has_cache = true;
    parsed->fetched_at_s = esp_timer_get_time() / 1000000;
    free(raw_desc);
    return ESP_OK;
}

static void news_refresh_task(void *arg)
{
    (void)arg;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        char *xml = NULL;
        news_snapshot_t *parsed = calloc(1, sizeof(*parsed));
        if (!parsed) {
            if (xSemaphoreTake(s_news_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                s_snapshot.loading = false;
                s_snapshot.last_error = ESP_ERR_NO_MEM;
                xSemaphoreGive(s_news_mutex);
            }
            ESP_LOGE(TAG, "News refresh failed: no memory for parsed snapshot");
            continue;
        }

        esp_err_t err = http_get_text(NEWS_RSS_URL, &xml);
        if (err == ESP_OK) {
            err = parse_feed(xml, parsed);
        }
        free(xml);

        if (xSemaphoreTake(s_news_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
            if (err == ESP_OK) {
                parsed->loading = false;
                s_snapshot = *parsed;
            } else {
                s_snapshot.loading = false;
                s_snapshot.last_error = err;
            }
            xSemaphoreGive(s_news_mutex);
        }

        UBaseType_t stack_high_water = uxTaskGetStackHighWaterMark(NULL);
        ESP_LOGI(TAG, "News refresh %s stack_hw=%u", err == ESP_OK ? "ok" : esp_err_to_name(err),
                 (unsigned)stack_high_water);
        free(parsed);
    }
}

esp_err_t news_service_init(void)
{
    if (s_news_mutex) {
        return ESP_OK;
    }

    s_news_mutex = xSemaphoreCreateMutex();
    if (!s_news_mutex) {
        return ESP_ERR_NO_MEM;
    }

    s_snapshot.last_error = ESP_FAIL;

    s_news_task = xTaskCreateStaticPinnedToCore(
        news_refresh_task,
        "news_refresh",
        (uint32_t)(sizeof(s_news_task_stack) / sizeof(StackType_t)),
        NULL,
        4,
        s_news_task_stack,
        &s_news_task_buf,
        NEWS_REFRESH_TASK_CORE);
    if (!s_news_task) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void news_service_get_snapshot(news_snapshot_t *out)
{
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (!s_news_mutex) {
        out->last_error = ESP_ERR_INVALID_STATE;
        return;
    }

    if (xSemaphoreTake(s_news_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        *out = s_snapshot;
        xSemaphoreGive(s_news_mutex);
    } else {
        out->last_error = ESP_ERR_TIMEOUT;
    }
}

bool news_service_request_refresh(void)
{
    if (!s_news_mutex || !s_news_task) {
        return false;
    }

    bool start_task = false;
    if (xSemaphoreTake(s_news_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
        if (!s_snapshot.loading) {
            s_snapshot.loading = true;
            s_snapshot.last_error = ESP_OK;
            start_task = true;
        }
        xSemaphoreGive(s_news_mutex);
    }

    if (!start_task) {
        return false;
    }

    xTaskNotifyGive(s_news_task);

    return true;
}
