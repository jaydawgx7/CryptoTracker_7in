#include "ui/ui_settings.h"

#include <stdio.h>

#include "lvgl.h"

#include "services/control_mcu.h"
#include "services/i2c_bus.h"
#include "services/wifi_manager.h"

#include "ui/ui_nav.h"
#include "models/app_state.h"

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

static lv_obj_t *s_i2c_list = NULL;
static lv_obj_t *s_saved_list = NULL;
static lv_obj_t *s_scan_list = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_pass_field = NULL;
static lv_obj_t *s_wifi_keyboard = NULL;
static char s_pending_ssid[33] = {0};

static void brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    control_mcu_set_brightness((uint8_t)value);
}

static void scan_i2c(lv_event_t *e)
{
    (void)e;
    uint8_t addresses[16] = {0};
    size_t count = i2c_bus_scan(addresses, 16);

    if (!s_i2c_list) {
        return;
    }

    lv_obj_clean(s_i2c_list);

    for (size_t i = 0; i < count && i < 16; i++) {
        char line[16];
        snprintf(line, sizeof(line), "0x%02X", addresses[i]);
        lv_obj_t *label = lv_label_create(s_i2c_list);
        lv_label_set_text(label, line);
    }
}

static void close_wifi_modal(void)
{
    if (s_wifi_modal) {
        lv_obj_del(s_wifi_modal);
        s_wifi_modal = NULL;
        s_pass_field = NULL;
    }
}

static void ensure_wifi_keyboard(lv_obj_t *ta)
{
#if !CT_KEYBOARD_ENABLE
    (void)ta;
    return;
#endif
    if (!s_wifi_modal) {
        return;
    }

    if (!s_wifi_keyboard) {
        s_wifi_keyboard = lv_keyboard_create(s_wifi_modal);
        lv_keyboard_set_mode(s_wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_set_size(s_wifi_keyboard, 760, 150);
        lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    }
    lv_keyboard_set_textarea(s_wifi_keyboard, ta);
}

static void wifi_field_focus_event(lv_event_t *e)
{
    ensure_wifi_keyboard(lv_event_get_target(e));
}

static void refresh_saved_list(void)
{
    if (!s_saved_list) {
        return;
    }

    lv_obj_clean(s_saved_list);
    wifi_saved_network_t networks[MAX_WIFI_NETWORKS] = {0};
    size_t count = wifi_manager_get_saved(networks, MAX_WIFI_NETWORKS);

    for (size_t i = 0; i < count; i++) {
        lv_obj_t *row = lv_obj_create(s_saved_list);
        lv_obj_set_width(row, 300);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x151A24), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, networks[i].ssid);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    }

    if (count == 0) {
        lv_obj_t *label = lv_label_create(s_saved_list);
        lv_label_set_text(label, "No saved networks");
        lv_obj_set_style_text_color(label, lv_color_hex(0x9AA1AD), 0);
    }
}

static void forget_network_event(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid) {
        return;
    }

    wifi_manager_forget_network(ssid);
    refresh_saved_list();
    close_wifi_modal();
}

static void save_network_event(lv_event_t *e)
{
    (void)e;
    if (!s_pass_field || s_pending_ssid[0] == '\0') {
        return;
    }

    const char *password = lv_textarea_get_text(s_pass_field);
    wifi_manager_add_network(s_pending_ssid, password);
    refresh_saved_list();
    close_wifi_modal();
}

static void open_password_modal(const char *ssid)
{
    if (!ssid) {
        return;
    }

    close_wifi_modal();
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);

    s_wifi_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wifi_modal, 800, 480);
    lv_obj_set_style_bg_color(s_wifi_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_wifi_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_wifi_modal);
    lv_obj_set_size(card, 420, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, ssid);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    s_pass_field = lv_textarea_create(card);
    lv_textarea_set_one_line(s_pass_field, true);
    lv_textarea_set_placeholder_text(s_pass_field, "Password");
    lv_obj_set_width(s_pass_field, 300);
    lv_obj_align(s_pass_field, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_color(s_pass_field, lv_color_hex(0x11151F), 0);
    lv_obj_set_style_text_color(s_pass_field, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_border_width(s_pass_field, 0, 0);
    lv_obj_add_event_cb(s_pass_field, wifi_field_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_pass_field, wifi_field_focus_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_btn = lv_btn_create(card);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(save_btn, save_network_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *forget_btn = lv_btn_create(card);
    lv_obj_set_size(forget_btn, 110, 36);
    lv_obj_align(forget_btn, LV_ALIGN_BOTTOM_RIGHT, -120, 0);
    lv_obj_add_event_cb(forget_btn, forget_network_event, LV_EVENT_CLICKED, s_pending_ssid);
    lv_obj_t *forget_label = lv_label_create(forget_btn);
    lv_label_set_text(forget_label, "Forget");
    lv_obj_center(forget_label);

    ensure_wifi_keyboard(s_pass_field);
}

static void scan_result_event(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || ssid[0] == '\0') {
        return;
    }
    open_password_modal(ssid);
}

static void scan_wifi_event(lv_event_t *e)
{
    (void)e;
    if (!s_scan_list) {
        return;
    }

    lv_obj_clean(s_scan_list);
    wifi_scan_result_t results[12] = {0};
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(results, 12, &count);
    if (err != ESP_OK) {
        lv_obj_t *label = lv_label_create(s_scan_list);
        lv_label_set_text(label, "Scan failed");
        lv_obj_set_style_text_color(label, lv_color_hex(0xE74C3C), 0);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        lv_obj_t *row = lv_btn_create(s_scan_list);
        lv_obj_set_width(row, 300);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x151A24), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        lv_obj_add_event_cb(row, scan_result_event, LV_EVENT_CLICKED, results[i].ssid);

        char line[48];
        snprintf(line, sizeof(line), "%s  %ddBm", results[i].ssid, results[i].rssi);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, line);
        lv_obj_set_style_text_color(label, lv_color_hex(0xE6E6E6), 0);
    }
}

lv_obj_t *ui_settings_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *brightness_label = lv_label_create(screen);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(0xB7BDCA), 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 16, 56);

    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_width(slider, 360);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 16, 80);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 60, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *scan_btn = lv_btn_create(screen);
    lv_obj_set_size(scan_btn, 140, 36);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 16, 130);
    lv_obj_add_event_cb(scan_btn, scan_i2c, LV_EVENT_CLICKED, NULL);

    lv_obj_t *scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "I2C Scan");
    lv_obj_center(scan_label);

    s_i2c_list = lv_obj_create(screen);
    lv_obj_set_size(s_i2c_list, 160, 200);
    lv_obj_align(s_i2c_list, LV_ALIGN_TOP_LEFT, 16, 180);
    lv_obj_set_style_bg_color(s_i2c_list, lv_color_hex(0x141822), 0);
    lv_obj_set_style_border_width(s_i2c_list, 0, 0);

    lv_obj_t *wifi_title = lv_label_create(screen);
    lv_label_set_text(wifi_title, "WiFi Networks");
    lv_obj_set_style_text_color(wifi_title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_title, LV_ALIGN_TOP_LEFT, 420, 12);

    lv_obj_t *scan_wifi_btn = lv_btn_create(screen);
    lv_obj_set_size(scan_wifi_btn, 120, 32);
    lv_obj_align(scan_wifi_btn, LV_ALIGN_TOP_LEFT, 420, 52);
    lv_obj_add_event_cb(scan_wifi_btn, scan_wifi_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_wifi_label = lv_label_create(scan_wifi_btn);
    lv_label_set_text(scan_wifi_label, "Scan");
    lv_obj_center(scan_wifi_label);

    lv_obj_t *saved_label = lv_label_create(screen);
    lv_label_set_text(saved_label, "Saved");
    lv_obj_set_style_text_color(saved_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(saved_label, LV_ALIGN_TOP_LEFT, 420, 90);

    s_saved_list = lv_obj_create(screen);
    lv_obj_set_size(s_saved_list, 320, 140);
    lv_obj_align(s_saved_list, LV_ALIGN_TOP_LEFT, 420, 110);
    lv_obj_set_style_bg_color(s_saved_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_saved_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *scan_results_label = lv_label_create(screen);
    lv_label_set_text(scan_results_label, "Scan Results");
    lv_obj_set_style_text_color(scan_results_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(scan_results_label, LV_ALIGN_TOP_LEFT, 420, 260);

    s_scan_list = lv_obj_create(screen);
    lv_obj_set_size(s_scan_list, 320, 140);
    lv_obj_align(s_scan_list, LV_ALIGN_TOP_LEFT, 420, 280);
    lv_obj_set_style_bg_color(s_scan_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_scan_list, LV_SCROLLBAR_MODE_AUTO);

    refresh_saved_list();

    ui_nav_attach(screen, UI_NAV_SETTINGS);
    return screen;
}
