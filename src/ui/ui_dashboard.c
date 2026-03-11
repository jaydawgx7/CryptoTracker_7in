#include "ui/ui_dashboard.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"

#include "services/app_state_guard.h"
#include "services/fng_service.h"
#include "services/nvs_store.h"
#include "services/scheduler.h"
#include "ui/ui.h"
#include "ui/ui_home.h"
#include "ui/ui_nav.h"
#include "ui/ui_theme.h"

#define DASH_CARD_RADIUS 12
#define DASH_COL_W 376
#define DASH_LEFT_X 16
#define DASH_RIGHT_X 408
#define DASH_TOP_Y 12
#define DASH_GAP_Y 12
#define DASH_MOOD_H 180
#define DASH_HIGHLIGHTS_H 148
#define DASH_MARKET_STRIP_H 56
#define DASH_PORTFOLIO_H 148
#define DASH_TOP5_H 220
#define DASH_TOP5_ROWS 5
#define DASH_ROW_H 30
#define DASH_ROW_H_COMPACT 28
#define DASH_LIST_GAP 6

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *title;
    lv_obj_t *symbol;
    lv_obj_t *metric;
    lv_obj_t *extra;
    size_t coin_index;
} dash_item_row_t;

static const app_state_t *s_state = NULL;
static lv_obj_t *s_screen = NULL;

static lv_obj_t *s_mood_value = NULL;
static lv_obj_t *s_mood_class = NULL;
static lv_obj_t *s_mood_updated = NULL;
static lv_obj_t *s_mood_fallback = NULL;
static lv_obj_t *s_mood_arc = NULL;
static lv_obj_t *s_loading_overlay = NULL;
static lv_obj_t *s_loading_spinner = NULL;
static lv_obj_t *s_loading_label = NULL;
static bool s_initial_data_loaded = false;
static int64_t s_spinner_started_us = 0;
static lv_obj_t *s_footer_updated = NULL;
static lv_timer_t *s_footer_timer = NULL;

static lv_obj_t *s_alt_btc_value = NULL;
static lv_obj_t *s_alt_eth_value = NULL;
static lv_obj_t *s_alt_others_value = NULL;
static lv_obj_t *s_alt_btc_change = NULL;
static lv_obj_t *s_alt_eth_change = NULL;
static lv_obj_t *s_alt_others_change = NULL;
static lv_obj_t *s_alt_left = NULL;
static lv_obj_t *s_alt_mid = NULL;
static lv_obj_t *s_alt_right = NULL;
static lv_obj_t *s_alt_btc_dot = NULL;
static lv_obj_t *s_alt_eth_dot = NULL;
static lv_obj_t *s_alt_others_dot = NULL;
static lv_obj_t *s_alt_track = NULL;
static lv_obj_t *s_alt_btc_fill = NULL;
static lv_obj_t *s_alt_eth_fill = NULL;
static lv_obj_t *s_alt_alt_fill = NULL;

static lv_obj_t *s_portfolio_total = NULL;
static lv_obj_t *s_portfolio_24h = NULL;
static lv_obj_t *s_portfolio_7d = NULL;
static lv_obj_t *s_portfolio_meta = NULL;
static lv_obj_t *s_portfolio_demo_label = NULL;
static lv_obj_t *s_values_toggle_btn = NULL;
static lv_obj_t *s_values_toggle_icon = NULL;

static lv_obj_t *s_market_cap_value = NULL;
static lv_obj_t *s_market_cap_change = NULL;
static lv_obj_t *s_btc_price_value = NULL;
static lv_obj_t *s_btc_price_change = NULL;

static dash_item_row_t s_top_rows[DASH_TOP5_ROWS] = {0};

static void refresh_dashboard_internal(bool require_active);

static void set_label_text_if_changed(lv_obj_t *label, const char *text)
{
    if (!label || !text) {
        return;
    }

    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0) {
        return;
    }

    lv_label_set_text(label, text);
}

static void set_hidden_if_changed(lv_obj_t *obj, bool hidden)
{
    if (!obj) {
        return;
    }

    bool is_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
    if (is_hidden == hidden) {
        return;
    }

    if (hidden) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static uint32_t dashboard_footer_update_period_ms(int64_t age_s)
{
    if (age_s < 0) {
        return 10000;
    }
    if (age_s < 60) {
        return 5000;
    }
    if (age_s < 3600) {
        return 30000;
    }
    return 60000;
}

static bool is_finite_value(double value)
{
    return (value - value) == 0.0;
}

static bool dashboard_show_values(void)
{
    return !s_state || s_state->prefs.show_values;
}

static lv_color_t muted_text_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return lv_color_hex(theme ? theme->text_muted : 0x9AA1AD);
}

static void update_values_toggle_icon(void)
{
    if (!s_values_toggle_btn || !s_values_toggle_icon) {
        return;
    }

    bool show_values = dashboard_show_values();
    const ui_theme_colors_t *theme = ui_theme_get();
    uint32_t accent = theme ? theme->accent : 0x00FE8F;
    uint32_t muted = theme ? theme->text_muted : 0x6B717B;
    uint32_t btn_bg = theme ? theme->nav_inactive_bg : 0x1B1F2A;

    lv_obj_set_style_bg_color(s_values_toggle_btn, lv_color_hex(btn_bg), 0);
    set_label_text_if_changed(s_values_toggle_icon, show_values ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(s_values_toggle_icon, lv_color_hex(show_values ? accent : muted), 0);
}

static void values_toggle_event(lv_event_t *e)
{
    (void)e;
    if (!s_state) {
        return;
    }

    if (!app_state_guard_lock(250)) {
        return;
    }

    app_state_t *state = (app_state_t *)s_state;
    state->prefs.show_values = !state->prefs.show_values;
    update_values_toggle_icon();
    (void)nvs_store_save_app_state(state);
    app_state_guard_unlock();
    ui_dashboard_refresh();
}

static lv_color_t percent_color(double change)
{
    if (change > 0.05) {
        return lv_color_hex(0x2ECC71);
    }
    if (change < -0.05) {
        return lv_color_hex(0xE74C3C);
    }
    const ui_theme_colors_t *theme = ui_theme_get();
    return lv_color_hex(theme ? theme->text_muted : 0x9AA1AD);
}

static void update_mood_age_label(void)
{
    if (!s_footer_updated) {
        return;
    }

    uint32_t market_age_s = scheduler_get_last_market_update_age_s();
    fng_snapshot_t snapshot = {0};
    fng_service_get_snapshot(&snapshot);

    uint32_t fng_age_s = UINT32_MAX;
    if (snapshot.has_value && snapshot.fetched_at_s > 0) {
        int64_t now_s = esp_timer_get_time() / 1000000;
        int64_t delta_s = (now_s > snapshot.fetched_at_s) ? (now_s - snapshot.fetched_at_s) : 0;
        if (delta_s < 0) {
            delta_s = 0;
        }
        if (delta_s <= (int64_t)UINT32_MAX) {
            fng_age_s = (uint32_t)delta_s;
        }
    }

    uint32_t age_s = market_age_s;
    if (age_s == UINT32_MAX) {
        age_s = fng_age_s;
    }

    if (age_s == UINT32_MAX) {
        set_label_text_if_changed(s_footer_updated, "Updated: --");
        if (s_footer_timer) {
            lv_timer_set_period(s_footer_timer, dashboard_footer_update_period_ms(-1));
        }
        return;
    }

    char updated[48];
    if (age_s >= 3600) {
        snprintf(updated, sizeof(updated), "Updated: %uh ago", (unsigned)(age_s / 3600));
    } else if (age_s >= 60) {
        unsigned minutes = (unsigned)(age_s / 60);
        unsigned seconds = (unsigned)(age_s % 60);
        snprintf(updated, sizeof(updated), "Updated: %um%us ago", minutes, seconds);
    } else {
        snprintf(updated, sizeof(updated), "Updated: %us ago", (unsigned)age_s);
    }
    set_label_text_if_changed(s_footer_updated, updated);
    if (s_footer_timer) {
        lv_timer_set_period(s_footer_timer, dashboard_footer_update_period_ms(age_s));
    }
}

static void footer_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_screen || !lv_obj_is_valid(s_screen) || lv_scr_act() != s_screen) {
        return;
    }
    update_mood_age_label();
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

static void format_number_commas_trim(double value, int max_decimals, char *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    char tmp[64];
    if (max_decimals < 0) {
        max_decimals = 0;
    }
    snprintf(tmp, sizeof(tmp), "%.*f", max_decimals, value);
    format_trim_zeros(tmp);

    bool negative = (tmp[0] == '-');
    const char *start = negative ? tmp + 1 : tmp;
    const char *dot = strchr(start, '.');
    size_t int_len = dot ? (size_t)(dot - start) : strlen(start);
    size_t commas = int_len > 3 ? (int_len - 1) / 3 : 0;
    size_t tail_len = dot ? strlen(dot) : 0;
    size_t needed = int_len + commas + tail_len + (negative ? 1 : 0) + 1;
    if (needed > len) {
        strncpy(buf, tmp, len - 1);
        buf[len - 1] = '\0';
        return;
    }

    size_t pos = 0;
    if (negative) {
        buf[pos++] = '-';
    }
    for (size_t i = 0; i < int_len; i++) {
        if (i > 0 && ((int_len - i) % 3) == 0) {
            buf[pos++] = ',';
        }
        buf[pos++] = start[i];
    }
    if (dot) {
        strncpy(buf + pos, dot, len - pos);
    } else {
        buf[pos] = '\0';
    }
}

static void format_usd(double price, char *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    if (price <= 0.0) {
        snprintf(buf, len, "$0.00");
        return;
    }

    if (price >= 1000.0) {
        format_number_commas_trim(price, 0, buf, len);
        if (buf[0] != '\0' && buf[0] != '$') {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "$%s", buf);
            snprintf(buf, len, "%s", tmp);
        }
        return;
    }

    if (price >= 1.0) {
        format_number_commas_trim(price, 2, buf, len);
        if (buf[0] != '\0' && buf[0] != '$') {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "$%s", buf);
            snprintf(buf, len, "%s", tmp);
        }
        return;
    }

    int leading_zeros = 0;
    double scaled = price;
    while (scaled < 0.1 && leading_zeros < 10) {
        scaled *= 10.0;
        leading_zeros++;
    }

    int decimals = leading_zeros + 4;
    if (decimals > 12) {
        decimals = 12;
    }
    snprintf(buf, len, "$%.*f", decimals, price);
}

static void format_usd_price(double price, char *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    if (price <= 0.0) {
        snprintf(buf, len, "$0.00");
        return;
    }

    if (price >= 1000.0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%.0f", price);

        size_t int_len = strlen(tmp);
        size_t commas = int_len > 3 ? (int_len - 1) / 3 : 0;
        size_t needed = 1 + int_len + commas + 1;
        if (needed > len) {
            snprintf(buf, len, "$%.0f", price);
            return;
        }

        size_t pos = 0;
        buf[pos++] = '$';
        for (size_t i = 0; i < int_len; i++) {
            if (i > 0 && ((int_len - i) % 3) == 0) {
                buf[pos++] = ',';
            }
            buf[pos++] = tmp[i];
        }
        buf[pos] = '\0';
        return;
    }

    if (price >= 1.0) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%.2f", price);

        const char *dot = strchr(tmp, '.');
        size_t int_len = dot ? (size_t)(dot - tmp) : strlen(tmp);
        size_t commas = int_len > 3 ? (int_len - 1) / 3 : 0;
        size_t needed = 1 + int_len + commas + (dot ? strlen(dot) : 0) + 1;
        if (needed > len) {
            snprintf(buf, len, "$%.2f", price);
            return;
        }

        size_t pos = 0;
        buf[pos++] = '$';
        for (size_t i = 0; i < int_len; i++) {
            if (i > 0 && ((int_len - i) % 3) == 0) {
                buf[pos++] = ',';
            }
            buf[pos++] = tmp[i];
        }
        if (dot) {
            strncpy(buf + pos, dot, len - pos);
        } else {
            buf[pos] = '\0';
        }
        return;
    }

    int leading_zeros = 0;
    double scaled = price;
    while (scaled < 0.1 && leading_zeros < 10) {
        scaled *= 10.0;
        leading_zeros++;
    }

    int decimals = leading_zeros + 4;
    if (decimals > 12) {
        decimals = 12;
    }
    snprintf(buf, len, "$%.*f", decimals, price);
}

static void format_compact_usd(double value, char *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    if (!is_finite_value(value) || value <= 0.0) {
        snprintf(buf, len, "$--");
        return;
    }

    double abs_value = fabs(value);
    double scale = 1.0;
    const char *suffix = "";

    if (abs_value >= 1e12) {
        scale = 1e12;
        suffix = "T";
    } else if (abs_value >= 1e9) {
        scale = 1e9;
        suffix = "B";
    } else if (abs_value >= 1e6) {
        scale = 1e6;
        suffix = "M";
    } else if (abs_value >= 1e3) {
        scale = 1e3;
        suffix = "K";
    }

    if (suffix[0] == '\0') {
        format_number_commas_trim(value, 0, buf, len);
        if (buf[0] != '\0' && buf[0] != '$') {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "$%s", buf);
            snprintf(buf, len, "%s", tmp);
        }
        return;
    }

    double compact = value / scale;
    int decimals = (fabs(compact) >= 100.0) ? 0 : ((fabs(compact) >= 10.0) ? 1 : 2);
    char num[24];
    snprintf(num, sizeof(num), "%.*f", decimals, compact);
    format_trim_zeros(num);
    snprintf(buf, len, "$%s%s", num, suffix);
}

static void format_percent(double change, char *buf, size_t len)
{
    const char *arrow = "";
    if (change > 0.05) {
        arrow = LV_SYMBOL_UP;
    } else if (change < -0.05) {
        arrow = LV_SYMBOL_DOWN;
    }
    snprintf(buf, len, "%s%.2f%%", arrow, fabs(change));
}

static lv_obj_t *create_card(lv_obj_t *parent, const char *title, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme ? theme->surface : 0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, DASH_CARD_RADIUS, 0);
    lv_obj_set_style_pad_left(card, 14, 0);
    lv_obj_set_style_pad_right(card, 14, 0);
    lv_obj_set_style_pad_top(card, 10, 0);
    lv_obj_set_style_pad_bottom(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(theme ? theme->accent : 0x00FE8F), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);

    return card;
}

static void coin_row_click_event(lv_event_t *e)
{
    dash_item_row_t *row = (dash_item_row_t *)lv_event_get_user_data(e);
    if (!row || !s_state || row->coin_index >= s_state->watchlist_count) {
        return;
    }
    ui_show_coin_detail(row->coin_index);
}

static void coin_row_long_press_event(lv_event_t *e)
{
    dash_item_row_t *row = (dash_item_row_t *)lv_event_get_user_data(e);
    if (!row || !s_state || row->coin_index >= s_state->watchlist_count) {
        return;
    }
    ui_home_open_holdings_editor(row->coin_index);
}

static void create_item_row(dash_item_row_t *row, lv_obj_t *parent, lv_coord_t y, bool compact)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    if (!row) {
        return;
    }

    memset(row, 0, sizeof(*row));

    lv_coord_t parent_w = lv_obj_get_width(parent);
    lv_coord_t pad_l = lv_obj_get_style_pad_left(parent, LV_PART_MAIN);
    lv_coord_t pad_r = lv_obj_get_style_pad_right(parent, LV_PART_MAIN);
    lv_coord_t content_w = parent_w - pad_l - pad_r;
    if (content_w <= 0) {
        content_w = DASH_COL_W - 28;
    }

    row->btn = lv_btn_create(parent);
    lv_obj_set_pos(row->btn, 0, y);
    lv_obj_set_size(row->btn, content_w, compact ? DASH_ROW_H_COMPACT : DASH_ROW_H);
    lv_obj_set_style_bg_color(row->btn, lv_color_hex(theme ? theme->nav_inactive_bg : 0x151A24), 0);
    lv_obj_set_style_border_width(row->btn, 0, 0);
    lv_obj_set_style_shadow_width(row->btn, 0, 0);
    lv_obj_set_style_shadow_opa(row->btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_ofs_y(row->btn, 0, 0);
    lv_obj_set_style_radius(row->btn, 8, 0);
    lv_obj_set_style_pad_left(row->btn, 10, 0);
    lv_obj_set_style_pad_right(row->btn, 10, 0);
    lv_obj_set_style_pad_top(row->btn, 4, 0);
    lv_obj_set_style_pad_bottom(row->btn, 4, 0);

    row->title = lv_label_create(row->btn);
    lv_label_set_long_mode(row->title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(row->title, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_width(row->title, compact ? 52 : 70);
    lv_obj_align(row->title, LV_ALIGN_LEFT_MID, 0, 0);

    row->symbol = lv_label_create(row->btn);
    lv_label_set_long_mode(row->symbol, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(row->symbol, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_font(row->symbol, &lv_font_montserrat_14, 0);
    lv_obj_set_width(row->symbol, compact ? 96 : 54);
    lv_obj_align_to(row->symbol, row->title, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    row->metric = lv_label_create(row->btn);
    lv_label_set_long_mode(row->metric, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(row->metric, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_width(row->metric, compact ? 70 : 68);
    lv_obj_set_style_text_align(row->metric, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(row->metric, LV_ALIGN_RIGHT_MID, 0, 0);

    row->extra = lv_label_create(row->btn);
    lv_label_set_long_mode(row->extra, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(row->extra, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_width(row->extra, compact ? 92 : 94);
    lv_obj_set_style_text_align(row->extra, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align_to(row->extra, row->metric, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    row->coin_index = SIZE_MAX;

    lv_obj_add_event_cb(row->btn, coin_row_click_event, LV_EVENT_CLICKED, row);
    lv_obj_add_event_cb(row->btn, coin_row_long_press_event, LV_EVENT_LONG_PRESSED, row);
}

static lv_color_t mood_color(int value)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    lv_color_t accent = lv_color_hex(theme ? theme->accent : 0x00FE8F);
    lv_color_t neg = lv_color_hex(0xE74C3C);
    lv_color_t pos = lv_color_hex(0x2ECC71);
    lv_color_t neutral = lv_color_hex(theme ? theme->text_muted : 0x9AA1AD);

    if (value < 20) {
        return neg;
    }
    if (value < 40) {
        return lv_color_mix(neg, neutral, LV_OPA_60);
    }
    if (value < 60) {
        return neutral;
    }
    if (value < 80) {
        return accent;
    }
    return lv_color_mix(pos, accent, LV_OPA_70);
}

static void set_row_interactive(dash_item_row_t *row, bool enabled)
{
    if (!row || !row->btn) {
        return;
    }

    if (enabled) {
        lv_obj_add_flag(row->btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row->btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_bg_opa(row->btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_opa(row->btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(row->btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(row->btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_style_bg_opa(row->btn, LV_OPA_70, 0);
        lv_obj_set_style_text_opa(row->btn, LV_OPA_70, 0);
    }
}

static void update_market_mood_card(void)
{
    if (!s_mood_value || !s_mood_class || !s_mood_arc) {
        return;
    }

    const ui_theme_colors_t *theme = ui_theme_get();
    fng_snapshot_t snapshot = {0};
    fng_service_get_snapshot(&snapshot);

    if (!snapshot.has_value) {
        set_label_text_if_changed(s_mood_value, "N/A");
        set_label_text_if_changed(s_mood_class, "N/A");
        if (s_mood_fallback) {
            set_hidden_if_changed(s_mood_fallback, true);
        }
        lv_arc_set_value(s_mood_arc, 0);
        lv_obj_set_style_arc_color(s_mood_arc, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_mood_value, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
        update_mood_age_label();
        return;
    }

    char value[8];
    snprintf(value, sizeof(value), "%d", snapshot.value);
    set_label_text_if_changed(s_mood_value, value);
    lv_obj_clear_flag(s_mood_value, LV_OBJ_FLAG_HIDDEN);

    char mood[64];
    snprintf(mood, sizeof(mood), "%s", snapshot.classification[0] ? snapshot.classification : "N/A");
    set_label_text_if_changed(s_mood_class, mood);
    lv_obj_clear_flag(s_mood_class, LV_OBJ_FLAG_HIDDEN);

    lv_obj_align_to(s_mood_value, s_mood_arc, LV_ALIGN_CENTER, 0, -28);
    lv_obj_align_to(s_mood_class, s_mood_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_move_foreground(s_mood_value);
    lv_obj_move_foreground(s_mood_class);

    if (s_mood_fallback) {
        if (snapshot.using_fallback) {
            set_hidden_if_changed(s_mood_fallback, false);
        } else {
            set_hidden_if_changed(s_mood_fallback, true);
        }
    }

    lv_color_t color = mood_color(snapshot.value);
    lv_arc_set_value(s_mood_arc, snapshot.value);
    lv_obj_set_style_arc_color(s_mood_arc, color, LV_PART_INDICATOR);
    if (snapshot.stale || snapshot.error) {
        lv_color_t muted = muted_text_color();
        lv_obj_set_style_text_color(s_mood_value, muted, 0);
        lv_obj_set_style_text_color(s_mood_class, muted, 0);
    } else {
        lv_obj_set_style_text_color(s_mood_value, color, 0);
        lv_obj_set_style_text_color(s_mood_class, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    }

    update_mood_age_label();
}

static void update_portfolio_snapshot_card(void)
{
    if (!s_portfolio_total || !s_portfolio_24h || !s_portfolio_7d || !s_portfolio_meta) {
        return;
    }

    if (s_portfolio_demo_label) {
        if (s_state && s_state->prefs.demo_portfolio) {
            lv_obj_clear_flag(s_portfolio_demo_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_portfolio_demo_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    bool show_values = dashboard_show_values();

    if (!s_state || s_state->watchlist_count == 0) {
        set_label_text_if_changed(s_portfolio_total, show_values ? "$0.00" : "Hidden");
        set_label_text_if_changed(s_portfolio_24h, "24h: --");
        set_label_text_if_changed(s_portfolio_7d, "");
        set_label_text_if_changed(s_portfolio_meta, "Holdings: 0 coins");
        return;
    }

    if (!scheduler_is_portfolio_data_ready()) {
        set_label_text_if_changed(s_portfolio_total, show_values ? "Loading..." : "Hidden");
        set_label_text_if_changed(s_portfolio_24h, "24h: --");
        set_hidden_if_changed(s_portfolio_7d, true);
        set_label_text_if_changed(s_portfolio_meta, "Holdings: Loading...");
        return;
    }

    double total = 0.0;
    double weighted_24h = 0.0;
    double weighted_7d = 0.0;
    size_t holdings_count = 0;
    size_t largest_idx = SIZE_MAX;
    double largest_value = 0.0;
    bool has_7d = false;

    for (size_t i = 0; i < s_state->watchlist_count; i++) {
        const coin_t *coin = &s_state->watchlist[i];
        double value = coin->holdings * coin->price;
        total += value;
        weighted_24h += value * coin->change_24h;
        weighted_7d += value * coin->change_7d;

        if (fabs(coin->change_7d) > 0.001) {
            has_7d = true;
        }

        if (coin->holdings > 0.0) {
            holdings_count++;
        }

        if (value > largest_value) {
            largest_value = value;
            largest_idx = i;
        }
    }

    char total_text[32];
    format_usd(total, total_text, sizeof(total_text));
    set_label_text_if_changed(s_portfolio_total, show_values ? total_text : "Hidden");

    char p24[24];
    double change_24h = (total > 0.0) ? (weighted_24h / total) : 0.0;
    format_percent(change_24h, p24, sizeof(p24));
    char line_24[48];
    snprintf(line_24, sizeof(line_24), "24h: %s", p24);
    set_label_text_if_changed(s_portfolio_24h, line_24);
    lv_obj_set_style_text_color(s_portfolio_24h, percent_color(change_24h), 0);

    if (has_7d && total > 0.0) {
        char p7[24];
        double change_7d = weighted_7d / total;
        format_percent(change_7d, p7, sizeof(p7));
        char line_7d[48];
        snprintf(line_7d, sizeof(line_7d), "7d: %s", p7);
        set_label_text_if_changed(s_portfolio_7d, line_7d);
        lv_obj_set_style_text_color(s_portfolio_7d, percent_color(change_7d), 0);
        set_hidden_if_changed(s_portfolio_7d, false);
    } else {
        set_hidden_if_changed(s_portfolio_7d, true);
    }

    char meta[96];
    if (largest_idx != SIZE_MAX && total > 0.0) {
        double pct = (largest_value * 100.0) / total;
        snprintf(meta, sizeof(meta), "Holdings: %u coins | Largest: %s (%.1f%%)",
                 (unsigned)holdings_count,
                 s_state->watchlist[largest_idx].symbol,
                 pct);
    } else {
        snprintf(meta, sizeof(meta), "Holdings: %u coins", (unsigned)holdings_count);
    }
    set_label_text_if_changed(s_portfolio_meta, meta);
}

static void update_bitcoin_dominance_card(void)
{
    if (!s_alt_btc_value || !s_alt_eth_value || !s_alt_others_value ||
        !s_alt_btc_change || !s_alt_eth_change || !s_alt_others_change ||
        !s_alt_track || !s_alt_btc_fill || !s_alt_eth_fill || !s_alt_alt_fill) {
        return;
    }

    const ui_theme_colors_t *theme = ui_theme_get();
    fng_snapshot_t snapshot = {0};
    fng_service_get_snapshot(&snapshot);
    bool stale = snapshot.market_metrics_stale || snapshot.stale;
    lv_color_t muted = muted_text_color();

    if (!snapshot.has_btc_dominance || !snapshot.has_eth_dominance) {
        lv_label_set_text(s_alt_btc_value, "--");
        lv_label_set_text(s_alt_eth_value, "--");
        lv_label_set_text(s_alt_others_value, "--");
        lv_label_set_text(s_alt_btc_change, "--");
        lv_label_set_text(s_alt_eth_change, "--");
        lv_label_set_text(s_alt_others_change, "--");
        lv_obj_set_width(s_alt_btc_fill, 0);
        lv_obj_set_width(s_alt_eth_fill, 0);
        lv_obj_set_width(s_alt_alt_fill, 0);
        return;
    }

    double btc = snapshot.btc_dominance;
    double eth = snapshot.eth_dominance;
    double others = 100.0 - btc - eth;
    if (others < 0.0) {
        others = 0.0;
    }
    if (btc < 0.0) {
        btc = 0.0;
    }
    if (eth < 0.0) {
        eth = 0.0;
    }

    char value_buf[24];
    snprintf(value_buf, sizeof(value_buf), "%.1f%%", btc);
    lv_label_set_text(s_alt_btc_value, value_buf);
    snprintf(value_buf, sizeof(value_buf), "%.1f%%", eth);
    lv_label_set_text(s_alt_eth_value, value_buf);
    snprintf(value_buf, sizeof(value_buf), "%.1f%%", others);
    lv_label_set_text(s_alt_others_value, value_buf);
    lv_obj_set_style_text_color(s_alt_btc_value, stale ? muted : lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_color(s_alt_eth_value, stale ? muted : lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_color(s_alt_others_value, stale ? muted : lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);

    char change_buf[24];
    format_percent(snapshot.btc_dominance_change_24h, change_buf, sizeof(change_buf));
    lv_label_set_text(s_alt_btc_change, change_buf);
    lv_obj_set_style_text_color(s_alt_btc_change, stale ? muted : percent_color(snapshot.btc_dominance_change_24h), 0);

    format_percent(snapshot.eth_dominance_change_24h, change_buf, sizeof(change_buf));
    lv_label_set_text(s_alt_eth_change, change_buf);
    lv_obj_set_style_text_color(s_alt_eth_change, stale ? muted : percent_color(snapshot.eth_dominance_change_24h), 0);

    double others_change = -(snapshot.btc_dominance_change_24h + snapshot.eth_dominance_change_24h);
    format_percent(others_change, change_buf, sizeof(change_buf));
    lv_label_set_text(s_alt_others_change, change_buf);
    lv_obj_set_style_text_color(s_alt_others_change, stale ? muted : percent_color(others_change), 0);

    lv_coord_t track_w = lv_obj_get_width(s_alt_track);
    if (track_w <= 0) {
        track_w = 320;
    }

    lv_coord_t btc_w = (lv_coord_t)llround((btc / 100.0) * track_w);
    lv_coord_t eth_w = (lv_coord_t)llround((eth / 100.0) * track_w);
    if (btc_w < 0) {
        btc_w = 0;
    }
    if (eth_w < 0) {
        eth_w = 0;
    }
    if (btc_w > track_w) {
        btc_w = track_w;
    }
    if (btc_w + eth_w > track_w) {
        eth_w = track_w - btc_w;
    }
    lv_coord_t others_w = track_w - btc_w - eth_w;

    lv_obj_set_size(s_alt_btc_fill, btc_w, 6);
    lv_obj_set_size(s_alt_eth_fill, eth_w, 6);
    lv_obj_set_size(s_alt_alt_fill, others_w, 6);
    lv_obj_set_pos(s_alt_btc_fill, 0, 0);
    lv_obj_set_pos(s_alt_eth_fill, btc_w, 0);
    lv_obj_set_pos(s_alt_alt_fill, btc_w + eth_w, 0);

    (void)theme;
}

static void update_market_strip_card(void)
{
    if (!s_market_cap_value || !s_market_cap_change || !s_btc_price_value || !s_btc_price_change) {
        return;
    }

    const ui_theme_colors_t *theme = ui_theme_get();
    fng_snapshot_t snapshot = {0};
    fng_service_get_snapshot(&snapshot);
    bool market_stale = snapshot.market_metrics_stale || snapshot.stale;
    bool btc_stale = snapshot.btc_price_stale || snapshot.stale;
    lv_color_t muted = muted_text_color();

    char buf[32];
    char pct[24];

    if (snapshot.has_total_market_cap) {
        format_compact_usd(snapshot.total_market_cap, buf, sizeof(buf));
        lv_label_set_text(s_market_cap_value, buf);
        format_percent(snapshot.total_market_cap_change_24h, pct, sizeof(pct));
        lv_label_set_text(s_market_cap_change, pct);
        lv_obj_set_style_text_color(s_market_cap_value,
                                    market_stale ? muted : lv_color_hex(theme ? theme->text_primary : 0xE6E6E6),
                                    0);
        lv_obj_set_style_text_color(s_market_cap_change,
                                    market_stale ? muted : percent_color(snapshot.total_market_cap_change_24h),
                                    0);
    } else {
        lv_label_set_text(s_market_cap_value, "$--");
        lv_label_set_text(s_market_cap_change, "--");
        lv_obj_set_style_text_color(s_market_cap_value, muted, 0);
        lv_obj_set_style_text_color(s_market_cap_change, muted, 0);
    }

    if (snapshot.has_btc_price) {
        format_usd_price(snapshot.btc_price, buf, sizeof(buf));
        lv_label_set_text(s_btc_price_value, buf);
        format_percent(snapshot.btc_price_change_24h, pct, sizeof(pct));
        lv_label_set_text(s_btc_price_change, pct);
        lv_obj_set_style_text_color(s_btc_price_value,
                                    btc_stale ? muted : lv_color_hex(theme ? theme->text_primary : 0xE6E6E6),
                                    0);
        lv_obj_set_style_text_color(s_btc_price_change,
                                    btc_stale ? muted : percent_color(snapshot.btc_price_change_24h),
                                    0);
    } else {
        lv_label_set_text(s_btc_price_value, "$--");
        lv_label_set_text(s_btc_price_change, "--");
        lv_obj_set_style_text_color(s_btc_price_value, muted, 0);
        lv_obj_set_style_text_color(s_btc_price_change, muted, 0);
    }
}

static void set_top_row(size_t row_idx, const coin_t *coin, size_t index, double value)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    bool show_values = dashboard_show_values();
    if (row_idx >= DASH_TOP5_ROWS) {
        return;
    }

    dash_item_row_t *row = &s_top_rows[row_idx];
    if (!row->btn || !row->title || !row->symbol || !row->metric || !row->extra) {
        return;
    }

    if (!coin || index == SIZE_MAX) {
        row->coin_index = SIZE_MAX;
        lv_label_set_text(row->title, "--");
        lv_label_set_text(row->symbol, "");
        lv_label_set_text(row->extra, "");
        lv_label_set_text(row->metric, "");
        lv_obj_set_style_text_color(row->metric, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
        set_row_interactive(row, false);
        return;
    }

    row->coin_index = index;

    lv_label_set_text(row->title, coin->symbol);

    char price_buf[24];
    format_usd_price(coin->price, price_buf, sizeof(price_buf));
    lv_label_set_text(row->symbol, price_buf);

    char value_buf[24];
    format_usd(value, value_buf, sizeof(value_buf));
    lv_label_set_text(row->extra, show_values ? value_buf : "--");

    char pct_buf[24];
    format_percent(coin->change_24h, pct_buf, sizeof(pct_buf));
    lv_label_set_text(row->metric, pct_buf);
    lv_obj_set_style_text_color(row->metric, percent_color(coin->change_24h), 0);

    set_row_interactive(row, true);
}

static void update_top_holdings_card(void)
{
    for (size_t i = 0; i < DASH_TOP5_ROWS; i++) {
        set_top_row(i, NULL, SIZE_MAX, 0.0);
    }

    if (!s_state || s_state->watchlist_count == 0) {
        return;
    }

    size_t count = s_state->watchlist_count;
    if (count > MAX_WATCHLIST) {
        count = MAX_WATCHLIST;
    }

    bool used[MAX_WATCHLIST] = {0};
    for (size_t row = 0; row < DASH_TOP5_ROWS; row++) {
        size_t best_idx = SIZE_MAX;
        double best_value = -INFINITY;

        for (size_t i = 0; i < count; i++) {
            if (used[i]) {
                continue;
            }

            const coin_t *coin = &s_state->watchlist[i];
            if (!is_finite_value(coin->holdings) || !is_finite_value(coin->price)) {
                continue;
            }

            double value = coin->holdings * coin->price;
            if (!is_finite_value(value)) {
                continue;
            }

            if (best_idx == SIZE_MAX || value > best_value) {
                best_idx = i;
                best_value = value;
            }
        }

        if (best_idx == SIZE_MAX) {
            break;
        }

        used[best_idx] = true;
        set_top_row(row, &s_state->watchlist[best_idx], best_idx, best_value);
    }
}

void ui_dashboard_set_state(const app_state_t *state)
{
    s_state = state;
}

static void refresh_dashboard_internal(bool require_active)
{
    if (!s_screen || !lv_obj_is_valid(s_screen)) {
        return;
    }
    if (require_active && lv_scr_act() != s_screen) {
        return;
    }

    update_values_toggle_icon();
    update_market_mood_card();
    update_portfolio_snapshot_card();
    update_bitcoin_dominance_card();
    update_market_strip_card();
    update_top_holdings_card();

    if (s_loading_overlay && lv_obj_is_valid(s_loading_overlay)) {
        lv_obj_move_foreground(s_loading_overlay);
    }

    if (!s_initial_data_loaded && s_loading_overlay) {
        fng_snapshot_t snapshot = {0};
        fng_service_get_snapshot(&snapshot);
        bool portfolio_ready = !s_state || s_state->watchlist_count == 0 || scheduler_is_portfolio_data_ready();
        bool data_ready = snapshot.has_value && portfolio_ready;
        bool timed_out = false;
        if (s_spinner_started_us > 0) {
            int64_t now_us = esp_timer_get_time();
            timed_out = (now_us - s_spinner_started_us) > 12000000;
        }

        if (data_ready || timed_out) {
            s_initial_data_loaded = true;
            lv_obj_del(s_loading_overlay);
            s_loading_overlay = NULL;
            s_loading_spinner = NULL;
            s_loading_label = NULL;
            s_spinner_started_us = 0;
        }
    }
}

void ui_dashboard_refresh(void)
{
    if (!app_state_guard_lock(250)) {
        return;
    }

    refresh_dashboard_internal(true);
    app_state_guard_unlock();
}

void ui_dashboard_prepare_for_show(void)
{
    if (!app_state_guard_lock(250)) {
        return;
    }

    refresh_dashboard_internal(false);
    app_state_guard_unlock();
}

lv_obj_t *ui_dashboard_screen_create(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();

    memset(s_top_rows, 0, sizeof(s_top_rows));
    s_initial_data_loaded = false;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(theme ? theme->bg : 0x0F1117), 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_pad_top(s_screen, UI_NAV_HEIGHT, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_screen, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *mood_card = create_card(s_screen, "Fear & Greed Index", DASH_LEFT_X, DASH_TOP_Y, DASH_COL_W, DASH_MOOD_H);
    s_mood_fallback = lv_label_create(mood_card);
    lv_label_set_text(s_mood_fallback, "(Fallback to alternative.me)");
    lv_obj_set_style_text_color(s_mood_fallback, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_text_font(s_mood_fallback, &lv_font_montserrat_12, 0);
    lv_obj_align(s_mood_fallback, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_add_flag(s_mood_fallback, LV_OBJ_FLAG_HIDDEN);

    s_mood_value = lv_label_create(mood_card);
    lv_label_set_text(s_mood_value, "--");
    lv_obj_set_style_text_font(s_mood_value, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_mood_value, lv_color_hex(theme ? theme->accent : 0x00FE8F), 0);
    lv_obj_set_width(s_mood_value, 120);
    lv_obj_set_style_text_align(s_mood_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mood_value, LV_ALIGN_TOP_LEFT, 4, 28);

    s_mood_class = lv_label_create(mood_card);
    lv_label_set_text(s_mood_class, "--");
    lv_obj_set_style_text_font(s_mood_class, &lv_font_montserrat_16, 0);
    lv_obj_set_style_transform_zoom(s_mood_class, 256, 0);
    lv_obj_set_style_text_color(s_mood_class, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_width(s_mood_class, 180);
    lv_obj_set_style_text_align(s_mood_class, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_mood_class, LV_ALIGN_TOP_LEFT, 0, 88);

    s_mood_updated = lv_label_create(mood_card);
    lv_label_set_text(s_mood_updated, "Updated: --");
    lv_obj_set_style_text_color(s_mood_updated, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_mood_updated, LV_ALIGN_BOTTOM_LEFT, 8, -8);
    lv_obj_add_flag(s_mood_updated, LV_OBJ_FLAG_HIDDEN);

    s_mood_arc = lv_arc_create(mood_card);
    lv_obj_set_size(s_mood_arc, 204, 204);
    lv_obj_align(s_mood_arc, LV_ALIGN_TOP_MID, 0, 30);
    lv_arc_set_range(s_mood_arc, 0, 100);
    lv_arc_set_value(s_mood_arc, 0);
    lv_arc_set_bg_angles(s_mood_arc, 180, 360);
    lv_obj_remove_style(s_mood_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_mood_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_mood_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_mood_arc, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_mood_arc, lv_color_hex(theme ? theme->accent : 0x00FE8F), LV_PART_INDICATOR);
    lv_obj_align_to(s_mood_value, s_mood_arc, LV_ALIGN_CENTER, 0, -28);
    lv_obj_align_to(s_mood_class, s_mood_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_move_foreground(s_mood_value);
    lv_obj_move_foreground(s_mood_class);

    s_loading_overlay = lv_obj_create(s_screen);
    lv_obj_set_size(s_loading_overlay, 228, 96);
    lv_obj_center(s_loading_overlay);
    lv_obj_set_style_bg_color(s_loading_overlay, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_loading_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_color(s_loading_overlay, lv_color_hex(0x2A3142), 0);
    lv_obj_set_style_border_opa(s_loading_overlay, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_loading_overlay, 1, 0);
    lv_obj_set_style_radius(s_loading_overlay, 14, 0);
    lv_obj_set_style_pad_all(s_loading_overlay, 12, 0);
    lv_obj_clear_flag(s_loading_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_loading_overlay, LV_SCROLLBAR_MODE_OFF);
    lv_obj_move_foreground(s_loading_overlay);

    s_loading_spinner = lv_spinner_create(s_loading_overlay, 1600, 72);
    lv_obj_set_size(s_loading_spinner, 34, 34);
    lv_obj_set_style_arc_width(s_loading_spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_loading_spinner, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_loading_spinner, lv_color_hex(0x1F2633), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_loading_spinner, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_loading_spinner, lv_color_hex(theme ? theme->accent : 0x00FE8F), LV_PART_INDICATOR);
    lv_obj_align(s_loading_spinner, LV_ALIGN_TOP_MID, 0, 6);

    s_loading_label = lv_label_create(s_loading_overlay);
    lv_label_set_text(s_loading_label, "Loading market data...");
    lv_obj_set_width(s_loading_label, 196);
    lv_obj_set_style_text_align(s_loading_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_loading_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_loading_label, lv_color_hex(0xE6E6E6), 0);
    lv_obj_align(s_loading_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    s_spinner_started_us = esp_timer_get_time();

    lv_obj_t *footer = lv_obj_create(s_screen);
    lv_obj_set_size(footer, 800, 26);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_all(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

    s_footer_updated = lv_label_create(footer);
    lv_label_set_text(s_footer_updated, "Updated: --");
    lv_obj_set_style_text_color(s_footer_updated, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_width(s_footer_updated, 760);
    lv_obj_set_style_text_align(s_footer_updated, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_footer_updated, LV_ALIGN_RIGHT_MID, -16, 0);

    if (s_footer_timer) {
        lv_timer_del(s_footer_timer);
        s_footer_timer = NULL;
    }
    s_footer_timer = lv_timer_create(footer_timer_cb, 5000, NULL);

    lv_obj_t *highlights = create_card(s_screen, "Bitcoin Dominance", DASH_RIGHT_X,
                                       DASH_TOP_Y + DASH_PORTFOLIO_H + DASH_GAP_Y,
                                       DASH_COL_W, DASH_HIGHLIGHTS_H);

    s_alt_left = lv_label_create(highlights);
    lv_label_set_text(s_alt_left, "Bitcoin");
    lv_obj_set_style_text_color(s_alt_left, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_left, LV_ALIGN_TOP_LEFT, 0, 28);

    s_alt_btc_dot = lv_obj_create(highlights);
    lv_obj_set_size(s_alt_btc_dot, 6, 6);
    lv_obj_set_style_radius(s_alt_btc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_alt_btc_dot, lv_color_hex(0xF39C3D), 0);
    lv_obj_set_style_border_width(s_alt_btc_dot, 0, 0);
    lv_obj_set_style_pad_all(s_alt_btc_dot, 0, 0);
    lv_obj_clear_flag(s_alt_btc_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(s_alt_btc_dot, s_alt_left, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    s_alt_mid = lv_label_create(highlights);
    lv_label_set_text(s_alt_mid, "Ethereum");
    lv_obj_set_style_text_color(s_alt_mid, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_mid, LV_ALIGN_TOP_MID, 0, 28);

    s_alt_eth_dot = lv_obj_create(highlights);
    lv_obj_set_size(s_alt_eth_dot, 6, 6);
    lv_obj_set_style_radius(s_alt_eth_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_alt_eth_dot, lv_color_hex(0x4B6CFF), 0);
    lv_obj_set_style_border_width(s_alt_eth_dot, 0, 0);
    lv_obj_set_style_pad_all(s_alt_eth_dot, 0, 0);
    lv_obj_clear_flag(s_alt_eth_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(s_alt_eth_dot, s_alt_mid, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    s_alt_right = lv_label_create(highlights);
    lv_label_set_text(s_alt_right, "Others");
    lv_obj_set_style_text_color(s_alt_right, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_right, LV_ALIGN_TOP_RIGHT, 0, 28);

    s_alt_others_dot = lv_obj_create(highlights);
    lv_obj_set_size(s_alt_others_dot, 6, 6);
    lv_obj_set_style_radius(s_alt_others_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_alt_others_dot, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_border_width(s_alt_others_dot, 0, 0);
    lv_obj_set_style_pad_all(s_alt_others_dot, 0, 0);
    lv_obj_clear_flag(s_alt_others_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align_to(s_alt_others_dot, s_alt_right, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    s_alt_btc_value = lv_label_create(highlights);
    lv_label_set_text(s_alt_btc_value, "--");
    lv_obj_set_style_text_font(s_alt_btc_value, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_alt_btc_value, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_align(s_alt_btc_value, LV_ALIGN_TOP_LEFT, 0, 46);

    s_alt_eth_value = lv_label_create(highlights);
    lv_label_set_text(s_alt_eth_value, "--");
    lv_obj_set_style_text_font(s_alt_eth_value, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_alt_eth_value, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_align(s_alt_eth_value, LV_ALIGN_TOP_MID, 0, 46);

    s_alt_others_value = lv_label_create(highlights);
    lv_label_set_text(s_alt_others_value, "--");
    lv_obj_set_style_text_font(s_alt_others_value, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_alt_others_value, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_align(s_alt_others_value, LV_ALIGN_TOP_RIGHT, 0, 46);

    s_alt_btc_change = lv_label_create(highlights);
    lv_label_set_text(s_alt_btc_change, "--");
    lv_obj_set_style_text_color(s_alt_btc_change, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_btc_change, LV_ALIGN_TOP_LEFT, 0, 76);

    s_alt_eth_change = lv_label_create(highlights);
    lv_label_set_text(s_alt_eth_change, "--");
    lv_obj_set_style_text_color(s_alt_eth_change, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_eth_change, LV_ALIGN_TOP_MID, 0, 76);

    s_alt_others_change = lv_label_create(highlights);
    lv_label_set_text(s_alt_others_change, "--");
    lv_obj_set_style_text_color(s_alt_others_change, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_alt_others_change, LV_ALIGN_TOP_RIGHT, 0, 76);

    s_alt_track = lv_obj_create(highlights);
    lv_obj_set_size(s_alt_track, DASH_COL_W - 28, 6);
    lv_obj_align(s_alt_track, LV_ALIGN_BOTTOM_LEFT, 0, -6);
    lv_obj_set_style_bg_color(s_alt_track, lv_color_hex(theme ? theme->nav_inactive_bg : 0x212121), 0);
    lv_obj_set_style_border_width(s_alt_track, 0, 0);
    lv_obj_set_style_radius(s_alt_track, 3, 0);
    lv_obj_set_style_pad_all(s_alt_track, 0, 0);
    lv_obj_clear_flag(s_alt_track, LV_OBJ_FLAG_SCROLLABLE);

    s_alt_btc_fill = lv_obj_create(s_alt_track);
    lv_obj_set_size(s_alt_btc_fill, 0, 6);
    lv_obj_set_style_bg_color(s_alt_btc_fill, lv_color_hex(0xF39C3D), 0);
    lv_obj_set_style_border_width(s_alt_btc_fill, 0, 0);
    lv_obj_set_style_radius(s_alt_btc_fill, 3, 0);

    s_alt_eth_fill = lv_obj_create(s_alt_track);
    lv_obj_set_size(s_alt_eth_fill, 0, 6);
    lv_obj_set_style_bg_color(s_alt_eth_fill, lv_color_hex(0x4B6CFF), 0);
    lv_obj_set_style_border_width(s_alt_eth_fill, 0, 0);
    lv_obj_set_style_radius(s_alt_eth_fill, 3, 0);

    s_alt_alt_fill = lv_obj_create(s_alt_track);
    lv_obj_set_size(s_alt_alt_fill, 0, 6);
    lv_obj_set_style_bg_color(s_alt_alt_fill, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_border_width(s_alt_alt_fill, 0, 0);
    lv_obj_set_style_radius(s_alt_alt_fill, 3, 0);

    lv_obj_t *market_strip = lv_obj_create(s_screen);
    lv_obj_set_pos(market_strip,
                   DASH_RIGHT_X,
                   DASH_TOP_Y + DASH_PORTFOLIO_H + DASH_GAP_Y + DASH_HIGHLIGHTS_H + DASH_GAP_Y);
    lv_obj_set_size(market_strip, DASH_COL_W, DASH_MARKET_STRIP_H);
    lv_obj_set_style_bg_color(market_strip, lv_color_hex(theme ? theme->surface : 0x1A1D26), 0);
    lv_obj_set_style_border_width(market_strip, 0, 0);
    lv_obj_set_style_radius(market_strip, DASH_CARD_RADIUS, 0);
    lv_obj_set_style_pad_left(market_strip, 12, 0);
    lv_obj_set_style_pad_right(market_strip, 12, 0);
    lv_obj_set_style_pad_top(market_strip, 6, 0);
    lv_obj_set_style_pad_bottom(market_strip, 6, 0);
    lv_obj_clear_flag(market_strip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mcap_col = lv_obj_create(market_strip);
    lv_obj_set_size(mcap_col, 170, 44);
    lv_obj_align(mcap_col, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(mcap_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mcap_col, 0, 0);
    lv_obj_set_style_pad_all(mcap_col, 0, 0);
    lv_obj_clear_flag(mcap_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mcap_label = lv_label_create(mcap_col);
    lv_label_set_text(mcap_label, "Market Cap");
    lv_obj_set_style_text_color(mcap_label, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_text_font(mcap_label, &lv_font_montserrat_12, 0);
    lv_obj_align(mcap_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_market_cap_value = lv_label_create(mcap_col);
    lv_label_set_text(s_market_cap_value, "$--");
    lv_obj_set_style_text_color(s_market_cap_value, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_market_cap_value, &lv_font_montserrat_22, 0);
    lv_obj_set_width(s_market_cap_value, 118);
    lv_obj_set_style_text_align(s_market_cap_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_market_cap_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_market_cap_change = lv_label_create(mcap_col);
    lv_label_set_text(s_market_cap_change, "--");
    lv_obj_set_style_text_color(s_market_cap_change, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_label_set_long_mode(s_market_cap_change, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_market_cap_change, 60);
    lv_obj_set_style_text_align(s_market_cap_change, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_market_cap_change, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *btc_col = lv_obj_create(market_strip);
    lv_obj_set_size(btc_col, 170, 44);
    lv_obj_align(btc_col, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(btc_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btc_col, 0, 0);
    lv_obj_set_style_pad_all(btc_col, 0, 0);
    lv_obj_clear_flag(btc_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btc_dot = lv_obj_create(btc_col);
    lv_obj_set_size(btc_dot, 6, 6);
    lv_obj_set_style_radius(btc_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btc_dot, lv_color_hex(0xF39C3D), 0);
    lv_obj_set_style_border_width(btc_dot, 0, 0);
    lv_obj_set_style_pad_all(btc_dot, 0, 0);
    lv_obj_clear_flag(btc_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btc_dot, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *btc_label = lv_label_create(btc_col);
    lv_label_set_text(btc_label, "BTC");
    lv_obj_set_style_text_color(btc_label, lv_color_hex(0xF39C3D), 0);
    lv_obj_set_style_text_font(btc_label, &lv_font_montserrat_12, 0);
    lv_obj_align_to(btc_label, btc_dot, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    s_btc_price_value = lv_label_create(btc_col);
    lv_label_set_text(s_btc_price_value, "$--");
    lv_obj_set_style_text_color(s_btc_price_value, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_btc_price_value, &lv_font_montserrat_22, 0);
    lv_obj_set_width(s_btc_price_value, 118);
    lv_obj_set_style_text_align(s_btc_price_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_btc_price_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    s_btc_price_change = lv_label_create(btc_col);
    lv_label_set_text(s_btc_price_change, "--");
    lv_obj_set_style_text_color(s_btc_price_change, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_label_set_long_mode(s_btc_price_change, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_btc_price_change, 60);
    lv_obj_set_style_text_align(s_btc_price_change, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_btc_price_change, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *portfolio = create_card(s_screen, "Portfolio Snapshot", DASH_RIGHT_X, DASH_TOP_Y, DASH_COL_W, DASH_PORTFOLIO_H);
    lv_obj_t *portfolio_title = lv_obj_get_child(portfolio, 0);

    s_portfolio_demo_label = lv_label_create(portfolio);
    lv_label_set_text(s_portfolio_demo_label, "(Demo Portfolio)");
    lv_obj_set_style_text_color(s_portfolio_demo_label, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_set_style_text_font(s_portfolio_demo_label, &lv_font_montserrat_12, 0);
    if (portfolio_title) {
        lv_obj_align_to(s_portfolio_demo_label, portfolio_title, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    } else {
        lv_obj_align(s_portfolio_demo_label, LV_ALIGN_TOP_LEFT, 140, 2);
    }
    lv_obj_add_flag(s_portfolio_demo_label, LV_OBJ_FLAG_HIDDEN);

    s_values_toggle_btn = lv_btn_create(portfolio);
    lv_obj_set_size(s_values_toggle_btn, 44, 34);
    lv_obj_align(s_values_toggle_btn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_values_toggle_btn, 10, 0);
    lv_obj_set_style_border_width(s_values_toggle_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_values_toggle_btn, 0, 0);
    lv_obj_set_style_pad_all(s_values_toggle_btn, 0, 0);
    lv_obj_clear_flag(s_values_toggle_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_values_toggle_btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_values_toggle_btn, values_toggle_event, LV_EVENT_CLICKED, NULL);

    s_values_toggle_icon = lv_label_create(s_values_toggle_btn);
    lv_label_set_text(s_values_toggle_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(s_values_toggle_icon, &lv_font_montserrat_22, 0);
    lv_obj_center(s_values_toggle_icon);

    s_portfolio_total = lv_label_create(portfolio);
    lv_label_set_text(s_portfolio_total, "$0.00");
    lv_obj_set_style_text_font(s_portfolio_total, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(s_portfolio_total, lv_color_hex(theme ? theme->text_primary : 0xE6E6E6), 0);
    lv_obj_align(s_portfolio_total, LV_ALIGN_TOP_LEFT, 0, 30);

    s_portfolio_24h = lv_label_create(portfolio);
    lv_label_set_text(s_portfolio_24h, "24h: --");
    lv_obj_set_style_text_color(s_portfolio_24h, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_portfolio_24h, LV_ALIGN_TOP_LEFT, 4, 76);

    s_portfolio_7d = lv_label_create(portfolio);
    lv_label_set_text(s_portfolio_7d, "7d: --");
    lv_obj_set_style_text_color(s_portfolio_7d, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_portfolio_7d, LV_ALIGN_TOP_LEFT, 112, 76);

    s_portfolio_meta = lv_label_create(portfolio);
    lv_label_set_text(s_portfolio_meta, "Holdings: 0 coins");
    lv_obj_set_style_text_color(s_portfolio_meta, lv_color_hex(theme ? theme->text_muted : 0x9AA1AD), 0);
    lv_obj_align(s_portfolio_meta, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *top5 = create_card(s_screen, "Top 5 Holdings", DASH_LEFT_X,
                                 DASH_TOP_Y + DASH_MOOD_H + DASH_GAP_Y,
                                 DASH_COL_W, DASH_TOP5_H);

    for (size_t i = 0; i < DASH_TOP5_ROWS; i++) {
        create_item_row(&s_top_rows[i], top5, 30 + (lv_coord_t)(i * (DASH_ROW_H_COMPACT + DASH_LIST_GAP)), true);
    }

    ui_nav_attach_with_home_label(s_screen, UI_NAV_DASHBOARD, "CoinWatch");

    ui_dashboard_refresh();
    return s_screen;
}
