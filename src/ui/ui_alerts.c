#include "ui/ui_alerts.h"

#include "lvgl.h"

#include <stdio.h>

#include "services/alert_manager.h"
#include "ui/ui_nav.h"

static lv_obj_t *s_active_list = NULL;
static lv_obj_t *s_log_list = NULL;
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;

static void build_list_item(lv_obj_t *list, const char *text, lv_color_t color)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_set_width(row, 760);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x151A24), 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_top(row, 6, 0);
    lv_obj_set_style_pad_bottom(row, 6, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
}

static void toast_hide_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_alerts_show_toast(const char *text)
{
    ui_nav_set_alert_badge(true);
    if (!s_toast) {
        s_toast = lv_obj_create(lv_layer_top());
        lv_obj_set_size(s_toast, 260, 40);
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1A1D26), 0);
        lv_obj_set_style_border_width(s_toast, 0, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_pad_left(s_toast, 12, 0);
        lv_obj_set_style_pad_right(s_toast, 12, 0);
        lv_obj_set_style_pad_top(s_toast, 8, 0);
        lv_obj_set_style_pad_bottom(s_toast, 8, 0);
        lv_obj_align(s_toast, LV_ALIGN_TOP_RIGHT, -12, 8);

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
    s_toast_timer = lv_timer_create(toast_hide_cb, 1800, NULL);
}

void ui_alerts_refresh(void)
{
    if (!s_active_list || !s_log_list) {
        return;
    }

    lv_obj_clean(s_active_list);
    lv_obj_clean(s_log_list);

    alert_active_t active[16] = {0};
    size_t active_count = alert_manager_get_active(active, 16);
    if (active_count == 0) {
        build_list_item(s_active_list, "No active alerts", lv_color_hex(0x9AA1AD));
    } else {
        for (size_t i = 0; i < active_count; i++) {
            char low_buf[16];
            char high_buf[16];
            if (active[i].low > 0.0) {
                snprintf(low_buf, sizeof(low_buf), "$%.4f", active[i].low);
            } else {
                snprintf(low_buf, sizeof(low_buf), "--");
            }
            if (active[i].high > 0.0) {
                snprintf(high_buf, sizeof(high_buf), "$%.4f", active[i].high);
            } else {
                snprintf(high_buf, sizeof(high_buf), "--");
            }

            char line[96];
            snprintf(line, sizeof(line), "%s  Low:%s  High:%s  Price:$%.4f",
                     active[i].symbol,
                     low_buf,
                     high_buf,
                     active[i].price);
            build_list_item(s_active_list, line, lv_color_hex(0xE6E6E6));
        }
    }

    alert_log_t logs[16] = {0};
    size_t log_count = alert_manager_get_log(logs, 16);
    if (log_count == 0) {
        build_list_item(s_log_list, "No triggers yet", lv_color_hex(0x9AA1AD));
    } else {
        for (size_t i = 0; i < log_count; i++) {
            char line[96];
            snprintf(line, sizeof(line), "%s %s at $%.4f (threshold $%.4f)",
                     logs[i].symbol,
                     logs[i].is_high ? "High" : "Low",
                     logs[i].price,
                     logs[i].threshold);
            build_list_item(s_log_list, line, lv_color_hex(0xE6E6E6));
        }
    }
}

lv_obj_t *ui_alerts_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Alerts");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *active_label = lv_label_create(screen);
    lv_label_set_text(active_label, "Active Alerts");
    lv_obj_set_style_text_color(active_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(active_label, LV_ALIGN_TOP_LEFT, 16, 50);

    s_active_list = lv_obj_create(screen);
    lv_obj_set_size(s_active_list, 760, 140);
    lv_obj_align(s_active_list, LV_ALIGN_TOP_LEFT, 16, 70);
    lv_obj_set_style_bg_color(s_active_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_active_list, 0, 0);
    lv_obj_set_flex_flow(s_active_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_active_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *log_label = lv_label_create(screen);
    lv_label_set_text(log_label, "Recent Triggers");
    lv_obj_set_style_text_color(log_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(log_label, LV_ALIGN_TOP_LEFT, 16, 220);

    s_log_list = lv_obj_create(screen);
    lv_obj_set_size(s_log_list, 760, 140);
    lv_obj_align(s_log_list, LV_ALIGN_TOP_LEFT, 16, 240);
    lv_obj_set_style_bg_color(s_log_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_log_list, 0, 0);
    lv_obj_set_flex_flow(s_log_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_log_list, LV_SCROLLBAR_MODE_AUTO);

    ui_alerts_refresh();
    ui_nav_set_alert_badge(false);

    ui_nav_attach(screen, UI_NAV_ALERTS);
    return screen;
}
