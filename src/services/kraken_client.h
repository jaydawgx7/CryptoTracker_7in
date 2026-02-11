#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "models/app_state.h"

esp_err_t kraken_client_fetch_prices(app_state_t *state, bool *out_any_missing);
