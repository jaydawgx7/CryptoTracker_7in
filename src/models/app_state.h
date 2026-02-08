#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "models/coin.h"

#define MAX_WATCHLIST 64
#define MAX_WIFI_NETWORKS 5

typedef enum {
    SORT_SYMBOL = 0,
    SORT_PRICE,
    SORT_1H,
    SORT_24H,
    SORT_7D,
    SORT_VALUE
} sort_field_t;

typedef struct {
    bool dark_mode;
    sort_field_t sort_field;
    bool sort_desc;
    uint16_t refresh_seconds;
    uint8_t brightness;
    bool buzzer_enabled;
} ui_prefs_t;

typedef struct {
    coin_t watchlist[MAX_WATCHLIST];
    size_t watchlist_count;
    ui_prefs_t prefs;
} app_state_t;
