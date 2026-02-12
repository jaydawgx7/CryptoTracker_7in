#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    OTA_STATE_IDLE = 0,
    OTA_STATE_DOWNLOADING,
    OTA_STATE_SUCCESS,
    OTA_STATE_FAILED
} ota_state_t;

typedef struct {
    ota_state_t state;
    int percent;
    int last_error;
    char message[96];
} ota_status_t;

esp_err_t ota_update_start(const char *url);
void ota_update_get_status(ota_status_t *out);
