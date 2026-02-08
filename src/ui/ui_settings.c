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

#define SETTINGS_TEXT_COLOR 0x00FE8F
#define SETTINGS_BTN_BG 0x222222

static lv_obj_t *s_i2c_list = NULL;
static lv_obj_t *s_saved_list = NULL;
static lv_obj_t *s_scan_list = NULL;
static lv_obj_t *s_scan_modal = NULL;
static lv_obj_t *s_saved_modal = NULL;
static lv_obj_t *s_touch_modal = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_pass_field = NULL;
static lv_obj_t *s_wifi_keyboard = NULL;
static lv_obj_t *s_wifi_status_label = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static char s_pending_ssid[33] = {0};
static char s_manage_ssid[33] = {0};

static void scan_wifi_event(lv_event_t *e);
static void saved_row_event(lv_event_t *e);

static void style_button(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(SETTINGS_BTN_BG), 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

static void style_button_label(lv_obj_t *label)
{
    lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
}

static char *dup_ssid(const char *ssid)
{
    if (!ssid) {
        return NULL;
    }

    size_t len = strnlen(ssid, 32);
    char *copy = lv_mem_alloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, ssid, len);
    copy[len] = '\0';
    return copy;
}

static void free_ssid_event(lv_event_t *e)
{
    char *ssid = (char *)lv_event_get_user_data(e);
    if (ssid) {
        lv_mem_free(ssid);
    }
}

static void update_wifi_status_label(void)
{
    if (!s_wifi_status_label) {
        return;
    }

    wifi_state_t state = WIFI_STATE_DISCONNECTED;
    int rssi = 0;
    wifi_manager_get_state(&state, &rssi);

    char text[64];
    if (state == WIFI_STATE_CONNECTED) {
        snprintf(text, sizeof(text), "Network Status: Connected (%ddBm)", rssi);
    } else if (state == WIFI_STATE_CONNECTING) {
        snprintf(text, sizeof(text), "Network Status: Connecting...");
    } else {
        snprintf(text, sizeof(text), "Network Status: Disconnected");
    }

    lv_label_set_text(s_wifi_status_label, text);
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_wifi_status_label();
}

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
        lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    }
}

static void close_scan_modal(void)
{
    if (s_scan_modal) {
        lv_obj_del(s_scan_modal);
        s_scan_modal = NULL;
        s_scan_list = NULL;
    }
}

static void close_saved_modal(void)
{
    if (s_saved_modal) {
        lv_obj_del(s_saved_modal);
        s_saved_modal = NULL;
        s_manage_ssid[0] = '\0';
    }
}

static void close_touch_modal(void)
{
    if (s_touch_modal) {
        lv_obj_del(s_touch_modal);
        s_touch_modal = NULL;
    }
}

static void close_wifi_modal(void)
{
    if (s_wifi_modal) {
        lv_obj_del(s_wifi_modal);
        s_wifi_modal = NULL;
        s_pass_field = NULL;
        if (s_wifi_keyboard) {
            lv_obj_del(s_wifi_keyboard);
            s_wifi_keyboard = NULL;
        }
        s_pending_ssid[0] = '\0';
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
        s_wifi_keyboard = lv_keyboard_create(lv_layer_top());
        lv_keyboard_set_mode(s_wifi_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
        lv_obj_set_size(s_wifi_keyboard, 800, 220);
        lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_wifi_keyboard, ta);
}

static void wifi_field_focus_event(lv_event_t *e)
{
    ensure_wifi_keyboard(lv_event_get_target(e));
}

static void wifi_field_blur_event(lv_event_t *e)
{
    (void)e;
#if !CT_KEYBOARD_ENABLE
    return;
#endif
    if (s_wifi_keyboard) {
        lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
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
        lv_obj_t *row = lv_btn_create(s_saved_list);
        lv_obj_set_width(row, 300);
        lv_obj_set_style_bg_color(row, lv_color_hex(SETTINGS_BTN_BG), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        char *ssid_copy = dup_ssid(networks[i].ssid);
        lv_obj_add_event_cb(row, saved_row_event, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(row, free_ssid_event, LV_EVENT_DELETE, ssid_copy);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, networks[i].ssid);
        lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    }

    if (count == 0) {
        lv_obj_t *label = lv_label_create(s_saved_list);
        lv_label_set_text(label, "No saved networks");
        lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
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
    close_scan_modal();

    s_wifi_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_wifi_modal, 800, 480);
    lv_obj_set_style_bg_color(s_wifi_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_wifi_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_wifi_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_wifi_modal);
    lv_obj_set_size(card, 460, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Enter Password");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *subtitle = lv_label_create(card);
    lv_label_set_text(subtitle, s_pending_ssid);
    lv_obj_set_style_text_color(subtitle, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 0, 30);

    s_pass_field = lv_textarea_create(card);
    lv_textarea_set_one_line(s_pass_field, true);
    lv_textarea_set_placeholder_text(s_pass_field, "Password");
    lv_obj_set_width(s_pass_field, 300);
    lv_obj_align(s_pass_field, LV_ALIGN_TOP_LEFT, 0, 70);
    lv_obj_set_style_bg_color(s_pass_field, lv_color_hex(0x11151F), 0);
    lv_obj_set_style_text_color(s_pass_field, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_border_width(s_pass_field, 0, 0);
    lv_obj_add_event_cb(s_pass_field, wifi_field_focus_event, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_pass_field, wifi_field_focus_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_pass_field, wifi_field_blur_event, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *save_btn = lv_btn_create(card);
    lv_obj_set_size(save_btn, 110, 36);
    lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(save_btn, save_network_event, LV_EVENT_CLICKED, NULL);
    style_button(save_btn);
    lv_obj_t *save_label = lv_label_create(save_btn);
    lv_label_set_text(save_label, "Save");
    style_button_label(save_label);
    lv_obj_center(save_label);

    lv_obj_t *forget_btn = lv_btn_create(card);
    lv_obj_set_size(forget_btn, 110, 36);
    lv_obj_align(forget_btn, LV_ALIGN_BOTTOM_RIGHT, -120, 0);
    lv_obj_add_event_cb(forget_btn, forget_network_event, LV_EVENT_CLICKED, s_pending_ssid);
    style_button(forget_btn);
    lv_obj_t *forget_label = lv_label_create(forget_btn);
    lv_label_set_text(forget_label, "Forget");
    style_button_label(forget_label);
    lv_obj_center(forget_label);
}

static void saved_close_event(lv_event_t *e)
{
    (void)e;
    close_saved_modal();
}

static void saved_forget_event(lv_event_t *e)
{
    (void)e;
    if (s_manage_ssid[0] == '\0') {
        close_saved_modal();
        return;
    }

    wifi_manager_forget_network(s_manage_ssid);
    refresh_saved_list();
    close_saved_modal();
}

static void open_saved_modal(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return;
    }

    close_saved_modal();
    strncpy(s_manage_ssid, ssid, sizeof(s_manage_ssid) - 1);

    s_saved_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_saved_modal, 800, 480);
    lv_obj_set_style_bg_color(s_saved_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_saved_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_saved_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_saved_modal);
    lv_obj_set_size(card, 420, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, s_manage_ssid);
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *forget_btn = lv_btn_create(card);
    lv_obj_set_size(forget_btn, 140, 36);
    lv_obj_align(forget_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(forget_btn, saved_forget_event, LV_EVENT_CLICKED, NULL);
    style_button(forget_btn);
    lv_obj_t *forget_label = lv_label_create(forget_btn);
    lv_label_set_text(forget_label, "Forget");
    style_button_label(forget_label);
    lv_obj_center(forget_label);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 140, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, -150, 0);
    lv_obj_add_event_cb(close_btn, saved_close_event, LV_EVENT_CLICKED, NULL);
    style_button(close_btn);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    style_button_label(close_label);
    lv_obj_center(close_label);
}

static void scan_result_event(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || ssid[0] == '\0') {
        return;
    }
    open_password_modal(ssid);
}

static void saved_row_event(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    if (!ssid || ssid[0] == '\0') {
        return;
    }
    open_saved_modal(ssid);
}

static void scan_close_event(lv_event_t *e)
{
    (void)e;
    close_scan_modal();
}

static void touch_close_event(lv_event_t *e)
{
    (void)e;
    close_touch_modal();
}

static void touch_calibrate_event(lv_event_t *e)
{
    (void)e;
    if (s_touch_modal) {
        return;
    }

    s_touch_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_touch_modal, 800, 480);
    lv_obj_set_style_bg_color(s_touch_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_touch_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_touch_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_touch_modal);
    lv_obj_set_size(card, 420, 220);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Touch Calibration");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, "Coming soon");
    lv_obj_set_style_text_color(body, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 52);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 120, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, touch_close_event, LV_EVENT_CLICKED, NULL);
    style_button(close_btn);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    style_button_label(close_label);
    lv_obj_center(close_label);
}

static void scan_wifi_run(void)
{
    if (!s_scan_list) {
        return;
    }

    lv_obj_clean(s_scan_list);
    wifi_scan_result_t results[12] = {0};
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(results, 12, &count);
    if (err != ESP_OK) {
        lv_obj_t *label = lv_label_create(s_scan_list);
        lv_label_set_text(label, "Scan failed. Tap Rescan.");
        lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        if (results[i].ssid[0] == '\0') {
            continue;
        }
        lv_obj_t *row = lv_btn_create(s_scan_list);
        lv_obj_set_width(row, lv_pct(100));
        style_button(row);
        lv_obj_set_style_pad_left(row, 8, 0);
        lv_obj_set_style_pad_right(row, 8, 0);
        lv_obj_set_style_pad_top(row, 6, 0);
        lv_obj_set_style_pad_bottom(row, 6, 0);
        char *ssid_copy = dup_ssid(results[i].ssid);
        lv_obj_add_event_cb(row, scan_result_event, LV_EVENT_CLICKED, ssid_copy);
        lv_obj_add_event_cb(row, free_ssid_event, LV_EVENT_DELETE, ssid_copy);

        char line[48];
        snprintf(line, sizeof(line), "%s  %ddBm", results[i].ssid, results[i].rssi);
        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, line);
        lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    }
}

static void open_scan_modal(void)
{
    if (s_scan_modal) {
        return;
    }

    s_scan_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scan_modal, 800, 480);
    lv_obj_set_style_bg_color(s_scan_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_scan_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_scan_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_scan_modal);
    lv_obj_set_size(card, 520, 340);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Available Networks");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    s_scan_list = lv_obj_create(card);
    lv_obj_set_size(s_scan_list, 480, 220);
    lv_obj_align(s_scan_list, LV_ALIGN_TOP_LEFT, 0, 48);
    lv_obj_set_style_bg_color(s_scan_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_scan_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *placeholder = lv_label_create(s_scan_list);
    lv_label_set_text(placeholder, "Scanning...");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    lv_obj_t *rescan_btn = lv_btn_create(card);
    lv_obj_set_size(rescan_btn, 120, 36);
    lv_obj_align(rescan_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(rescan_btn, scan_wifi_event, LV_EVENT_CLICKED, NULL);
    style_button(rescan_btn);
    lv_obj_t *rescan_label = lv_label_create(rescan_btn);
    lv_label_set_text(rescan_label, "Rescan");
    style_button_label(rescan_label);
    lv_obj_center(rescan_label);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 120, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close_btn, scan_close_event, LV_EVENT_CLICKED, NULL);
    style_button(close_btn);
    lv_obj_t *close_label = lv_label_create(close_btn);
    lv_label_set_text(close_label, "Close");
    style_button_label(close_label);
    lv_obj_center(close_label);
}

static void scan_wifi_event(lv_event_t *e)
{
    (void)e;
    open_scan_modal();
    if (s_scan_list) {
        lv_obj_clean(s_scan_list);
        lv_obj_t *placeholder = lv_label_create(s_scan_list);
        lv_label_set_text(placeholder, "Scanning...");
        lv_obj_set_style_text_color(placeholder, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    }
    lv_async_call((lv_async_cb_t)scan_wifi_run, NULL);
}

lv_obj_t *ui_settings_screen_create(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_pad_top(screen, UI_NAV_HEIGHT, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    lv_obj_t *brightness_label = lv_label_create(screen);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 16, 56);

    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_width(slider, 360);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 16, 80);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *scan_btn = lv_btn_create(screen);
    lv_obj_set_size(scan_btn, 140, 36);
    lv_obj_align(scan_btn, LV_ALIGN_TOP_LEFT, 16, 130);
    lv_obj_add_event_cb(scan_btn, scan_i2c, LV_EVENT_CLICKED, NULL);
    style_button(scan_btn);

    lv_obj_t *scan_label = lv_label_create(scan_btn);
    lv_label_set_text(scan_label, "I2C Scan");
    style_button_label(scan_label);
    lv_obj_center(scan_label);

    s_i2c_list = lv_obj_create(screen);
    lv_obj_set_size(s_i2c_list, 160, 200);
    lv_obj_align(s_i2c_list, LV_ALIGN_TOP_LEFT, 16, 180);
    lv_obj_set_style_bg_color(s_i2c_list, lv_color_hex(0x141822), 0);
    lv_obj_set_style_border_width(s_i2c_list, 0, 0);

    lv_obj_t *touch_btn = lv_btn_create(screen);
    lv_obj_set_size(touch_btn, 200, 36);
    lv_obj_align(touch_btn, LV_ALIGN_TOP_LEFT, 16, 400);
    style_button(touch_btn);
    lv_obj_add_event_cb(touch_btn, touch_calibrate_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *touch_label = lv_label_create(touch_btn);
    lv_label_set_text(touch_label, "Touch Calibration");
    style_button_label(touch_label);
    lv_obj_center(touch_label);

    lv_obj_t *wifi_title = lv_label_create(screen);
    lv_label_set_text(wifi_title, "WiFi Networks");
    lv_obj_set_style_text_color(wifi_title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_22, 0);
    lv_obj_align(wifi_title, LV_ALIGN_TOP_LEFT, 420, 24);

    s_wifi_status_label = lv_label_create(screen);
    lv_label_set_text(s_wifi_status_label, "Network Status: ...");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 420, 56);
    update_wifi_status_label();
    if (!s_wifi_status_timer) {
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 1000, NULL);
    }

    lv_obj_t *scan_wifi_btn = lv_btn_create(screen);
    lv_obj_set_size(scan_wifi_btn, 160, 36);
    lv_obj_align(scan_wifi_btn, LV_ALIGN_TOP_LEFT, 420, 96);
    lv_obj_add_event_cb(scan_wifi_btn, scan_wifi_event, LV_EVENT_CLICKED, NULL);
    style_button(scan_wifi_btn);
    lv_obj_t *scan_wifi_label = lv_label_create(scan_wifi_btn);
    lv_label_set_text(scan_wifi_label, "Add Network");
    style_button_label(scan_wifi_label);
    lv_obj_center(scan_wifi_label);

    lv_obj_t *saved_label = lv_label_create(screen);
    lv_label_set_text(saved_label, "Saved Networks");
    lv_obj_set_style_text_color(saved_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(saved_label, LV_ALIGN_TOP_LEFT, 420, 142);

    s_saved_list = lv_obj_create(screen);
    lv_obj_set_size(s_saved_list, 320, 140);
    lv_obj_align(s_saved_list, LV_ALIGN_TOP_LEFT, 420, 162);
    lv_obj_set_style_bg_color(s_saved_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_saved_list, LV_SCROLLBAR_MODE_AUTO);

    refresh_saved_list();

    ui_nav_attach(screen, UI_NAV_SETTINGS);
    return screen;
}
