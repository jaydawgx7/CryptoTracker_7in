#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool has_value;
    bool stale;
    bool error;
    bool using_fallback;
    int value;
    bool has_btc_dominance;
    double btc_dominance;
    bool has_eth_dominance;
    double eth_dominance;
    bool has_total_market_cap;
    double total_market_cap;
    double total_market_cap_change_24h;
    bool has_btc_price;
    double btc_price;
    double btc_price_change_24h;
    double btc_dominance_change_24h;
    double eth_dominance_change_24h;
    bool has_altcoin_season_index;
    int altcoin_season_index;
    char classification[24];
    int64_t source_ts_s;
    int64_t source_age_s_at_fetch;
    int64_t fetched_at_s;
    esp_err_t last_error;
} fng_snapshot_t;

esp_err_t fng_service_init(void);
esp_err_t fng_service_refresh(uint32_t min_interval_s);
void fng_service_get_snapshot(fng_snapshot_t *out);
