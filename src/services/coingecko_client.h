#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "models/app_state.h"

#define COINGECKO_MAX_COINS 6000

typedef struct {
	char id[32];
	char symbol[12];
	char name[48];
	double usd_price;
} coin_meta_t;

typedef struct {
	coin_meta_t *items;
	size_t count;
} coin_list_t;

typedef struct {
    int64_t ts_ms;
    double price;
} chart_point_t;

esp_err_t coingecko_client_init(void);
esp_err_t coingecko_client_fetch_coin_list(coin_list_t *list);
esp_err_t coingecko_client_search_coins(const char *query, coin_list_t *list, size_t limit);
esp_err_t coingecko_client_fetch_markets(app_state_t *state);
esp_err_t coingecko_client_fetch_markets_mode(app_state_t *state, bool update_price);
esp_err_t coingecko_client_get_chart(const char *coin_id, int days, const chart_point_t **points, size_t *count);
esp_err_t coingecko_client_get_chart_cached(const char *coin_id, int days, const chart_point_t **points, size_t *count);
esp_err_t coingecko_client_load_cached_list(coin_list_t *list);
esp_err_t coingecko_client_cache_list(const coin_list_t *list);
const coin_meta_t *coingecko_client_find_symbol(const coin_list_t *list, const char *symbol);
void coingecko_client_free_list(coin_list_t *list);
