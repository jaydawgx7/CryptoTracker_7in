#include "ui/ui_settings.h"

#include <stdio.h>

#include "lvgl.h"

#include "services/control_mcu.h"
#include "services/nvs_store.h"
#include "services/wifi_manager.h"

#include "ui/ui.h"
#include "ui/ui_nav.h"
#include "ui/ui_theme.h"
#include "models/app_state.h"

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

#define SETTINGS_TEXT_COLOR settings_accent_color()
#define SETTINGS_BTN_BG 0x222222

#ifndef CT_BUILD_BRANCH_STR
#define CT_BUILD_BRANCH_STR "dev"
#endif

static lv_obj_t *s_saved_list = NULL;
static lv_obj_t *s_scan_list = NULL;
static lv_obj_t *s_scan_modal = NULL;
static lv_obj_t *s_touch_modal = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_accent_modal = NULL;
static lv_obj_t *s_accent_sheet = NULL;
static lv_obj_t *s_pass_field = NULL;
static lv_obj_t *s_wifi_keyboard = NULL;
static lv_obj_t *s_wifi_status_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static lv_obj_t *s_brightness_slider = NULL;
static lv_obj_t *s_refresh_slider = NULL;
static lv_obj_t *s_refresh_value = NULL;
static lv_obj_t *s_button3d_toggle = NULL;
static lv_obj_t *s_dark_toggle = NULL;
static lv_obj_t *s_accent_modal_dots[8] = {0};
static lv_obj_t *s_shadow_modal_dots[8] = {0};
static lv_obj_t *s_accent_picker = NULL;
static lv_obj_t *s_shadow_picker = NULL;
static lv_obj_t *s_source_coin_btn = NULL;
static lv_obj_t *s_source_kraken_btn = NULL;
static lv_obj_t *s_source_coin_label = NULL;
static lv_obj_t *s_source_kraken_label = NULL;
static app_state_t *s_state = NULL;
static char s_pending_ssid[33] = {0};

static void scan_wifi_event(lv_event_t *e);
static void saved_row_event(lv_event_t *e);
static void apply_prefs_to_controls(void);
static void update_refresh_label(int32_t value);
static void update_accent_dots(void);
static void update_shadow_dots(void);
static void accent_modal_select_event(lv_event_t *e);
static void shadow_modal_select_event(lv_event_t *e);
static void accent_colorwheel_event(lv_event_t *e);
static void shadow_colorwheel_event(lv_event_t *e);
static void set_colorwheel_full_hue(lv_obj_t *picker, uint32_t color_hex);
static void accent_modal_open_event(lv_event_t *e);
static void accent_modal_close_event(lv_event_t *e);
static void accent_sheet_event(lv_event_t *e);
static void button3d_toggle_event(lv_event_t *e);
static void dark_mode_toggle_event(lv_event_t *e);
static void data_source_event(lv_event_t *e);
static void update_source_buttons(void);
static void apply_refresh_range(void);
static uint16_t clamp_refresh_value(int32_t value);
static void style_settings_switch(lv_obj_t *toggle);

static const uint32_t s_color_presets[8] = {
    0x00FE8F,
    0x00C2FF,
    0xF15BB5,
    0xF5A623,
    0xFF5252,
    0x64FFDA,
    0x448AFF,
    0xBB86FC
};

static void set_colorwheel_full_hue(lv_obj_t *picker, uint32_t color_hex)
{
    if (!picker) {
        return;
    }

    lv_color_hsv_t hsv = lv_color_to_hsv(lv_color_hex(color_hex));
    hsv.s = 100;
    hsv.v = 100;
    lv_colorwheel_set_hsv(picker, hsv);
}

static uint32_t settings_accent_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return theme ? theme->accent : 0x00FE8F;
}

static void style_settings_switch(lv_obj_t *toggle)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    bool dark_mode = theme ? theme->dark_mode : true;
    uint32_t track_main = dark_mode ? 0x11151F : 0xE2E8F0;
    uint32_t track_indicator = dark_mode ? 0x2A3142 : 0xCBD5E1;
    uint32_t knob_color = dark_mode ? 0x1A1D26 : 0xFFFFFF;

    lv_obj_set_style_bg_color(toggle, lv_color_hex(track_main), LV_PART_MAIN);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(track_indicator), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(knob_color), LV_PART_KNOB);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(track_main), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(track_indicator), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(toggle, lv_color_hex(knob_color), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(toggle, 0, LV_PART_MAIN);
}

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

    if (s_wifi_ssid_label) {
        char ssid[33] = {0};
        bool has_ssid = wifi_manager_get_connected_ssid(ssid, sizeof(ssid));
        if (state == WIFI_STATE_CONNECTED && has_ssid) {
            char ssid_line[48];
            snprintf(ssid_line, sizeof(ssid_line), "SSID: %s", ssid);
            lv_label_set_text(s_wifi_ssid_label, ssid_line);
        } else if (state == WIFI_STATE_CONNECTING) {
            lv_label_set_text(s_wifi_ssid_label, "SSID: Connecting");
        } else {
            lv_label_set_text(s_wifi_ssid_label, "SSID: --");
        }
    }
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

    if (s_state) {
        s_state->prefs.brightness = (uint8_t)value;
        nvs_store_save_app_state(s_state);
    }
}

static void refresh_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t value = lv_slider_get_value(slider);
    value = clamp_refresh_value(value);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    update_refresh_label(value);

    if (s_state) {
        s_state->prefs.refresh_seconds = (uint16_t)value;
        nvs_store_save_app_state(s_state);
    }
}

static uint16_t clamp_refresh_value(int32_t value)
{
    int32_t min = 5;
    int32_t max = 60;

    if (s_state && s_state->prefs.data_source == DATA_SOURCE_KRAKEN) {
        min = 1;
        max = 15;
    }

    if (value < min) {
        value = min;
    } else if (value > max) {
        value = max;
    }

    return (uint16_t)value;
}

static void button3d_toggle_event(lv_event_t *e)
{
    lv_obj_t *toggle = lv_event_get_target(e);
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

    if (s_state) {
        s_state->prefs.buttons_3d = enabled;
        nvs_store_save_app_state(s_state);
    }

    ui_theme_set_buttons_3d(enabled);
}

static void dark_mode_toggle_event(lv_event_t *e)
{
    lv_obj_t *toggle = lv_event_get_target(e);
    bool dark_mode = lv_obj_has_state(toggle, LV_STATE_CHECKED);

    if (s_state) {
        s_state->prefs.dark_mode = dark_mode;
        nvs_store_save_app_state(s_state);
    }

    ui_apply_theme(dark_mode);
}

static void data_source_event(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target(e);
    data_source_t source = DATA_SOURCE_COINGECKO;

    if (target == s_source_kraken_btn) {
        source = DATA_SOURCE_KRAKEN;
    }

    if (s_state) {
        s_state->prefs.data_source = source;
        nvs_store_save_app_state(s_state);
    }

    update_source_buttons();
    apply_refresh_range();
}

static void update_refresh_label(int32_t value)
{
    if (!s_refresh_value) {
        return;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%ds", (int)value);
    lv_label_set_text(s_refresh_value, buf);
}

static void update_source_buttons(void)
{
    if (!s_state || !s_source_coin_btn || !s_source_kraken_btn) {
        return;
    }

    bool kraken = (s_state->prefs.data_source == DATA_SOURCE_KRAKEN);
    lv_obj_set_style_bg_color(s_source_coin_btn, lv_color_hex(kraken ? SETTINGS_BTN_BG : 0x424242), 0);
    lv_obj_set_style_bg_color(s_source_kraken_btn, lv_color_hex(kraken ? 0x424242 : SETTINGS_BTN_BG), 0);

    if (s_source_coin_label) {
        lv_obj_set_style_text_color(s_source_coin_label, lv_color_hex(kraken ? 0x9AA1AD : SETTINGS_TEXT_COLOR), 0);
    }
    if (s_source_kraken_label) {
        lv_obj_set_style_text_color(s_source_kraken_label, lv_color_hex(kraken ? SETTINGS_TEXT_COLOR : 0x9AA1AD), 0);
    }
}

static void apply_refresh_range(void)
{
    if (!s_state || !s_refresh_slider) {
        return;
    }

    if (s_state->prefs.data_source == DATA_SOURCE_KRAKEN) {
        lv_slider_set_range(s_refresh_slider, 1, 15);
    } else {
        lv_slider_set_range(s_refresh_slider, 5, 60);
    }

    uint16_t clamped = clamp_refresh_value(s_state->prefs.refresh_seconds);
    if (clamped != s_state->prefs.refresh_seconds) {
        s_state->prefs.refresh_seconds = clamped;
        nvs_store_save_app_state(s_state);
    }
    lv_slider_set_value(s_refresh_slider, clamped, LV_ANIM_OFF);
    update_refresh_label(clamped);
}

static void update_accent_dots(void)
{
    if (!s_state) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        if (!s_accent_modal_dots[i]) {
            continue;
        }
        uint32_t accent = (uint32_t)(uintptr_t)lv_obj_get_user_data(s_accent_modal_dots[i]);
        if (accent == s_state->prefs.accent_hex) {
            lv_obj_set_style_border_width(s_accent_modal_dots[i], 2, 0);
            lv_obj_set_style_border_color(s_accent_modal_dots[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_border_width(s_accent_modal_dots[i], 0, 0);
        }
    }
}

static void update_shadow_dots(void)
{
    if (!s_state) {
        return;
    }

    for (int i = 0; i < 8; i++) {
        if (!s_shadow_modal_dots[i]) {
            continue;
        }
        uint32_t shadow = (uint32_t)(uintptr_t)lv_obj_get_user_data(s_shadow_modal_dots[i]);
        if (shadow == s_state->prefs.shadow_hex) {
            lv_obj_set_style_border_width(s_shadow_modal_dots[i], 2, 0);
            lv_obj_set_style_border_color(s_shadow_modal_dots[i], lv_color_hex(0xFFFFFF), 0);
        } else {
            lv_obj_set_style_border_width(s_shadow_modal_dots[i], 0, 0);
        }
    }
}

static void accent_modal_select_event(lv_event_t *e)
{
    uint32_t accent = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_state) {
        s_state->prefs.accent_hex = accent;
        nvs_store_save_app_state(s_state);
    }
    if (s_accent_picker) {
        set_colorwheel_full_hue(s_accent_picker, accent);
    }
    update_accent_dots();
    ui_apply_theme(s_state ? s_state->prefs.dark_mode : true);
}

static void shadow_modal_select_event(lv_event_t *e)
{
    uint32_t shadow = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_state) {
        s_state->prefs.shadow_hex = shadow;
        nvs_store_save_app_state(s_state);
    }
    if (s_shadow_picker) {
        set_colorwheel_full_hue(s_shadow_picker, shadow);
    }
    update_shadow_dots();
    ui_apply_theme(s_state ? s_state->prefs.dark_mode : true);
}

static void accent_colorwheel_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *picker = lv_event_get_target(e);
    lv_color_t color = lv_colorwheel_get_rgb(picker);
    uint32_t hex = lv_color_to32(color) & 0x00FFFFFFU;

    if (s_state) {
        s_state->prefs.accent_hex = hex;
    }
    update_accent_dots();
    if (code == LV_EVENT_RELEASED) {
        if (s_state) {
            nvs_store_save_app_state(s_state);
        }
        ui_apply_theme(s_state ? s_state->prefs.dark_mode : true);
    }
}

static void shadow_colorwheel_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *picker = lv_event_get_target(e);
    lv_color_t color = lv_colorwheel_get_rgb(picker);
    uint32_t hex = lv_color_to32(color) & 0x00FFFFFFU;

    if (s_state) {
        s_state->prefs.shadow_hex = hex;
    }
    update_shadow_dots();
    if (code == LV_EVENT_RELEASED) {
        if (s_state) {
            nvs_store_save_app_state(s_state);
        }
        ui_apply_theme(s_state ? s_state->prefs.dark_mode : true);
    }
}

static void accent_modal_close_event(lv_event_t *e)
{
    (void)e;
    if (s_accent_modal) {
        lv_obj_del(s_accent_modal);
        s_accent_modal = NULL;
        s_accent_sheet = NULL;
        for (int i = 0; i < 8; i++) {
            s_accent_modal_dots[i] = NULL;
            s_shadow_modal_dots[i] = NULL;
        }
        s_accent_picker = NULL;
        s_shadow_picker = NULL;
    }
}

static void accent_sheet_event(lv_event_t *e)
{
    lv_event_stop_bubbling(e);
}

static void accent_modal_open_event(lv_event_t *e)
{
    (void)e;
    if (s_accent_modal) {
        return;
    }

    s_accent_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_accent_modal, 800, 480);
    lv_obj_set_style_bg_color(s_accent_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_accent_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_accent_modal, 0, 0);
    lv_obj_clear_flag(s_accent_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_accent_modal, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_accent_modal, accent_modal_close_event, LV_EVENT_CLICKED, NULL);

    s_accent_sheet = lv_obj_create(s_accent_modal);
    lv_obj_set_size(s_accent_sheet, 760, 300);
    lv_obj_align(s_accent_sheet, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_accent_sheet, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(s_accent_sheet, 0, 0);
    lv_obj_set_style_radius(s_accent_sheet, 16, 0);
    lv_obj_set_style_pad_all(s_accent_sheet, 12, 0);
    lv_obj_set_style_pad_top(s_accent_sheet, 4, 0);
    lv_obj_clear_flag(s_accent_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_accent_sheet, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_accent_sheet, LV_DIR_NONE);
    lv_obj_add_event_cb(s_accent_sheet, accent_sheet_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *content = lv_obj_create(s_accent_sheet);
    lv_obj_set_size(content, 736, 276);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, -20);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content, LV_DIR_NONE);

    uint32_t accent_color = s_state ? s_state->prefs.accent_hex : 0x00FE8F;
    uint32_t shadow_color = s_state ? s_state->prefs.shadow_hex : 0x2A3142;

    lv_obj_t *accent_col = lv_obj_create(content);
    lv_obj_set_size(accent_col, 350, 266);
    lv_obj_set_style_bg_opa(accent_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_col, 0, 0);
    lv_obj_set_flex_flow(accent_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(accent_col, 8, 0);
    lv_obj_set_style_pad_bottom(accent_col, 6, 0);
    lv_obj_set_flex_align(accent_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(accent_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *accent_title = lv_label_create(accent_col);
    lv_label_set_text(accent_title, "Accent Color");
    lv_obj_set_style_text_color(accent_title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    lv_obj_t *accent_spacer = lv_obj_create(accent_col);
    lv_obj_set_size(accent_spacer, 1, 6);
    lv_obj_set_style_bg_opa(accent_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_spacer, 0, 0);
    lv_obj_clear_flag(accent_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_spacer, LV_SCROLLBAR_MODE_OFF);

    s_accent_picker = lv_colorwheel_create(accent_col, true);
    lv_obj_set_size(s_accent_picker, 96, 96);
    lv_colorwheel_set_mode(s_accent_picker, LV_COLORWHEEL_MODE_HUE);
    lv_colorwheel_set_mode_fixed(s_accent_picker, true);
    set_colorwheel_full_hue(s_accent_picker, accent_color);
    lv_obj_add_event_cb(s_accent_picker, accent_colorwheel_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_width(s_accent_picker, 12, LV_PART_KNOB);
    lv_obj_set_style_height(s_accent_picker, 12, LV_PART_KNOB);
    lv_obj_set_style_radius(s_accent_picker, 6, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_accent_picker, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_accent_picker, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(s_accent_picker, accent_colorwheel_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *accent_row1 = lv_obj_create(accent_col);
    lv_obj_set_size(accent_row1, 320, 44);
    lv_obj_set_style_bg_opa(accent_row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_row1, 0, 0);
    lv_obj_set_flex_flow(accent_row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accent_row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(accent_row1, 10, 0);
    lv_obj_set_style_pad_bottom(accent_row1, 2, 0);
    lv_obj_clear_flag(accent_row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_row1, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *accent_row2 = lv_obj_create(accent_col);
    lv_obj_set_size(accent_row2, 320, 44);
    lv_obj_set_style_bg_opa(accent_row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_row2, 0, 0);
    lv_obj_set_flex_flow(accent_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accent_row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(accent_row2, 10, 0);
    lv_obj_set_style_pad_bottom(accent_row2, 2, 0);
    lv_obj_clear_flag(accent_row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_row2, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < 8; i++) {
        lv_obj_t *parent = (i < 4) ? accent_row1 : accent_row2;
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 26, 26);
        lv_obj_set_style_radius(dot, 13, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_color_presets[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(dot, LV_DIR_NONE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dot, accent_modal_select_event, LV_EVENT_CLICKED, (void *)(uintptr_t)s_color_presets[i]);
        lv_obj_set_user_data(dot, (void *)(uintptr_t)s_color_presets[i]);
        s_accent_modal_dots[i] = dot;
    }

    lv_obj_t *shadow_col = lv_obj_create(content);
    lv_obj_set_size(shadow_col, 350, 266);
    lv_obj_set_style_bg_opa(shadow_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_col, 0, 0);
    lv_obj_set_flex_flow(shadow_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(shadow_col, 8, 0);
    lv_obj_set_style_pad_bottom(shadow_col, 6, 0);
    lv_obj_set_flex_align(shadow_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(shadow_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *shadow_title = lv_label_create(shadow_col);
    lv_label_set_text(shadow_title, "Button Shadows");
    lv_obj_set_style_text_color(shadow_title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    lv_obj_t *shadow_spacer = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_spacer, 1, 6);
    lv_obj_set_style_bg_opa(shadow_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_spacer, 0, 0);
    lv_obj_clear_flag(shadow_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_spacer, LV_SCROLLBAR_MODE_OFF);

    s_shadow_picker = lv_colorwheel_create(shadow_col, true);
    lv_obj_set_size(s_shadow_picker, 96, 96);
    lv_colorwheel_set_mode(s_shadow_picker, LV_COLORWHEEL_MODE_HUE);
    lv_colorwheel_set_mode_fixed(s_shadow_picker, true);
    set_colorwheel_full_hue(s_shadow_picker, shadow_color);
    lv_obj_add_event_cb(s_shadow_picker, shadow_colorwheel_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_width(s_shadow_picker, 12, LV_PART_KNOB);
    lv_obj_set_style_height(s_shadow_picker, 12, LV_PART_KNOB);
    lv_obj_set_style_radius(s_shadow_picker, 6, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_shadow_picker, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_shadow_picker, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(s_shadow_picker, shadow_colorwheel_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *shadow_row1 = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_row1, 320, 44);
    lv_obj_set_style_bg_opa(shadow_row1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_row1, 0, 0);
    lv_obj_set_flex_flow(shadow_row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shadow_row1, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(shadow_row1, 10, 0);
    lv_obj_set_style_pad_bottom(shadow_row1, 2, 0);
    lv_obj_clear_flag(shadow_row1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_row1, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *shadow_row2 = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_row2, 320, 44);
    lv_obj_set_style_bg_opa(shadow_row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_row2, 0, 0);
    lv_obj_set_flex_flow(shadow_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shadow_row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(shadow_row2, 10, 0);
    lv_obj_set_style_pad_bottom(shadow_row2, 2, 0);
    lv_obj_clear_flag(shadow_row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_row2, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < 8; i++) {
        lv_obj_t *parent = (i < 4) ? shadow_row1 : shadow_row2;
        lv_obj_t *dot = lv_obj_create(parent);
        lv_obj_set_size(dot, 26, 26);
        lv_obj_set_style_radius(dot, 13, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(s_color_presets[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(dot, LV_DIR_NONE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(dot, shadow_modal_select_event, LV_EVENT_CLICKED, (void *)(uintptr_t)s_color_presets[i]);
        lv_obj_set_user_data(dot, (void *)(uintptr_t)s_color_presets[i]);
        s_shadow_modal_dots[i] = dot;
    }

    update_accent_dots();
    update_shadow_dots();
}

void ui_settings_set_state(app_state_t *state)
{
    s_state = state;
        if (s_state) {
            apply_prefs_to_controls();
        }
}

static void apply_prefs_to_controls(void)
{
    if (!s_state) {
        return;
    }

    if (s_refresh_slider) {
        apply_refresh_range();
    }
    if (s_brightness_slider) {
        lv_slider_set_value(s_brightness_slider, s_state->prefs.brightness, LV_ANIM_OFF);
    }
    if (s_button3d_toggle) {
        if (s_state->prefs.buttons_3d) {
            lv_obj_add_state(s_button3d_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_button3d_toggle, LV_STATE_CHECKED);
        }
    }
    if (s_dark_toggle) {
        if (s_state->prefs.dark_mode) {
            lv_obj_add_state(s_dark_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_dark_toggle, LV_STATE_CHECKED);
        }
    }
    update_source_buttons();
    update_accent_dots();
}


static void close_scan_modal(void)
{
    if (s_scan_modal) {
        lv_obj_del(s_scan_modal);
        s_scan_modal = NULL;
        s_scan_list = NULL;
        s_saved_list = NULL;
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

    char connected_ssid[33] = {0};
    bool has_connected = wifi_manager_get_connected_ssid(connected_ssid, sizeof(connected_ssid));

    for (size_t pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < count; i++) {
            bool is_connected = has_connected && strcasecmp(networks[i].ssid, connected_ssid) == 0;
            if ((pass == 0 && !is_connected) || (pass == 1 && is_connected)) {
                continue;
            }

            lv_obj_t *row = lv_btn_create(s_saved_list);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_style_bg_color(row, lv_color_hex(SETTINGS_BTN_BG), 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_left(row, 8, 0);
            lv_obj_set_style_pad_right(row, 8, 0);
            lv_obj_set_style_pad_top(row, 6, 0);
            lv_obj_set_style_pad_bottom(row, 6, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            char *ssid_copy = dup_ssid(networks[i].ssid);
            lv_obj_add_event_cb(row, saved_row_event, LV_EVENT_CLICKED, ssid_copy);
            lv_obj_add_event_cb(row, free_ssid_event, LV_EVENT_DELETE, ssid_copy);

            lv_obj_t *label = lv_label_create(row);
            lv_label_set_text(label, networks[i].ssid);
            lv_obj_set_style_text_color(label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

            if (is_connected) {
                lv_obj_t *check = lv_label_create(row);
                lv_label_set_text(check, LV_SYMBOL_OK);
                lv_obj_set_style_text_color(check, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
            }
        }
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
    open_password_modal(ssid);
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
    lv_obj_set_size(card, 560, 380);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Wi-Fi Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);

    lv_obj_t *saved_label = lv_label_create(card);
    lv_label_set_text(saved_label, "Saved Networks");
    lv_obj_set_style_text_color(saved_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(saved_label, LV_ALIGN_TOP_LEFT, 0, 42);

    s_saved_list = lv_obj_create(card);
    lv_obj_set_size(s_saved_list, 520, 120);
    lv_obj_align(s_saved_list, LV_ALIGN_TOP_LEFT, 0, 64);
    lv_obj_set_style_bg_color(s_saved_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_saved_list, 0, 0);
    lv_obj_set_style_pad_left(s_saved_list, 8, 0);
    lv_obj_set_style_pad_right(s_saved_list, 8, 0);
    lv_obj_set_style_pad_top(s_saved_list, 4, 0);
    lv_obj_set_style_pad_bottom(s_saved_list, 4, 0);
    lv_obj_set_style_pad_row(s_saved_list, 6, 0);
    lv_obj_set_flex_flow(s_saved_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_saved_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_saved_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *avail_label = lv_label_create(card);
    lv_label_set_text(avail_label, "Available Networks");
    lv_obj_set_style_text_color(avail_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(avail_label, LV_ALIGN_TOP_LEFT, 0, 194);

    s_scan_list = lv_obj_create(card);
    lv_obj_set_size(s_scan_list, 520, 120);
    lv_obj_align(s_scan_list, LV_ALIGN_TOP_LEFT, 0, 216);
    lv_obj_set_style_bg_color(s_scan_list, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(s_scan_list, 0, 0);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_scan_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scan_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *placeholder = lv_label_create(s_scan_list);
    lv_label_set_text(placeholder, "Scanning...");
    lv_obj_set_style_text_color(placeholder, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    refresh_saved_list();

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
    refresh_saved_list();
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

    lv_obj_t *display_label = lv_label_create(screen);
    lv_label_set_text(display_label, "Display");
    lv_obj_set_style_text_color(display_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(display_label, LV_ALIGN_TOP_LEFT, 16, 50);

    lv_obj_t *display_card = lv_obj_create(screen);
    lv_obj_set_size(display_card, 360, 200);
    lv_obj_align(display_card, LV_ALIGN_TOP_LEFT, 16, 72);
    lv_obj_set_style_bg_color(display_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(display_card, 0, 0);
    lv_obj_set_style_radius(display_card, 12, 0);
    lv_obj_set_style_pad_all(display_card, 12, 0);

    lv_obj_t *brightness_label = lv_label_create(display_card);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_brightness_slider = lv_slider_create(display_card);
    lv_obj_set_width(s_brightness_slider, 320);
    lv_obj_align(s_brightness_slider, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_slider_set_range(s_brightness_slider, 0, 100);
    lv_slider_set_value(s_brightness_slider, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_brightness_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh_label = lv_label_create(display_card);
    lv_label_set_text(refresh_label, "Refresh interval");
    lv_obj_set_style_text_color(refresh_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(refresh_label, LV_ALIGN_TOP_LEFT, 0, 70);

    s_refresh_slider = lv_slider_create(display_card);
    lv_obj_set_width(s_refresh_slider, 220);
    lv_obj_align(s_refresh_slider, LV_ALIGN_TOP_LEFT, 0, 96);
    lv_slider_set_range(s_refresh_slider, 5, 60);
    lv_slider_set_value(s_refresh_slider, 20, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_refresh_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_refresh_slider, refresh_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_refresh_value = lv_label_create(display_card);
    lv_label_set_text(s_refresh_value, "20s");
    lv_obj_set_style_text_color(s_refresh_value, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(s_refresh_value, LV_ALIGN_TOP_LEFT, 230, 92);

    lv_obj_t *source_label = lv_label_create(display_card);
    lv_label_set_text(source_label, "Data source");
    lv_obj_set_style_text_color(source_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(source_label, LV_ALIGN_TOP_LEFT, 0, 126);

    s_source_coin_btn = lv_btn_create(display_card);
    lv_obj_set_size(s_source_coin_btn, 150, 26);
    lv_obj_align(s_source_coin_btn, LV_ALIGN_TOP_LEFT, 0, 150);
    style_button(s_source_coin_btn);
    lv_obj_add_event_cb(s_source_coin_btn, data_source_event, LV_EVENT_CLICKED, NULL);
    s_source_coin_label = lv_label_create(s_source_coin_btn);
    lv_label_set_text(s_source_coin_label, "CoinGecko");
    style_button_label(s_source_coin_label);
    lv_obj_center(s_source_coin_label);

    s_source_kraken_btn = lv_btn_create(display_card);
    lv_obj_set_size(s_source_kraken_btn, 150, 26);
    lv_obj_align(s_source_kraken_btn, LV_ALIGN_TOP_LEFT, 170, 150);
    style_button(s_source_kraken_btn);
    lv_obj_add_event_cb(s_source_kraken_btn, data_source_event, LV_EVENT_CLICKED, NULL);
    s_source_kraken_label = lv_label_create(s_source_kraken_btn);
    lv_label_set_text(s_source_kraken_label, "Kraken");
    style_button_label(s_source_kraken_label);
    lv_obj_center(s_source_kraken_label);

    lv_obj_t *system_label = lv_label_create(screen);
    lv_label_set_text(system_label, "System");
    lv_obj_set_style_text_color(system_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(system_label, LV_ALIGN_TOP_LEFT, 16, 286);

    lv_obj_t *system_card = lv_obj_create(screen);
    lv_obj_set_size(system_card, 360, 140);
    lv_obj_align(system_card, LV_ALIGN_TOP_LEFT, 16, 308);
    lv_obj_set_style_bg_color(system_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(system_card, 0, 0);
    lv_obj_set_style_radius(system_card, 12, 0);
    lv_obj_set_style_pad_all(system_card, 12, 0);
    lv_obj_clear_flag(system_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(system_card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *actions_row = lv_obj_create(system_card);
    lv_obj_set_size(actions_row, 336, 44);
    lv_obj_align(actions_row, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(actions_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions_row, 0, 0);
    lv_obj_set_flex_flow(actions_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(actions_row, 0, 0);
    lv_obj_set_style_pad_column(actions_row, 12, 0);
    lv_obj_clear_flag(actions_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(actions_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *touch_btn = lv_btn_create(actions_row);
    lv_obj_set_size(touch_btn, 162, 36);
    lv_obj_set_style_radius(touch_btn, 18, 0);
    lv_obj_set_style_pad_left(touch_btn, 8, 0);
    lv_obj_set_style_pad_right(touch_btn, 8, 0);
    style_button(touch_btn);
    lv_obj_clear_flag(touch_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(touch_btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(touch_btn, touch_calibrate_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *touch_label = lv_label_create(touch_btn);
    lv_label_set_text(touch_label, "Touch Cal");
    style_button_label(touch_label);
    lv_obj_center(touch_label);

    lv_obj_t *accent_btn = lv_btn_create(actions_row);
    lv_obj_set_size(accent_btn, 162, 36);
    lv_obj_set_style_radius(accent_btn, 18, 0);
    lv_obj_set_style_pad_left(accent_btn, 8, 0);
    lv_obj_set_style_pad_right(accent_btn, 8, 0);
    style_button(accent_btn);
    lv_obj_set_style_bg_opa(accent_btn, LV_OPA_COVER, 0);
    lv_obj_clear_flag(accent_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(accent_btn, LV_DIR_NONE);
    lv_obj_clear_flag(accent_btn, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(accent_btn, accent_modal_open_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *accent_label = lv_label_create(accent_btn);
    lv_label_set_text(accent_label, "Accent");
    style_button_label(accent_label);
    lv_obj_align(accent_label, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_clear_flag(accent_label, LV_OBJ_FLAG_CLICKABLE);

    static const uint32_t accents_pill[4] = {0x00FE8F, 0x00C2FF, 0xF15BB5, 0xF5A623};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *dot = lv_obj_create(accent_btn);
        lv_obj_set_size(dot, 12, 12);
        lv_obj_set_style_radius(dot, 6, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(accents_pill[i]), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(dot, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_scroll_dir(dot, LV_DIR_NONE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_RIGHT_MID, -8 - ((3 - i) * 16), 0);
    }

    lv_obj_t *button3d_label = lv_label_create(system_card);
    lv_label_set_text(button3d_label, "Button Shadows");
    lv_obj_set_style_text_color(button3d_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(button3d_label, LV_ALIGN_TOP_LEFT, 0, 54);

    s_button3d_toggle = lv_switch_create(system_card);
    lv_obj_align(s_button3d_toggle, LV_ALIGN_TOP_RIGHT, 0, 48);
    style_settings_switch(s_button3d_toggle);
    lv_obj_set_style_bg_color(s_button3d_toggle, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_button3d_toggle, button3d_toggle_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dark_label = lv_label_create(system_card);
    lv_label_set_text(dark_label, "Dark Mode");
    lv_obj_set_style_text_color(dark_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(dark_label, LV_ALIGN_TOP_LEFT, 0, 86);

    s_dark_toggle = lv_switch_create(system_card);
    lv_obj_align(s_dark_toggle, LV_ALIGN_TOP_RIGHT, 0, 80);
    style_settings_switch(s_dark_toggle);
    lv_obj_set_style_bg_color(s_dark_toggle, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dark_toggle, dark_mode_toggle_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi_label = lv_label_create(screen);
    lv_label_set_text(wifi_label, "Connectivity");
    lv_obj_set_style_text_color(wifi_label, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(wifi_label, LV_ALIGN_TOP_LEFT, 420, 50);

    lv_obj_t *wifi_card = lv_obj_create(screen);
    lv_obj_set_size(wifi_card, 360, 140);
    lv_obj_align(wifi_card, LV_ALIGN_TOP_LEFT, 420, 72);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(wifi_card, 0, 0);
    lv_obj_set_style_radius(wifi_card, 12, 0);
    lv_obj_set_style_pad_all(wifi_card, 12, 0);

    lv_obj_t *wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_size(wifi_btn, 180, 36);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 10);
    lv_obj_add_event_cb(wifi_btn, scan_wifi_event, LV_EVENT_CLICKED, NULL);
    style_button(wifi_btn);
    lv_obj_t *wifi_btn_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_btn_label, "Wi-Fi Settings");
    style_button_label(wifi_btn_label);
    lv_obj_center(wifi_btn_label);

    s_wifi_status_label = lv_label_create(wifi_card);
    lv_label_set_text(s_wifi_status_label, "Network Status: ...");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 58);

    s_wifi_ssid_label = lv_label_create(wifi_card);
    lv_label_set_text(s_wifi_ssid_label, "SSID: --");
    lv_obj_set_style_text_color(s_wifi_ssid_label, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(s_wifi_ssid_label, LV_ALIGN_TOP_LEFT, 0, 82);

    lv_obj_t *footer_row = lv_obj_create(screen);
    lv_obj_set_size(footer_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(footer_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer_row, 0, 0);
    lv_obj_set_flex_flow(footer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(footer_row, 4, 0);
    lv_obj_align(footer_row, LV_ALIGN_BOTTOM_RIGHT, -16, -10);

    lv_obj_t *footer_text = lv_label_create(footer_row);
    lv_label_set_text(footer_text, "CrowPanel Advance 7 | PCLK 16 MHz |");
    lv_obj_set_style_text_color(footer_text, lv_color_hex(0x9AA1AD), 0);
    lv_obj_set_style_text_font(footer_text, &lv_font_montserrat_16, 0);

    lv_obj_t *footer_branch = lv_label_create(footer_row);
    lv_label_set_text(footer_branch, CT_BUILD_BRANCH_STR);
    lv_obj_set_style_text_color(footer_branch, lv_color_hex(0x9AA1AD), 0);
    lv_obj_set_style_text_font(footer_branch, &lv_font_montserrat_12, 0);

    update_wifi_status_label();
    if (!s_wifi_status_timer) {
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 1000, NULL);
    }

    apply_prefs_to_controls();

    ui_nav_attach(screen, UI_NAV_SETTINGS);
    return screen;
}
