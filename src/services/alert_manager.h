#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "models/app_state.h"

#define ALERT_LOG_MAX 24

typedef struct {
	char symbol[8];
	double price;
	double threshold;
	bool is_high;
	int64_t timestamp_s;
} alert_log_t;

typedef struct {
	char symbol[8];
	double price;
	double low;
	double high;
} alert_active_t;

typedef void (*alert_trigger_cb_t)(const alert_log_t *entry);

esp_err_t alert_manager_init(void);
void alert_manager_set_state(app_state_t *state);
void alert_manager_set_callback(alert_trigger_cb_t cb);
void alert_manager_check(void);
size_t alert_manager_get_active(alert_active_t *out, size_t max_count);
size_t alert_manager_get_log(alert_log_t *out, size_t max_count);
