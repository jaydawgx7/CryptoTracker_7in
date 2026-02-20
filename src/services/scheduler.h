#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "models/app_state.h"

#ifndef CT_SCHEDULER_ENABLE
#define CT_SCHEDULER_ENABLE 1
#endif

esp_err_t scheduler_init(app_state_t *state);
void scheduler_set_paused(bool paused);
