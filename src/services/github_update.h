#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    GITHUB_UPDATE_IDLE = 0,
    GITHUB_UPDATE_CHECKING,
    GITHUB_UPDATE_UP_TO_DATE,
    GITHUB_UPDATE_AVAILABLE,
    GITHUB_UPDATE_FAILED,
    GITHUB_UPDATE_RATE_LIMITED
} github_update_state_t;

typedef struct {
    github_update_state_t state;
    int last_http;
    int last_error;
    int64_t last_checked_ms;
    char latest_tag[32];
    char download_url[256];
    char notes[256];
} github_update_status_t;

esp_err_t github_update_start_check(void);
void github_update_get_status(github_update_status_t *out);
int semver_compare(const char *a, const char *b, bool *ok);
