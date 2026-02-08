#include "ui/ui_home.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "lvgl.h"

#include "ui/ui.h"
#include "ui/ui_nav.h"

#include "services/nvs_store.h"
#include "services/coingecko_client.h"

#define SPARKLINE_POINTS 28

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

#ifndef CT_SPARKLINE_ENABLE
#define CT_SPARKLINE_ENABLE 0
#endif

static const app_state_t *s_state = NULL;
static lv_obj_t *s_table_body = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_empty_label = NULL;
static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;
static lv_obj_t *s_add_btn = NULL;
static lv_obj_t *s_add_label = NULL;
static size_t s_active_index = 0;
static bool s_ignore_click = false;
static lv_obj_t *s_keyboard = NULL;
static lv_obj_t *s_holdings_field = NULL;
static lv_obj_t *s_alert_low_field = NULL;
static lv_obj_t *s_alert_high_field = NULL;
static lv_obj_t *s_sort_buttons[6] = {0};
static lv_obj_t *s_sort_dir_btn = NULL;
static lv_obj_t *s_sort_dir_label = NULL;
static sort_field_t s_sort_field = SORT_SYMBOL;
static bool s_sort_desc = false;

static lv_color_t color_pos = {0};
static lv_color_t color_neg = {0};
static lv_color_t color_neutral = {0};
static lv_color_t color_stale = {0};
static bool s_offline = false;

static void show_holdings_modal(size_t index);
static void show_alerts_modal(size_t index);
static void format_usd(double price, char *buf, size_t len);
static void format_holdings(double value, char *buf, size_t len);
static void field_focus_event(lv_event_t *e);
static void persist_state(void);
static void add_coin_nav_event(lv_event_t *e);
static double compute_total_value(void);
static void update_add_button_state(bool offline);

static void toast_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_toast(const char *text)
{
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_toast, 300, 40);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1A1D26), 0);
        lv_obj_set_style_border_width(s_toast, 0, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_pad_left(s_toast, 12, 0);
        lv_obj_set_style_pad_right(s_toast, 12, 0);
        lv_obj_set_style_pad_top(s_toast, 8, 0);
        lv_obj_set_style_pad_bottom(s_toast, 8, 0);
        lv_obj_align(s_toast, LV_ALIGN_BOTTOM_LEFT, 12, -12);

        lv_obj_t *label = lv_label_create(s_toast);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
        lv_obj_center(label);
    } else {
        lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *label = lv_obj_get_child(s_toast, 0);
        if (label) {
            lv_label_set_text(label, text);
        }
    }

    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_hide_cb, 1500, NULL);
}

static void persist_state(void)
{
    if (!s_state) {
        return;
    }

    esp_err_t err = nvs_store_save_app_state(s_state);
    if (err != ESP_OK) {
        show_toast("Save failed");
    }
}

static void add_coin_nav_event(lv_event_t *e)
{
    (void)e;
    ui_show_add_coin();
}

static double compute_total_value(void)
{
    if (!s_state) {
        return 0.0;
    }

    double total = 0.0;
    for (size_t i = 0; i < s_state->watchlist_count; i++) {
        total += s_state->watchlist[i].price * s_state->watchlist[i].holdings;
    }
    return total;
}

void ui_home_update_header(wifi_state_t wifi_state, int rssi, uint32_t updated_age_s, bool offline)
{
    if (!s_status_label) {
        return;
    }

    bool offline_changed = (s_offline != offline);
    s_offline = offline;

    char total_buf[32];
    format_usd(compute_total_value(), total_buf, sizeof(total_buf));

    char wifi_buf[32];
    if (wifi_state == WIFI_STATE_CONNECTED) {
        snprintf(wifi_buf, sizeof(wifi_buf), "WiFi %ddBm", rssi);
    } else if (wifi_state == WIFI_STATE_CONNECTING) {
        snprintf(wifi_buf, sizeof(wifi_buf), "WiFi connecting");
    } else {
        snprintf(wifi_buf, sizeof(wifi_buf), "WiFi --");
    }

    char status[96];
    if (offline) {
        snprintf(status, sizeof(status), "Offline | %s | Updated %lus | %s", wifi_buf, (unsigned long)updated_age_s, total_buf);
    } else {
        snprintf(status, sizeof(status), "%s | Updated %lus | %s", wifi_buf, (unsigned long)updated_age_s, total_buf);
    }

    lv_label_set_text(s_status_label, status);
    lv_obj_set_style_text_color(s_status_label, offline ? lv_color_hex(0xD6A16A) : lv_color_hex(0x9AA1AD), 0);

    if (offline_changed) {
        update_add_button_state(offline);
        ui_home_refresh();
    }
}

static void update_add_button_state(bool offline)
{
    if (!s_add_btn || !s_add_label) {
        return;
    }

    if (offline) {
        lv_obj_add_state(s_add_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(0x1B1F2A), 0);
        lv_obj_set_style_text_color(s_add_label, lv_color_hex(0x6B717B), 0);
    } else {
        lv_obj_clear_state(s_add_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(0x2A3142), 0);
        lv_obj_set_style_text_color(s_add_label, lv_color_hex(0xE6E6E6), 0);
    }
}

static void close_modal(void)
{
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
    s_keyboard = NULL;
    s_holdings_field = NULL;
    s_alert_low_field = NULL;
    s_alert_high_field = NULL;
}

static void detail_close_event(lv_event_t *e)
{
    (void)e;
    close_modal();
}

static void action_edit_event(lv_event_t *e)
{
    (void)e;
    close_modal();
    show_holdings_modal(s_active_index);
}

static void action_alerts_event(lv_event_t *e)
{
    (void)e;
    close_modal();
    show_alerts_modal(s_active_index);
}

static void action_pin_event(lv_event_t *e)
{
    (void)e;
    close_modal();

    if (!s_state || s_active_index >= s_state->watchlist_count) {
        return;
    }

    app_state_t *state = (app_state_t *)s_state;
    state->watchlist[s_active_index].pinned = !state->watchlist[s_active_index].pinned;
    persist_state();
    ui_home_refresh();
}

static void action_remove_event(lv_event_t *e)
{
    (void)e;
    close_modal();

    if (!s_state || s_active_index >= s_state->watchlist_count) {
        return;
    }

    app_state_t *state = (app_state_t *)s_state;
    size_t count = state->watchlist_count;
    for (size_t i = s_active_index; i + 1 < count; i++) {
        state->watchlist[i] = state->watchlist[i + 1];
    }
    state->watchlist_count = count > 0 ? count - 1 : 0;
    persist_state();
    ui_home_refresh();
}

static lv_obj_t *create_modal_container(void)
{
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, 800, 480);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, 460, 260);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    return card;
}

static lv_obj_t *create_modal_container_size(lv_coord_t width, lv_coord_t height)
{
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_modal, 800, 480);
    lv_obj_set_style_bg_color(s_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_modal);
    lv_obj_set_size(card, width, height);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    return card;
}

static lv_obj_t *create_numeric_field(lv_obj_t *parent, const char *placeholder)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_accepted_chars(ta, "0123456789.");
    lv_textarea_set_max_length(ta, 20);
    lv_obj_set_width(ta, 220);
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x11151F), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_add_event_cb(ta, field_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, field_focus_event, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void ensure_keyboard(lv_obj_t *ta)
{
#if !CT_KEYBOARD_ENABLE
    (void)ta;
    return;
#endif
    if (!s_modal) {
        return;
    }

    if (!s_keyboard) {
        s_keyboard = lv_keyboard_create(s_modal);
        lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_NUMBER);
        lv_obj_set_size(s_keyboard, 780, 160);
        lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_keyboard_set_textarea(s_keyboard, ta);
}

static void field_focus_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    ensure_keyboard(ta);
}

static bool parse_optional_double(const char *text, double *out)
{
    if (!text || text[0] == '\0') {
        return false;
    }

    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text) {
        return false;
    }

    if (out) {
        *out = value;
    }
    return true;
}

static void save_holdings_event(lv_event_t *e)
{
    (void)e;
    if (!s_state || s_active_index >= s_state->watchlist_count || !s_holdings_field) {
        close_modal();
        return;
    }

    const char *text = lv_textarea_get_text(s_holdings_field);
    double value = 0.0;
    if (!parse_optional_double(text, &value)) {
        show_toast("Invalid holdings");
        return;
    }

    double scaled = round(value * 100000000.0) / 100000000.0;
    app_state_t *state = (app_state_t *)s_state;
    state->watchlist[s_active_index].holdings = scaled;
    persist_state();
    close_modal();
    ui_home_refresh();
}

static void save_alerts_event(lv_event_t *e)
{
    (void)e;
    if (!s_state || s_active_index >= s_state->watchlist_count) {
        close_modal();
        return;
    }

    double low_value = 0.0;
    double high_value = 0.0;
    bool has_low = parse_optional_double(lv_textarea_get_text(s_alert_low_field), &low_value);
    bool has_high = parse_optional_double(lv_textarea_get_text(s_alert_high_field), &high_value);

    app_state_t *state = (app_state_t *)s_state;
    state->watchlist[s_active_index].alert_low = has_low ? low_value : 0.0;
    state->watchlist[s_active_index].alert_high = has_high ? high_value : 0.0;
    persist_state();
    close_modal();
    ui_home_refresh();
}

static void cancel_modal_event(lv_event_t *e)
{
    (void)e;
    close_modal();
}

static void show_holdings_modal(size_t index)
{
    if (!s_state || index >= s_state->watchlist_count) {
        return;
    }

    close_modal();
    s_active_index = index;

    const coin_t *coin = &s_state->watchlist[index];
    lv_obj_t *card = create_modal_container_size(520, 300);

    char title[64];
    snprintf(title, sizeof(title), "Edit Holdings (%s)", coin->symbol);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);

    lv_obj_t *field_label = lv_label_create(card);
    lv_label_set_text(field_label, "Quantity (up to 8 decimals)");
    lv_obj_set_style_text_color(field_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(field_label, LV_ALIGN_TOP_LEFT, 0, 48);

    s_holdings_field = create_numeric_field(card, "0.0");
    lv_obj_align(s_holdings_field, LV_ALIGN_TOP_LEFT, 0, 78);

    char buf_hold[32];
    format_holdings(coin->holdings, buf_hold, sizeof(buf_hold));
    lv_textarea_set_text(s_holdings_field, buf_hold);
    ensure_keyboard(s_holdings_field);

    lv_obj_t *save_btn = lv_btn_create(card);
    lv_obj_set_size(save_btn, 120, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(save_btn, save_holdings_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, 120, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_modal_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
}

static void show_alerts_modal(size_t index)
{
    if (!s_state || index >= s_state->watchlist_count) {
        return;
    }

    close_modal();
    s_active_index = index;

    const coin_t *coin = &s_state->watchlist[index];
    lv_obj_t *card = create_modal_container_size(520, 320);

    char title[64];
    snprintf(title, sizeof(title), "Alerts (%s)", coin->symbol);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);

    lv_obj_t *low_label = lv_label_create(card);
    lv_label_set_text(low_label, "Low price (optional)");
    lv_obj_set_style_text_color(low_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(low_label, LV_ALIGN_TOP_LEFT, 0, 48);

    s_alert_low_field = create_numeric_field(card, "Disabled");
    lv_obj_align(s_alert_low_field, LV_ALIGN_TOP_LEFT, 0, 78);

    lv_obj_t *high_label = lv_label_create(card);
    lv_label_set_text(high_label, "High price (optional)");
    lv_obj_set_style_text_color(high_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(high_label, LV_ALIGN_TOP_LEFT, 0, 130);

    s_alert_high_field = create_numeric_field(card, "Disabled");
    lv_obj_align(s_alert_high_field, LV_ALIGN_TOP_LEFT, 0, 160);

    if (coin->alert_low > 0.0) {
        char buf_low[32];
        format_usd(coin->alert_low, buf_low, sizeof(buf_low));
        lv_textarea_set_text(s_alert_low_field, buf_low + 1);
    }

    if (coin->alert_high > 0.0) {
        char buf_high[32];
        format_usd(coin->alert_high, buf_high, sizeof(buf_high));
        lv_textarea_set_text(s_alert_high_field, buf_high + 1);
    }

    ensure_keyboard(s_alert_low_field);

    lv_obj_t *save_btn = lv_btn_create(card);
    lv_obj_set_size(save_btn, 120, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(save_btn, save_alerts_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *cancel_btn = lv_btn_create(card);
    lv_obj_set_size(cancel_btn, 120, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_modal_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
}


static void show_actions_modal(size_t index)
{
    if (!s_state || index >= s_state->watchlist_count) {
        return;
    }

    close_modal();
    s_active_index = index;

    const coin_t *coin = &s_state->watchlist[index];
    lv_obj_t *card = create_modal_container();

    char title[64];
    snprintf(title, sizeof(title), "%s (%s)", coin->name, coin->symbol);
    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_22, 0);

    lv_obj_t *grid = lv_obj_create(card);
    lv_obj_set_size(grid, 420, 150);
    lv_obj_align(grid, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *edit_btn = lv_btn_create(grid);
    lv_obj_set_size(edit_btn, 200, 42);
    lv_obj_add_event_cb(edit_btn, action_edit_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *edit_label = lv_label_create(edit_btn);
    lv_label_set_text(edit_label, "Edit Holdings");
    lv_obj_center(edit_label);

    lv_obj_t *alert_btn = lv_btn_create(grid);
    lv_obj_set_size(alert_btn, 200, 42);
    lv_obj_add_event_cb(alert_btn, action_alerts_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *alert_label = lv_label_create(alert_btn);
    lv_label_set_text(alert_label, "Alerts");
    lv_obj_center(alert_label);

    lv_obj_t *pin_btn = lv_btn_create(grid);
    lv_obj_set_size(pin_btn, 200, 42);
    lv_obj_add_event_cb(pin_btn, action_pin_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pin_label = lv_label_create(pin_btn);
    lv_label_set_text(pin_label, coin->pinned ? "Unpin" : "Pin to Top");
    lv_obj_center(pin_label);

    lv_obj_t *remove_btn = lv_btn_create(grid);
    lv_obj_set_size(remove_btn, 200, 42);
    lv_obj_add_event_cb(remove_btn, action_remove_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *remove_label = lv_label_create(remove_btn);
    lv_label_set_text(remove_label, "Remove");
    lv_obj_center(remove_label);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 100, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, detail_close_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    lv_obj_center(close_label);
}

static void row_event_cb(lv_event_t *e)
{
    size_t index = (size_t)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (s_offline) {
        s_ignore_click = false;
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        s_ignore_click = true;
        show_actions_modal(index);
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        if (s_ignore_click) {
            s_ignore_click = false;
            return;
        }
        ui_show_coin_detail(index);
    }
}

static void format_trim_zeros(char *buf)
{
    char *dot = strchr(buf, '.');
    if (!dot) {
        return;
    }

    char *end = buf + strlen(buf) - 1;
    while (end > dot && *end == '0') {
        *end = '\0';
        end--;
    }
    if (end == dot) {
        *end = '\0';
    }
}

static void format_usd(double price, char *buf, size_t len)
{
    if (price <= 0.0) {
        snprintf(buf, len, "$0.00");
        return;
    }

    if (price >= 1.0) {
        snprintf(buf, len, "$%.2f", price);
        return;
    }

    if (price >= 0.01) {
        snprintf(buf, len, "$%.6f", price);
        format_trim_zeros(buf);
        return;
    }

    int first_nonzero = 0;
    double scaled = price;
    while (scaled < 1.0 && first_nonzero < 10) {
        scaled *= 10.0;
        first_nonzero++;
    }

    int decimals = first_nonzero + 1;
    if (decimals > 10) {
        decimals = 10;
    }

    snprintf(buf, len, "$%.*f", decimals, price);
}

static void format_holdings(double value, char *buf, size_t len)
{
    snprintf(buf, len, "%.8f", value);
    format_trim_zeros(buf);
}

static void format_percent(double change, char *buf, size_t len)
{
    const char *arrow = "";
    if (change > 0.05) {
        arrow = "^";
    } else if (change < -0.05) {
        arrow = "v";
    }

    snprintf(buf, len, "%s%.2f%%", arrow, fabs(change));
}

static lv_color_t percent_color(double change)
{
    if (change > 0.05) {
        return color_pos;
    }
    if (change < -0.05) {
        return color_neg;
    }
    return color_neutral;
}

static lv_obj_t *create_cell(lv_obj_t *parent, const char *text, lv_coord_t width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    return label;
}

static void set_sparkline_placeholder(lv_obj_t *chart, lv_chart_series_t *series, double price)
{
    if (!chart || !series) {
        return;
    }

    int32_t value = (int32_t)(price * 100.0);
    lv_chart_set_point_count(chart, 2);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, value, value + 1);
    lv_chart_set_value_by_id(chart, series, 0, value);
    lv_chart_set_value_by_id(chart, series, 1, value);
    lv_obj_set_style_line_color(chart, color_neutral, LV_PART_ITEMS);
    lv_chart_refresh(chart);
}

static void set_sparkline_data(lv_obj_t *chart, lv_chart_series_t *series, const chart_point_t *points, size_t count)
{
    if (!chart || !series || !points || count < 2) {
        return;
    }

    size_t sample = SPARKLINE_POINTS;
    if (count < sample) {
        sample = count;
    }

    double min = points[0].price;
    double max = points[0].price;
    for (size_t i = 0; i < sample; i++) {
        size_t idx = (sample > 1) ? (size_t)((i * (count - 1)) / (sample - 1)) : 0;
        double price = points[idx].price;
        if (price < min) {
            min = price;
        }
        if (price > max) {
            max = price;
        }
    }

    int32_t min_val = (int32_t)(min * 100.0);
    int32_t max_val = (int32_t)(max * 100.0);
    if (min_val == max_val) {
        max_val += 1;
    }

    lv_chart_set_point_count(chart, (uint16_t)sample);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, min_val, max_val);

    double first_price = 0.0;
    double last_price = 0.0;
    for (size_t i = 0; i < sample; i++) {
        size_t idx = (sample > 1) ? (size_t)((i * (count - 1)) / (sample - 1)) : 0;
        double price = points[idx].price;
        if (i == 0) {
            first_price = price;
        }
        if (i == sample - 1) {
            last_price = price;
        }
        int32_t value = (int32_t)(price * 100.0);
        lv_chart_set_value_by_id(chart, series, (uint16_t)i, value);
    }

    lv_color_t line_color = s_offline ? color_neutral : (last_price >= first_price ? color_pos : color_neg);
    lv_obj_set_style_line_color(chart, line_color, LV_PART_ITEMS);
    lv_chart_refresh(chart);
}

static lv_obj_t *create_sparkline_cell(lv_obj_t *parent, const coin_t *coin)
{
#if !CT_SPARKLINE_ENABLE
    lv_obj_t *placeholder = lv_label_create(parent);
    lv_label_set_text(placeholder, "-");
    lv_obj_set_width(placeholder, 90);
    lv_obj_set_style_text_color(placeholder, lv_color_hex(0x6B717B), 0);
    lv_obj_set_style_text_align(placeholder, LV_TEXT_ALIGN_CENTER, 0);
    return placeholder;
#else
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_size(chart, 90, 24);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_opa(chart, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_add_flag(chart, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(chart, 0, 0);

    lv_chart_series_t *series = lv_chart_add_series(chart, color_neutral, LV_CHART_AXIS_PRIMARY_Y);

    const chart_point_t *points = NULL;
    size_t count = 0;
    if (coin && coingecko_client_get_chart_cached(coin->id, 1, &points, &count) == ESP_OK) {
        set_sparkline_data(chart, series, points, count);
    } else {
        set_sparkline_placeholder(chart, series, coin ? coin->price : 0.0);
    }

    return chart;
#endif
}

static int compare_coins(const coin_t *a, const coin_t *b)
{
    if (a->pinned != b->pinned) {
        return a->pinned ? -1 : 1;
    }

    double key_a = 0.0;
    double key_b = 0.0;

    switch (s_sort_field) {
        case SORT_PRICE:
            key_a = a->price;
            key_b = b->price;
            break;
        case SORT_1H:
            key_a = a->change_1h;
            key_b = b->change_1h;
            break;
        case SORT_24H:
            key_a = a->change_24h;
            key_b = b->change_24h;
            break;
        case SORT_7D:
            key_a = a->change_7d;
            key_b = b->change_7d;
            break;
        case SORT_VALUE:
            key_a = a->price * a->holdings;
            key_b = b->price * b->holdings;
            break;
        case SORT_SYMBOL:
        default:
            break;
    }

    if (s_sort_field == SORT_SYMBOL) {
        int cmp = strcasecmp(a->symbol, b->symbol);
        return s_sort_desc ? -cmp : cmp;
    }

    if (key_a < key_b) {
        return s_sort_desc ? 1 : -1;
    }
    if (key_a > key_b) {
        return s_sort_desc ? -1 : 1;
    }
    return 0;
}

static void sort_indices(size_t *indices, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            const coin_t *a = &s_state->watchlist[indices[i]];
            const coin_t *b = &s_state->watchlist[indices[j]];
            if (compare_coins(a, b) > 0) {
                size_t tmp = indices[i];
                indices[i] = indices[j];
                indices[j] = tmp;
            }
        }
    }
}

static void update_sort_buttons(void)
{
    for (int i = 0; i < 6; i++) {
        if (!s_sort_buttons[i]) {
            continue;
        }
        bool active = (i == s_sort_field);
        lv_obj_set_style_bg_color(s_sort_buttons[i], active ? lv_color_hex(0x2A3142) : lv_color_hex(0x1A1D26), 0);
        lv_obj_set_style_text_color(s_sort_buttons[i], active ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x9AA1AD), 0);
    }

    if (s_sort_dir_label) {
        lv_label_set_text(s_sort_dir_label, s_sort_desc ? "Desc" : "Asc");
    }
}

static void sort_button_event(lv_event_t *e)
{
    sort_field_t field = (sort_field_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_sort_field == field) {
        s_sort_desc = !s_sort_desc;
    } else {
        s_sort_field = field;
    }
    update_sort_buttons();
    ui_home_refresh();
}

static void sort_dir_event(lv_event_t *e)
{
    (void)e;
    s_sort_desc = !s_sort_desc;
    update_sort_buttons();
    ui_home_refresh();
}

void ui_home_set_state(const app_state_t *state)
{
    s_state = state;
    if (state) {
        s_sort_field = state->prefs.sort_field;
        s_sort_desc = state->prefs.sort_desc;
    }
    update_sort_buttons();
    ui_home_refresh();
}

void ui_home_refresh(void)
{
    if (!s_table_body) {
        return;
    }

    lv_obj_clean(s_table_body);
    s_empty_label = NULL;

    if (!s_empty_label) {
        s_empty_label = lv_label_create(s_table_body);
        lv_label_set_text(s_empty_label, "Watchlist empty. Add coins to get started.");
        lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0x5C626B), 0);
        lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);
    }

    if (!s_state || s_state->watchlist_count == 0) {
        lv_obj_clear_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_add_flag(s_empty_label, LV_OBJ_FLAG_HIDDEN);

    size_t count = s_state->watchlist_count;
    size_t indices[MAX_WATCHLIST] = {0};
    for (size_t i = 0; i < count; i++) {
        indices[i] = i;
    }
    sort_indices(indices, count);

    for (size_t i = 0; i < count; i++) {
        const coin_t *coin = &s_state->watchlist[indices[i]];
        size_t coin_index = indices[i];

        lv_obj_t *row = lv_obj_create(s_table_body);
        lv_obj_set_size(row, 780, 36);
        lv_obj_set_style_bg_color(row, (i % 2 == 0) ? lv_color_hex(0x121620) : lv_color_hex(0x101218), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)coin_index);
        lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)coin_index);

        char buf_price[32];
        char buf_1h[16];
        char buf_24h[16];
        char buf_7d[16];
        char buf_hold[24];
        char buf_value[32];

        format_usd(coin->price, buf_price, sizeof(buf_price));
        format_percent(coin->change_1h, buf_1h, sizeof(buf_1h));
        format_percent(coin->change_24h, buf_24h, sizeof(buf_24h));
        format_percent(coin->change_7d, buf_7d, sizeof(buf_7d));
        format_holdings(coin->holdings, buf_hold, sizeof(buf_hold));
        format_usd(coin->price * coin->holdings, buf_value, sizeof(buf_value));

        lv_color_t value_color = s_offline ? color_stale : lv_color_hex(0xE6E6E6);
        lv_color_t holdings_color = s_offline ? color_stale : lv_color_hex(0xC5CBD6);
        lv_color_t pct_1h_color = s_offline ? color_neutral : percent_color(coin->change_1h);
        lv_color_t pct_24h_color = s_offline ? color_neutral : percent_color(coin->change_24h);
        lv_color_t pct_7d_color = s_offline ? color_neutral : percent_color(coin->change_7d);

        create_cell(row, coin->symbol, 80, lv_color_hex(0xE6E6E6));
        create_cell(row, buf_price, 120, value_color);
        create_cell(row, buf_1h, 70, pct_1h_color);
        create_cell(row, buf_24h, 70, pct_24h_color);
        create_cell(row, buf_7d, 70, pct_7d_color);
        create_sparkline_cell(row, coin);
        create_cell(row, buf_hold, 120, holdings_color);
        create_cell(row, buf_value, 120, value_color);
    }
}

lv_obj_t *ui_home_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101218), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);

    color_pos = lv_color_hex(0x2ECC71);
    color_neg = lv_color_hex(0xE74C3C);
    color_neutral = lv_color_hex(0x9AA1AD);
    color_stale = lv_color_hex(0x6B717B);

    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, 800, 50);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(header, 0, 0);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "CryptoTracker");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);

    s_add_btn = lv_btn_create(header);
    lv_obj_set_size(s_add_btn, 90, 30);
    lv_obj_align(s_add_btn, LV_ALIGN_RIGHT_MID, -200, 0);
    lv_obj_set_style_radius(s_add_btn, 6, 0);
    lv_obj_set_style_bg_color(s_add_btn, lv_color_hex(0x2A3142), 0);
    lv_obj_add_event_cb(s_add_btn, add_coin_nav_event, LV_EVENT_CLICKED, NULL);

    s_add_label = lv_label_create(s_add_btn);
    lv_label_set_text(s_add_label, "Add Coin");
    lv_obj_center(s_add_label);

    s_status_label = lv_label_create(header);
    lv_label_set_text(s_status_label, "WiFi -- | Updated --s | $0.00");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(s_status_label, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_t *table_header = lv_obj_create(screen);
    lv_obj_set_size(table_header, 800, 36);
    lv_obj_align(table_header, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_color(table_header, lv_color_hex(0x141822), 0);
    lv_obj_set_style_border_width(table_header, 0, 0);

    lv_obj_t *cols = lv_label_create(table_header);
    lv_label_set_text(cols, "Symbol   Price   1h%   24h%   7d%   Spark   Holdings   Value");
    lv_obj_set_style_text_color(cols, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(cols, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *sort_bar = lv_obj_create(screen);
    lv_obj_set_size(sort_bar, 800, 34);
    lv_obj_align(sort_bar, LV_ALIGN_TOP_LEFT, 0, 86);
    lv_obj_set_style_bg_color(sort_bar, lv_color_hex(0x11151F), 0);
    lv_obj_set_style_border_width(sort_bar, 0, 0);
    lv_obj_set_style_pad_left(sort_bar, 8, 0);
    lv_obj_set_style_pad_right(sort_bar, 8, 0);
    lv_obj_set_style_pad_top(sort_bar, 4, 0);
    lv_obj_set_style_pad_bottom(sort_bar, 4, 0);
    lv_obj_set_flex_flow(sort_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(sort_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    static const char *labels[6] = {"Sym", "Price", "1h", "24h", "7d", "Value"};
    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_btn_create(sort_bar);
        lv_obj_set_size(btn, 70, 26);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A1D26), 0);
        lv_obj_add_event_cb(btn, sort_button_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, labels[i]);
        lv_obj_center(label);
        s_sort_buttons[i] = btn;
    }

    s_sort_dir_btn = lv_btn_create(sort_bar);
    lv_obj_set_size(s_sort_dir_btn, 70, 26);
    lv_obj_set_style_radius(s_sort_dir_btn, 6, 0);
    lv_obj_set_style_bg_color(s_sort_dir_btn, lv_color_hex(0x1A1D26), 0);
    lv_obj_add_event_cb(s_sort_dir_btn, sort_dir_event, LV_EVENT_CLICKED, NULL);

    s_sort_dir_label = lv_label_create(s_sort_dir_btn);
    lv_label_set_text(s_sort_dir_label, "Asc");
    lv_obj_center(s_sort_dir_label);

    s_table_body = lv_obj_create(screen);
    lv_obj_set_size(s_table_body, 800, 300);
    lv_obj_align(s_table_body, LV_ALIGN_TOP_LEFT, 0, 120);
    lv_obj_set_style_bg_color(s_table_body, lv_color_hex(0x101218), 0);
    lv_obj_set_style_border_width(s_table_body, 0, 0);
    lv_obj_set_style_pad_left(s_table_body, 10, 0);
    lv_obj_set_style_pad_right(s_table_body, 10, 0);
    lv_obj_set_style_pad_top(s_table_body, 8, 0);
    lv_obj_set_style_pad_bottom(s_table_body, 8, 0);
    lv_obj_set_flex_flow(s_table_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_table_body, LV_SCROLLBAR_MODE_AUTO);

    s_empty_label = lv_label_create(s_table_body);
    lv_label_set_text(s_empty_label, "Watchlist empty. Add coins to get started.");
    lv_obj_set_style_text_color(s_empty_label, lv_color_hex(0x5C626B), 0);
    lv_obj_align(s_empty_label, LV_ALIGN_CENTER, 0, 0);

    ui_nav_attach(screen, UI_NAV_HOME);
    update_sort_buttons();
    return screen;
}
