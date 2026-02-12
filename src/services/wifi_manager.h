#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
	WIFI_STATE_DISCONNECTED = 0,
	WIFI_STATE_CONNECTING,
	WIFI_STATE_CONNECTED
} wifi_state_t;

typedef struct {
	char ssid[33];
	int rssi;
	uint8_t authmode;
} wifi_scan_result_t;

typedef struct {
	char ssid[33];
	int64_t last_success;
	uint8_t priority;
} wifi_saved_network_t;

esp_err_t wifi_manager_init(void);
void wifi_manager_get_state(wifi_state_t *state, int *rssi);
bool wifi_manager_get_connected_ssid(char *out, size_t len);
bool wifi_manager_get_ip(char *out, size_t len);
esp_err_t wifi_manager_scan(wifi_scan_result_t *results, size_t max_results, size_t *out_count);
size_t wifi_manager_get_saved(wifi_saved_network_t *out, size_t max_results);
esp_err_t wifi_manager_add_network(const char *ssid, const char *password);
esp_err_t wifi_manager_forget_network(const char *ssid);
