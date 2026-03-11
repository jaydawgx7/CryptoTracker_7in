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

typedef enum {
    DATA_SOURCE_COINGECKO = 0,
    DATA_SOURCE_KRAKEN
} data_source_t;

typedef enum {
    BUTTON_SHADOW_MINIMAL = 0,
    BUTTON_SHADOW_MEDIUM = 1,
    BUTTON_SHADOW_MAXIMUM = 2
} button_shadow_strength_t;

typedef struct {
    bool dark_mode;
    bool buttons_3d;
    sort_field_t sort_field;
    bool sort_desc;
    bool show_values;
    uint16_t refresh_seconds;
    uint8_t brightness;
    bool buzzer_enabled;
    uint32_t accent_hex;
    uint32_t shadow_hex;
    button_shadow_strength_t button_shadow_strength;
    data_source_t data_source;
    bool demo_portfolio;
} ui_prefs_t;

typedef struct app_state_t {
    coin_t watchlist[MAX_WATCHLIST];
    size_t watchlist_count;
    ui_prefs_t prefs;
    bool needs_restore;
} app_state_t;
