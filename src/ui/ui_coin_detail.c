#include "ui/ui_coin_detail.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/coingecko_client.h"
#include "services/display_driver.h"
#include "ui/ui.h"
#include "ui/ui_nav.h"
#include "ui/ui_theme.h"


typedef enum {
    RANGE_1H = 0,
    RANGE_24H,
    RANGE_7D,
    RANGE_30D,
    RANGE_1Y
} chart_range_t;

typedef struct {
    char coin_id[32];
    chart_range_t range;
} chart_task_ctx_t;

#define CHIP_COUNT 5
#define CHIP_WIDTH 100
#define CHIP_GAP 12
#define CHIP_AREA_WIDTH (CHIP_COUNT * CHIP_WIDTH + (CHIP_COUNT - 1) * CHIP_GAP)
#define Y_LABEL_COUNT 6
#define X_LABEL_COUNT 12
#define Y_AXIS_WIDTH 56
#define CHART_POINT_SIZE 4

static app_state_t *s_state = NULL;
static size_t s_coin_index = 0;
static chart_range_t s_range = RANGE_24H;
static bool s_loading = false;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_chip_container = NULL;
static lv_obj_t *s_chip_boxes[CHIP_COUNT] = {0};
static lv_obj_t *s_chip_labels[CHIP_COUNT] = {0};
static lv_obj_t *s_holdings = NULL;
static lv_obj_t *s_chart = NULL;
static lv_chart_series_t *s_series = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_range_buttons[4] = {0};
static const chart_range_t s_range_order[4] = {RANGE_1H, RANGE_24H, RANGE_7D, RANGE_1Y};
static lv_obj_t *s_y_axis = NULL;
static lv_obj_t *s_y_labels[Y_LABEL_COUNT] = {0};
static lv_obj_t *s_x_axis = NULL;
static lv_obj_t *s_x_labels[X_LABEL_COUNT] = {0};
static lv_obj_t *s_tooltip = NULL;
static lv_obj_t *s_tooltip_label = NULL;
static lv_obj_t *s_guideline = NULL;
static const chart_point_t *s_chart_points = NULL;
static size_t s_chart_count = 0;

static lv_color_t percent_color(double change)
{
    if (change > 0.05) {
        return lv_color_hex(0x2ECC71);
    }
    if (change < -0.05) {
        return lv_color_hex(0xE74C3C);
    }
    return lv_color_hex(0x9AA1AD);
}

static void format_usd_price(double price, char *buf, size_t len);

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

static void format_time_label(int64_t ts_ms, char *buf, size_t len)
{
    if (len == 0) {
        return;
    }

    time_t ts = (time_t)(ts_ms / 1000);
    struct tm timeinfo;
    if (!localtime_r(&ts, &timeinfo)) {
        snprintf(buf, len, "--");
        return;
    }

    const char *fmt = (s_range == RANGE_1H || s_range == RANGE_24H) ? "%H:%M" : "%m/%d";
    strftime(buf, len, fmt, &timeinfo);
}

static void update_axis_labels(double min, double max)
{
    if (!s_chart || !s_y_axis || !s_x_axis || !s_chart_points || s_chart_count == 0) {
        return;
    }

    char price[24];
    for (int i = 0; i < Y_LABEL_COUNT; i++) {
        double t = (double)i / (double)(Y_LABEL_COUNT - 1);
        double value = max - ((max - min) * t);
        format_usd_price(value, price, sizeof(price));
        lv_label_set_text(s_y_labels[i], price);
    }

    char timebuf[24];
    for (int i = 0; i < X_LABEL_COUNT; i++) {
        size_t idx = 0;
        if (s_chart_count > 1) {
            idx = (size_t)((i * (s_chart_count - 1)) / (X_LABEL_COUNT - 1));
        }
        format_time_label(s_chart_points[idx].ts_ms, timebuf, sizeof(timebuf));
        lv_label_set_text(s_x_labels[i], timebuf);
    }

    lv_coord_t axis_h = lv_obj_get_height(s_y_axis);
    lv_coord_t axis_w = lv_obj_get_width(s_y_axis);
    for (int i = 0; i < Y_LABEL_COUNT; i++) {
        lv_coord_t label_h = lv_obj_get_height(s_y_labels[i]);
        lv_obj_set_width(s_y_labels[i], axis_w);
        lv_obj_set_style_text_align(s_y_labels[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_coord_t y = (axis_h - label_h) * i / (Y_LABEL_COUNT - 1);
        lv_obj_set_pos(s_y_labels[i], 0, y);
    }

    lv_coord_t x_axis_w = lv_obj_get_width(s_x_axis);
    for (int i = 0; i < X_LABEL_COUNT; i++) {
        lv_coord_t label_w = lv_obj_get_width(s_x_labels[i]);
        lv_coord_t x = (x_axis_w * i) / (X_LABEL_COUNT - 1) - (label_w / 2);
        if (x < 0) {
            x = 0;
        }
        if (x + label_w > x_axis_w) {
            x = x_axis_w - label_w;
        }
        lv_obj_set_pos(s_x_labels[i], x, 0);
    }
}

static void update_chip(int index, double change)
{
    if (index < 0 || index >= CHIP_COUNT || !s_chip_boxes[index] || !s_chip_labels[index]) {
        return;
    }

    char text[16];
    format_percent(change, text, sizeof(text));
    lv_label_set_text(s_chip_labels[index], text);

    lv_color_t color = percent_color(change);
    lv_obj_set_style_text_color(s_chip_labels[index], color, 0);
    lv_obj_set_style_border_color(s_chip_boxes[index], color, 0);

    lv_color_t bg = lv_color_hex(0x10141C);
    lv_opa_t opa = LV_OPA_40;
    if (change > 0.05) {
        bg = lv_color_hex(0x14351F);
        opa = LV_OPA_60;
    } else if (change < -0.05) {
        bg = lv_color_hex(0x35161B);
        opa = LV_OPA_60;
    }
    lv_obj_set_style_bg_color(s_chip_boxes[index], bg, 0);
    lv_obj_set_style_bg_opa(s_chip_boxes[index], opa, 0);
}

static void format_usd(double price, char *buf, size_t len)
{
    if (price <= 0.0) {
        snprintf(buf, len, "$0.00");
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

static void format_usd_price(double price, char *buf, size_t len)
{
    if (price <= 0.0) {
        snprintf(buf, len, "$0.00");
        return;
    }

    if (price >= 1.0) {
        int decimals = 2;
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%.*f", decimals, price);

        const char *dot = strchr(tmp, '.');
        size_t int_len = dot ? (size_t)(dot - tmp) : strlen(tmp);
        size_t commas = int_len > 3 ? (int_len - 1) / 3 : 0;
        size_t needed = 1 + int_len + commas + (dot ? strlen(dot) : 0) + 1;
        if (needed > len) {
            snprintf(buf, len, "$%.*f", decimals, price);
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

static void update_header_values(void)
{
    if (!s_state || s_coin_index >= s_state->watchlist_count) {
        return;
    }

    const coin_t *coin = &s_state->watchlist[s_coin_index];
    char price[32];
    format_usd_price(coin->price, price, sizeof(price));

    char title[96];
    snprintf(title, sizeof(title), "%s (%s) | %s", coin->name, coin->symbol, price);
    lv_label_set_text(s_title, title);

    update_chip(0, coin->change_1h);
    update_chip(1, coin->change_24h);
    update_chip(2, coin->change_7d);
    update_chip(3, coin->change_30d);
    update_chip(4, coin->change_1y);

    char holdings[32];
    format_number_commas_trim(coin->holdings, 6, holdings, sizeof(holdings));

    char value[32];
    format_usd_price(coin->price * coin->holdings, value, sizeof(value));
    format_trim_zeros(value);

    char holdings_line[80];
    snprintf(holdings_line, sizeof(holdings_line), "Holdings: %s | %s", holdings, value);
    lv_label_set_text(s_holdings, holdings_line);
}

static int range_to_days(chart_range_t range)
{
    switch (range) {
        case RANGE_1H:
        case RANGE_24H:
            return 1;
        case RANGE_7D:
            return 7;
        case RANGE_30D:
            return 30;
        case RANGE_1Y:
        default:
            return 365;
    }
}

static void update_range_buttons(void)
{
    for (int i = 0; i < 4; i++) {
        if (!s_range_buttons[i]) {
            continue;
        }
        bool active = (s_range_order[i] == s_range);
        lv_obj_set_style_bg_color(s_range_buttons[i], active ? lv_color_hex(0x2A3142) : lv_color_hex(0x151A24), 0);
        lv_obj_set_style_text_color(s_range_buttons[i], active ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x9AA1AD), 0);
    }
}

static void show_tooltip(size_t index)
{
    if (!s_chart_points || s_chart_count == 0 || index >= s_chart_count) {
        return;
    }

    if (!s_guideline && s_chart) {
        s_guideline = lv_obj_create(s_chart);
        lv_obj_set_style_bg_color(s_guideline, lv_color_hex(0x2A3142), 0);
        lv_obj_set_style_border_width(s_guideline, 0, 0);
    }

    if (!s_tooltip) {
        s_tooltip = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_tooltip, 140, 44);
        lv_obj_set_style_bg_color(s_tooltip, lv_color_hex(0x1A1D26), 0);
        lv_obj_set_style_border_width(s_tooltip, 0, 0);
        lv_obj_set_style_radius(s_tooltip, 8, 0);
        lv_obj_set_style_pad_left(s_tooltip, 8, 0);
        lv_obj_set_style_pad_right(s_tooltip, 8, 0);
        lv_obj_set_style_pad_top(s_tooltip, 6, 0);
        lv_obj_set_style_pad_bottom(s_tooltip, 6, 0);

        s_tooltip_label = lv_label_create(s_tooltip);
        lv_obj_set_style_text_color(s_tooltip_label, lv_color_hex(0xE6E6E6), 0);
        lv_obj_center(s_tooltip_label);
    }

    char price[32];
    format_usd(s_chart_points[index].price, price, sizeof(price));
    lv_label_set_text(s_tooltip_label, price);

    lv_coord_t chart_w = s_chart ? lv_obj_get_width(s_chart) : 760;
    lv_coord_t chart_h = s_chart ? lv_obj_get_height(s_chart) : 200;
    lv_coord_t rel_x = (lv_coord_t)((index * chart_w) / (s_chart_count > 1 ? (s_chart_count - 1) : 1));
    if (s_guideline) {
        lv_obj_set_size(s_guideline, 2, chart_h);
        lv_obj_set_pos(s_guideline, rel_x, 0);
        lv_obj_clear_flag(s_guideline, LV_OBJ_FLAG_HIDDEN);
    }

    lv_coord_t x = 16 + rel_x;
    lv_coord_t y = 150;
    if (x > 640) {
        x = 640;
    }
    lv_obj_align(s_tooltip, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_clear_flag(s_tooltip, LV_OBJ_FLAG_HIDDEN);
}

static void hide_tooltip(void)
{
    if (s_tooltip) {
        lv_obj_add_flag(s_tooltip, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_guideline) {
        lv_obj_add_flag(s_guideline, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_chart_data(const chart_point_t *points, size_t count)
{
    if (!s_chart || !s_series || !points || count == 0) {
        return;
    }

    int64_t latest_ts = points[count - 1].ts_ms;
    int64_t cutoff = 0;
    if (s_range == RANGE_1H) {
        cutoff = latest_ts - 60LL * 60LL * 1000LL;
    } else if (s_range == RANGE_24H) {
        cutoff = latest_ts - 24LL * 60LL * 60LL * 1000LL;
    }

    size_t start = 0;
    if (cutoff > 0) {
        for (size_t i = 0; i < count; i++) {
            if (points[i].ts_ms >= cutoff) {
                start = i;
                break;
            }
        }
    }

    size_t range_count = count - start;
    if (range_count < 2) {
        return;
    }

    lv_chart_set_point_count(s_chart, (uint16_t)range_count);
    s_chart_points = points + start;
    s_chart_count = range_count;

    double min = points[start].price;
    double max = points[start].price;
    for (size_t i = start; i < count; i++) {
        if (points[i].price < min) {
            min = points[i].price;
        }
        if (points[i].price > max) {
            max = points[i].price;
        }
    }

    int scale = 100;
    int32_t min_val = (int32_t)(min * scale);
    int32_t max_val = (int32_t)(max * scale);
    if (min_val == max_val) {
        max_val += 1;
    }

    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, min_val, max_val);

    for (size_t i = 0; i < range_count; i++) {
        int32_t value = (int32_t)(points[start + i].price * scale);
        lv_chart_set_value_by_id(s_chart, s_series, (uint16_t)i, value);
    }

    lv_chart_refresh(s_chart);
    update_axis_labels(min, max);
}

static void chart_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_get_act(), &p);

        lv_area_t area;
        lv_obj_get_coords(s_chart, &area);
        if (p.x < area.x1 || p.x > area.x2) {
            return;
        }

        lv_coord_t rel_x = p.x - area.x1;
        size_t idx = 0;
        if (s_chart_count > 1) {
            idx = (size_t)((rel_x * (s_chart_count - 1)) / (area.x2 - area.x1));
        }
        show_tooltip(idx);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hide_tooltip();
    }
}

static void chart_ready_cb(void *arg)
{
    chart_task_ctx_t *ctx = (chart_task_ctx_t *)arg;
    if (!ctx) {
        return;
    }

    if (!s_state || s_coin_index >= s_state->watchlist_count) {
        free(ctx);
        return;
    }

    const coin_t *coin = &s_state->watchlist[s_coin_index];
    if (strcmp(ctx->coin_id, coin->id) != 0 || ctx->range != s_range) {
        free(ctx);
        return;
    }

    const chart_point_t *points = NULL;
    size_t count = 0;
    if (coingecko_client_get_chart(ctx->coin_id, range_to_days(ctx->range), &points, &count) == ESP_OK) {
        set_chart_data(points, count);
        lv_label_set_text(s_status, "");
    } else {
        lv_label_set_text(s_status, "Chart load failed");
    }

    s_loading = false;
    free(ctx);
}

static void chart_task(void *arg)
{
    chart_task_ctx_t *ctx = (chart_task_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    const chart_point_t *points = NULL;
    size_t count = 0;
    coingecko_client_get_chart(ctx->coin_id, range_to_days(ctx->range), &points, &count);
    lv_async_call(chart_ready_cb, ctx);
    vTaskDelete(NULL);
}

static void request_chart(void)
{
    if (!s_state || s_coin_index >= s_state->watchlist_count || s_loading) {
        return;
    }

    const coin_t *coin = &s_state->watchlist[s_coin_index];
    chart_task_ctx_t *ctx = calloc(1, sizeof(chart_task_ctx_t));
    if (!ctx) {
        return;
    }

    strncpy(ctx->coin_id, coin->id, sizeof(ctx->coin_id) - 1);
    ctx->range = s_range;
    s_loading = true;
    lv_label_set_text(s_status, "Loading chart...");

    xTaskCreate(chart_task, "chart_fetch", 6144, ctx, 5, NULL);
}

static void range_event(lv_event_t *e)
{
    chart_range_t range = (chart_range_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_range == range) {
        return;
    }
    s_range = range;
    update_range_buttons();
    request_chart();
}

void ui_coin_detail_set_state(app_state_t *state)
{
    s_state = state;
}

void ui_coin_detail_show_index(size_t index)
{
    s_coin_index = index;
    update_header_values();
    update_range_buttons();
    request_chart();
}

lv_obj_t *ui_coin_detail_screen_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(s_screen, UI_NAV_HEIGHT, 0);
    const ui_theme_colors_t *theme = ui_theme_get();
    uint32_t accent = theme ? theme->accent : 0x00FE8F;

    s_title = lv_label_create(s_screen);
    lv_label_set_text(s_title, "Coin Detail | $0.00");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_22, 0);
    lv_obj_set_width(s_title, 760);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_RIGHT, -16, 4);

    lv_obj_t *chip_labels = lv_obj_create(s_screen);
    lv_obj_set_size(chip_labels, CHIP_AREA_WIDTH, 18);
    lv_obj_align(chip_labels, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_bg_opa(chip_labels, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chip_labels, 0, 0);
    lv_obj_set_style_pad_all(chip_labels, 0, 0);
    lv_obj_set_flex_flow(chip_labels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip_labels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip_labels, CHIP_GAP, 0);
    lv_obj_clear_flag(chip_labels, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(chip_labels, LV_SCROLLBAR_MODE_OFF);

    static const char *chip_ranges[CHIP_COUNT] = {"Hour", "Day", "Week", "Month", "Year"};
    for (int i = 0; i < CHIP_COUNT; i++) {
        lv_obj_t *label = lv_label_create(chip_labels);
        lv_label_set_text(label, chip_ranges[i]);
        lv_obj_set_style_text_color(label, lv_color_hex(accent), 0);
        lv_obj_set_width(label, CHIP_WIDTH);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }

    s_chip_container = lv_obj_create(s_screen);
    lv_obj_set_size(s_chip_container, CHIP_AREA_WIDTH, 28);
    lv_obj_align(s_chip_container, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_bg_opa(s_chip_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chip_container, 0, 0);
    lv_obj_set_style_pad_all(s_chip_container, 0, 0);
    lv_obj_set_flex_flow(s_chip_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chip_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_chip_container, CHIP_GAP, 0);
    lv_obj_clear_flag(s_chip_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_chip_container, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < CHIP_COUNT; i++) {
        lv_obj_t *chip = lv_obj_create(s_chip_container);
        lv_obj_set_size(chip, CHIP_WIDTH, 24);
        lv_obj_set_style_bg_color(chip, lv_color_hex(0x10141C), 0);
        lv_obj_set_style_border_width(chip, 1, 0);
        lv_obj_set_style_border_color(chip, lv_color_hex(0x2A3142), 0);
        lv_obj_set_style_radius(chip, 12, 0);
        lv_obj_set_style_pad_left(chip, 8, 0);
        lv_obj_set_style_pad_right(chip, 8, 0);
        lv_obj_set_style_pad_top(chip, 4, 0);
        lv_obj_set_style_pad_bottom(chip, 4, 0);
        lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(chip, LV_SCROLLBAR_MODE_OFF);

        lv_obj_t *label = lv_label_create(chip);
        lv_label_set_text(label, "--");
        lv_obj_set_style_text_color(label, lv_color_hex(0x9AA1AD), 0);
        lv_obj_center(label);

        s_chip_boxes[i] = chip;
        s_chip_labels[i] = label;
    }

    s_holdings = lv_label_create(s_screen);
    lv_label_set_text(s_holdings, "Holdings: 0");
    lv_obj_set_style_text_color(s_holdings, lv_color_hex(0x9AA1AD), 0);
    lv_obj_set_width(s_holdings, 760);
    lv_obj_set_style_text_align(s_holdings, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_holdings, LV_ALIGN_TOP_RIGHT, -16, 32);

    s_chart = lv_chart_create(s_screen);
    lv_obj_set_size(s_chart, 700, 200);
    lv_obj_align(s_chart, LV_ALIGN_TOP_LEFT, 16, 146);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_point_count(s_chart, 30);
    lv_obj_set_style_size(s_chart, CHART_POINT_SIZE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x10141C), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_add_event_cb(s_chart, chart_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_chart, chart_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_chart, chart_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_chart, chart_event, LV_EVENT_PRESS_LOST, NULL);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(theme ? theme->accent : 0x4F77FF), LV_CHART_AXIS_PRIMARY_Y);

    s_y_axis = lv_obj_create(s_screen);
    lv_obj_set_size(s_y_axis, Y_AXIS_WIDTH, 200);
    lv_obj_set_style_bg_opa(s_y_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_y_axis, 0, 0);
    lv_obj_set_style_pad_all(s_y_axis, 0, 0);
    lv_obj_clear_flag(s_y_axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_y_axis, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align_to(s_y_axis, s_chart, LV_ALIGN_OUT_RIGHT_TOP, 8, 0);

    for (int i = 0; i < Y_LABEL_COUNT; i++) {
        s_y_labels[i] = lv_label_create(s_y_axis);
        lv_label_set_text(s_y_labels[i], "--");
        lv_obj_set_style_text_color(s_y_labels[i], lv_color_hex(0x6B717B), 0);
        lv_obj_set_style_text_font(s_y_labels[i], &lv_font_montserrat_12, 0);
    }

    s_x_axis = lv_obj_create(s_screen);
    lv_obj_set_size(s_x_axis, 700, 18);
    lv_obj_set_style_bg_opa(s_x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_x_axis, 0, 0);
    lv_obj_set_style_pad_all(s_x_axis, 0, 0);
    lv_obj_clear_flag(s_x_axis, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_x_axis, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align_to(s_x_axis, s_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    for (int i = 0; i < X_LABEL_COUNT; i++) {
        s_x_labels[i] = lv_label_create(s_x_axis);
        lv_label_set_text(s_x_labels[i], "--");
        lv_obj_set_style_text_color(s_x_labels[i], lv_color_hex(0x6B717B), 0);
        lv_obj_set_style_text_font(s_x_labels[i], &lv_font_montserrat_12, 0);
    }

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_LEFT, 16, 378);

    lv_obj_t *range_bar = lv_obj_create(s_screen);
    lv_obj_set_size(range_bar, 700, 40);
    lv_obj_align(range_bar, LV_ALIGN_TOP_LEFT, 16, 400);
    lv_obj_set_style_bg_color(range_bar, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(range_bar, 0, 0);
    lv_obj_set_flex_flow(range_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(range_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(range_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(range_bar, LV_SCROLLBAR_MODE_OFF);

    static const char *labels[] = {"1 Hour", "24 Hours", "7 Days", "1 Year"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *btn = lv_btn_create(range_bar);
        lv_obj_set_size(btn, 160, 30);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x151A24), 0);
        lv_obj_add_event_cb(btn, range_event, LV_EVENT_CLICKED, (void *)(uintptr_t)s_range_order[i]);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(btn, LV_SCROLLBAR_MODE_OFF);
        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, labels[i]);
        lv_obj_center(label);
        s_range_buttons[i] = btn;
    }

    ui_nav_attach_back_only(s_screen);
    lv_obj_move_foreground(s_title);
    lv_obj_move_foreground(s_holdings);
    update_range_buttons();
    hide_tooltip();
    return s_screen;
}
