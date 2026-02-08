#include "services/alert_manager.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "alert_manager";

typedef struct {
    char id[32];
    double last_price;
    bool low_triggered;
    bool high_triggered;
} alert_state_t;

static app_state_t *s_state = NULL;
static alert_log_t s_log[ALERT_LOG_MAX] = {0};
static size_t s_log_count = 0;
static alert_state_t s_alert_state[MAX_WATCHLIST] = {0};
static alert_trigger_cb_t s_trigger_cb = NULL;

static alert_state_t *get_state_for_coin(const coin_t *coin)
{
    for (size_t i = 0; i < MAX_WATCHLIST; i++) {
        if (s_alert_state[i].id[0] != '\0' && strcmp(s_alert_state[i].id, coin->id) == 0) {
            return &s_alert_state[i];
        }
    }

    for (size_t i = 0; i < MAX_WATCHLIST; i++) {
        if (s_alert_state[i].id[0] == '\0') {
            strncpy(s_alert_state[i].id, coin->id, sizeof(s_alert_state[i].id) - 1);
            return &s_alert_state[i];
        }
    }

    return &s_alert_state[0];
}

static void push_log(const coin_t *coin, double price, double threshold, bool is_high)
{
    if (!coin) {
        return;
    }

    if (s_log_count >= ALERT_LOG_MAX) {
        memmove(&s_log[0], &s_log[1], sizeof(alert_log_t) * (ALERT_LOG_MAX - 1));
        s_log_count = ALERT_LOG_MAX - 1;
    }

    alert_log_t *entry = &s_log[s_log_count++];
    memset(entry, 0, sizeof(*entry));
    strncpy(entry->symbol, coin->symbol, sizeof(entry->symbol) - 1);
    entry->price = price;
    entry->threshold = threshold;
    entry->is_high = is_high;
    entry->timestamp_s = esp_timer_get_time() / 1000000;

    if (s_trigger_cb) {
        s_trigger_cb(entry);
    }
}

esp_err_t alert_manager_init(void)
{
    ESP_LOGI(TAG, "Alert manager initialized");
    return ESP_OK;
}

void alert_manager_set_state(app_state_t *state)
{
    s_state = state;
}

void alert_manager_set_callback(alert_trigger_cb_t cb)
{
    s_trigger_cb = cb;
}

void alert_manager_check(void)
{
    if (!s_state) {
        return;
    }

    for (size_t i = 0; i < s_state->watchlist_count; i++) {
        coin_t *coin = &s_state->watchlist[i];
        if (coin->price <= 0.0) {
            continue;
        }

        alert_state_t *state = get_state_for_coin(coin);
        double last_price = state->last_price;
        state->last_price = coin->price;

        if (coin->alert_low > 0.0) {
            bool crossed = last_price > 0.0 && last_price > coin->alert_low && coin->price <= coin->alert_low;
            if (crossed && !state->low_triggered) {
                push_log(coin, coin->price, coin->alert_low, false);
                state->low_triggered = true;
            }
            if (coin->price > coin->alert_low) {
                state->low_triggered = false;
            }
        }

        if (coin->alert_high > 0.0) {
            bool crossed = last_price > 0.0 && last_price < coin->alert_high && coin->price >= coin->alert_high;
            if (crossed && !state->high_triggered) {
                push_log(coin, coin->price, coin->alert_high, true);
                state->high_triggered = true;
            }
            if (coin->price < coin->alert_high) {
                state->high_triggered = false;
            }
        }
    }
}

size_t alert_manager_get_active(alert_active_t *out, size_t max_count)
{
    if (!s_state || !out || max_count == 0) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < s_state->watchlist_count && count < max_count; i++) {
        const coin_t *coin = &s_state->watchlist[i];
        if (coin->alert_low <= 0.0 && coin->alert_high <= 0.0) {
            continue;
        }
        alert_active_t *entry = &out[count++];
        memset(entry, 0, sizeof(*entry));
        strncpy(entry->symbol, coin->symbol, sizeof(entry->symbol) - 1);
        entry->price = coin->price;
        entry->low = coin->alert_low;
        entry->high = coin->alert_high;
    }
    return count;
}

size_t alert_manager_get_log(alert_log_t *out, size_t max_count)
{
    if (!out || max_count == 0) {
        return 0;
    }

    size_t count = s_log_count > max_count ? max_count : s_log_count;
    for (size_t i = 0; i < count; i++) {
        out[i] = s_log[s_log_count - 1 - i];
    }
    return count;
}
