#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define NEWS_SERVICE_MAX_ITEMS 6

typedef struct {
    char title[192];
    char link[320];
    char author[80];
    char summary[384];
    char published_label[40];
} news_item_t;

typedef struct {
    bool loading;
    bool has_cache;
    esp_err_t last_error;
    int64_t fetched_at_s;
    size_t count;
    news_item_t items[NEWS_SERVICE_MAX_ITEMS];
} news_snapshot_t;

esp_err_t news_service_init(void);
void news_service_get_snapshot(news_snapshot_t *out);
bool news_service_request_refresh(void);
