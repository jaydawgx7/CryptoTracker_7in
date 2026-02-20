#include "ui/ui_settings.h"

#include <stdio.h>

#include "lvgl.h"

#include "services/control_mcu.h"
#include "services/github_update.h"
#include "services/nvs_store.h"
#include "services/ota_update.h"
#include "services/touch_driver.h"
#include "services/wifi_manager.h"

#include "esp_err.h"
#include "esp_timer.h"

#include "ui/ui.h"
#include "ui/ui_nav.h"
#include "ui/ui_theme.h"
#include "app_version.h"
#include "models/app_state.h"

#ifndef CT_KEYBOARD_ENABLE
#define CT_KEYBOARD_ENABLE 0
#endif

#define SETTINGS_TEXT_COLOR settings_accent_color()
#define SETTINGS_MUTED_COLOR settings_muted_color()
#define SETTINGS_BTN_BG 0x222222
#define SETTINGS_PRESET_COUNT 9

#ifndef CT_BUILD_BRANCH_STR
#define CT_BUILD_BRANCH_STR "dev"
#endif

static lv_obj_t *s_saved_list = NULL;
static lv_obj_t *s_scan_list = NULL;
static lv_obj_t *s_scan_modal = NULL;
static lv_obj_t *s_wifi_modal = NULL;
static lv_obj_t *s_accent_modal = NULL;
static lv_obj_t *s_accent_sheet = NULL;
static lv_obj_t *s_pass_field = NULL;
static lv_obj_t *s_wifi_keyboard = NULL;
static lv_obj_t *s_wifi_status_label = NULL;
static lv_obj_t *s_wifi_ssid_label = NULL;
static lv_obj_t *s_wifi_ip_label = NULL;
static lv_timer_t *s_wifi_status_timer = NULL;
static lv_timer_t *s_ota_status_timer = NULL;
static lv_obj_t *s_update_check_btn = NULL;
static lv_obj_t *s_update_install_btn = NULL;
static lv_obj_t *s_update_install_label = NULL;
static lv_obj_t *s_update_status_label = NULL;
static lv_obj_t *s_update_last_label = NULL;
static lv_obj_t *s_update_notes_label = NULL;
static lv_obj_t *s_demo_portfolio_toggle = NULL;
static lv_timer_t *s_update_status_timer = NULL;
static lv_obj_t *s_ota_overlay = NULL;
static lv_obj_t *s_ota_overlay_status = NULL;
static lv_obj_t *s_ota_overlay_arc = NULL;
static lv_obj_t *s_ota_overlay_percent = NULL;
static lv_obj_t *s_ota_overlay_back_btn = NULL;
static bool s_ota_overlay_active = false;
static int s_ota_overlay_last_progress = -1;
static bool s_ota_overlay_last_failed = false;
static int64_t s_ota_overlay_last_update_ms = 0;
static char s_ota_overlay_last_status[128] = {0};
static lv_obj_t *s_brightness_slider = NULL;
static lv_obj_t *s_refresh_slider = NULL;
static lv_obj_t *s_refresh_value = NULL;
static lv_obj_t *s_button3d_toggle = NULL;
static lv_obj_t *s_dark_toggle = NULL;
static lv_obj_t *s_accent_modal_dots[SETTINGS_PRESET_COUNT] = {0};
static lv_obj_t *s_shadow_modal_dots[SETTINGS_PRESET_COUNT] = {0};
static lv_obj_t *s_accent_hue_slider = NULL;
static lv_obj_t *s_shadow_hue_slider = NULL;
static lv_obj_t *s_accent_gray_slider = NULL;
static lv_obj_t *s_shadow_gray_slider = NULL;
static lv_obj_t *s_theme_preview_btn = NULL;
static lv_obj_t *s_theme_preview_label = NULL;
static lv_obj_t *s_shadow_strength_checks[3] = {0};
static bool s_reopen_modal_after_theme_apply = false;
static uint32_t s_accent_modal_initial_accent = 0;
static uint32_t s_accent_modal_initial_shadow = 0;
static button_shadow_strength_t s_accent_modal_initial_shadow_strength = BUTTON_SHADOW_MAXIMUM;
static bool s_accent_modal_has_initial = false;
static lv_obj_t *s_source_coin_btn = NULL;
static lv_obj_t *s_source_kraken_btn = NULL;
static lv_obj_t *s_source_coin_label = NULL;
static lv_obj_t *s_source_kraken_label = NULL;
static lv_obj_t *s_touch_reset_modal = NULL;
static bool s_touch_reset_consumed_click = false;
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
static void accent_hue_slider_event(lv_event_t *e);
static void shadow_hue_slider_event(lv_event_t *e);
static void accent_gray_slider_event(lv_event_t *e);
static void shadow_gray_slider_event(lv_event_t *e);
static void accent_modal_open_event(lv_event_t *e);
static void accent_modal_close_event(lv_event_t *e);
static void accent_sheet_event(lv_event_t *e);
static void button3d_toggle_event(lv_event_t *e);
static void dark_mode_toggle_event(lv_event_t *e);
static void demo_portfolio_toggle_event(lv_event_t *e);
static void data_source_event(lv_event_t *e);
static void update_source_buttons(void);
static void apply_refresh_range(void);
static uint16_t clamp_refresh_value(int32_t value);
static void style_settings_switch(lv_obj_t *toggle);
static void style_gray_slider(lv_obj_t *slider);
static void update_gray_slider_knob(lv_obj_t *slider, uint8_t value);
static void style_hue_slider(lv_obj_t *slider);
static void update_hue_slider_knob(lv_obj_t *slider, uint16_t hue);
static void hue_slider_draw_event(lv_event_t *e);
static void update_theme_preview_button(void);
static void update_shadow_strength_checks(void);
static void shadow_strength_event(lv_event_t *e);
static void accent_modal_preview_async_cb(void *user_data);
static void apply_theme_async_cb(void *user_data);
static void accent_modal_clear_refs(void);
static void accent_modal_cancel_and_close(void);
static void accent_modal_apply_and_close(void);
static void accent_modal_preview_event(lv_event_t *e);
static void accent_modal_cancel_btn_event(lv_event_t *e);
static void accent_modal_apply_btn_event(lv_event_t *e);
static void update_ota_status(void);
static void ota_status_timer_cb(lv_timer_t *timer);
static void update_update_status(void);
static void update_status_timer_cb(lv_timer_t *timer);
static void github_check_event(lv_event_t *e);
static void github_install_event(lv_event_t *e);
static void ota_overlay_back_event(lv_event_t *e);
static void ota_overlay_set_visible(bool visible);
static void ota_overlay_update(int progress, const char *status_text, bool failed);

static const uint32_t s_color_presets[SETTINGS_PRESET_COUNT] = {
    0x00FE8F,
    0xFF8A00,
    0xF15BB5,
    0x00E5FF,
    0xBB86FC,
    0xF85A01,
    0x01F701,
    0xFFFC19,
    0x2273FF
};

static uint32_t settings_accent_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return theme ? theme->accent : 0x00FE8F;
}

static uint32_t settings_muted_color(void)
{
    const ui_theme_colors_t *theme = ui_theme_get();
    return theme ? theme->text_muted : 0x9AA1AD;
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

static uint8_t gray_value_from_hex(uint32_t color_hex)
{
    uint32_t r = (color_hex >> 16) & 0xFF;
    uint32_t g = (color_hex >> 8) & 0xFF;
    uint32_t b = color_hex & 0xFF;
    return (uint8_t)((r * 30 + g * 59 + b * 11) / 100);
}

static uint32_t gray_hex_from_value(uint8_t value)
{
    return ((uint32_t)value << 16) | ((uint32_t)value << 8) | value;
}

static uint16_t hue_value_from_hex(uint32_t color_hex)
{
    lv_color_hsv_t hsv = lv_color_to_hsv(lv_color_hex(color_hex));
    return (uint16_t)hsv.h;
}

static uint32_t hue_hex_from_value(uint16_t hue)
{
    lv_color_t c = lv_color_hsv_to_rgb((uint16_t)(hue % 360), 100, 100);
    return lv_color_to32(c) & 0x00FFFFFFU;
}

static void style_gray_slider(lv_obj_t *slider)
{
    if (!slider) {
        return;
    }

    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(slider, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(slider, LV_GRAD_DIR_HOR, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);

    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_left(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(slider, 10, LV_PART_MAIN);
}

static void update_gray_slider_knob(lv_obj_t *slider, uint8_t value)
{
    if (!slider) {
        return;
    }
    lv_obj_set_style_bg_color(slider, lv_color_hex(gray_hex_from_value(value)), LV_PART_KNOB);
}

static void style_hue_slider(lv_obj_t *slider)
{
    if (!slider) {
        return;
    }

    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_left(slider, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(slider, 10, LV_PART_MAIN);

    lv_obj_add_event_cb(slider, hue_slider_draw_event, LV_EVENT_DRAW_PART_BEGIN, NULL);
}

static void update_hue_slider_knob(lv_obj_t *slider, uint16_t hue)
{
    if (!slider) {
        return;
    }
    lv_obj_set_style_bg_color(slider, lv_color_hex(hue_hex_from_value(hue)), LV_PART_KNOB);
}

static void hue_slider_draw_event(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = (lv_obj_draw_part_dsc_t *)lv_event_get_param(e);
    if (!dsc || dsc->part != LV_PART_MAIN) {
        return;
    }

    const lv_area_t *a = dsc->draw_area;
    if (!a) {
        return;
    }

    int w = lv_area_get_width(a);
    int h = lv_area_get_height(a);
    if (w <= 2 || h <= 2) {
        return;
    }

    const int segments = 120;
    int cap = h / 2;
    if (cap < 1) {
        cap = 1;
    }

    int inner_x1 = a->x1 + cap;
    int inner_x2 = a->x2 - cap;
    int inner_w = inner_x2 - inner_x1 + 1;
    if (inner_w < 1) {
        inner_x1 = a->x1;
        inner_x2 = a->x2;
        inner_w = w;
        cap = 0;
    }

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.border_width = 0;
    rect.bg_opa = LV_OPA_COVER;
    rect.radius = 0;

    for (int i = 0; i < segments; i++) {
        int x1 = inner_x1 + (i * inner_w) / segments;
        int x2 = inner_x1 + ((i + 1) * inner_w) / segments - 1;
        if (i == segments - 1) {
            x2 = inner_x2;
        }
        if (x1 > inner_x2) {
            break;
        }

        uint16_t hue = (uint16_t)((i * 360) / segments);
        rect.bg_color = lv_color_hex(hue_hex_from_value(hue));
        lv_area_t seg = {.x1 = x1, .y1 = a->y1, .x2 = x2, .y2 = a->y2};
        lv_draw_rect(dsc->draw_ctx, &rect, &seg);
    }

    if (cap > 0) {
        rect.radius = LV_RADIUS_CIRCLE;

        rect.bg_color = lv_color_hex(hue_hex_from_value(0));
        lv_area_t left_cap = {.x1 = a->x1, .y1 = a->y1, .x2 = a->x1 + (cap * 2) - 1, .y2 = a->y2};
        lv_draw_rect(dsc->draw_ctx, &rect, &left_cap);

        rect.bg_color = lv_color_hex(hue_hex_from_value(359));
        lv_area_t right_cap = {.x1 = a->x2 - (cap * 2) + 1, .y1 = a->y1, .x2 = a->x2, .y2 = a->y2};
        lv_draw_rect(dsc->draw_ctx, &rect, &right_cap);
    }
}

static button_shadow_strength_t get_shadow_strength_pref(void)
{
    if (!s_state) {
        return BUTTON_SHADOW_MAXIMUM;
    }

    int value = (int)s_state->prefs.button_shadow_strength;
    if (value < BUTTON_SHADOW_MINIMAL) {
        value = BUTTON_SHADOW_MINIMAL;
    } else if (value > BUTTON_SHADOW_MAXIMUM) {
        value = BUTTON_SHADOW_MAXIMUM;
    }
    return (button_shadow_strength_t)value;
}

static void get_shadow_strength_style(button_shadow_strength_t level, int16_t *width, int16_t *ofs_y, lv_opa_t *opa)
{
    if (!width || !ofs_y || !opa) {
        return;
    }

    switch (level) {
        case BUTTON_SHADOW_MINIMAL:
            *width = 6;
            *ofs_y = 1;
            *opa = LV_OPA_80;
            break;
        case BUTTON_SHADOW_MEDIUM:
            *width = 8;
            *ofs_y = 3;
            *opa = LV_OPA_80;
            break;
        default:
            *width = 10;
            *ofs_y = 5;
            *opa = LV_OPA_80;
            break;
    }
}

static void update_shadow_strength_checks(void)
{
    button_shadow_strength_t level = get_shadow_strength_pref();

    for (int i = 0; i < 3; i++) {
        if (!s_shadow_strength_checks[i]) {
            continue;
        }

        if ((int)level == i) {
            lv_obj_add_state(s_shadow_strength_checks[i], LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_shadow_strength_checks[i], LV_STATE_CHECKED);
        }
    }
}

static void shadow_strength_event(lv_event_t *e)
{
    if (!s_state) {
        return;
    }

    int index = (int)(uintptr_t)lv_event_get_user_data(e);
    if (index < BUTTON_SHADOW_MINIMAL) {
        index = BUTTON_SHADOW_MINIMAL;
    } else if (index > BUTTON_SHADOW_MAXIMUM) {
        index = BUTTON_SHADOW_MAXIMUM;
    }

    s_state->prefs.button_shadow_strength = (button_shadow_strength_t)index;
    update_shadow_strength_checks();
    update_theme_preview_button();
}

static void update_theme_preview_button(void)
{
    if (!s_theme_preview_btn || !s_theme_preview_label) {
        return;
    }

    uint32_t accent = s_state ? s_state->prefs.accent_hex : 0x00FE8F;
    uint32_t shadow = s_state ? s_state->prefs.shadow_hex : 0x2A3142;
    int16_t shadow_width = 10;
    int16_t shadow_ofs_y = 5;
    lv_opa_t shadow_opa = LV_OPA_80;
    button_shadow_strength_t strength = get_shadow_strength_pref();

    get_shadow_strength_style(strength, &shadow_width, &shadow_ofs_y, &shadow_opa);
    if (s_state && !s_state->prefs.buttons_3d) {
        shadow_width = 0;
        shadow_ofs_y = 0;
        shadow_opa = LV_OPA_TRANSP;
    }

    style_button(s_theme_preview_btn);
    lv_obj_set_style_text_color(s_theme_preview_label, lv_color_hex(accent), 0);
    lv_obj_set_style_shadow_color(s_theme_preview_btn, lv_color_hex(shadow), 0);
    lv_obj_set_style_shadow_width(s_theme_preview_btn, shadow_width, 0);
    lv_obj_set_style_shadow_ofs_y(s_theme_preview_btn, shadow_ofs_y, 0);
    lv_obj_set_style_shadow_spread(s_theme_preview_btn, 0, 0);
    lv_obj_set_style_shadow_opa(s_theme_preview_btn, shadow_opa, 0);
}

static void accent_modal_clear_refs(void)
{
    s_accent_modal = NULL;
    s_accent_sheet = NULL;
    for (int i = 0; i < SETTINGS_PRESET_COUNT; i++) {
        s_accent_modal_dots[i] = NULL;
        s_shadow_modal_dots[i] = NULL;
    }
    for (int i = 0; i < 3; i++) {
        s_shadow_strength_checks[i] = NULL;
    }
    s_accent_gray_slider = NULL;
    s_shadow_gray_slider = NULL;
    s_accent_hue_slider = NULL;
    s_shadow_hue_slider = NULL;
    s_theme_preview_btn = NULL;
    s_theme_preview_label = NULL;
}

static void apply_theme_async_cb(void *user_data)
{
    bool dark_mode = (bool)(uintptr_t)user_data;
    ui_apply_theme(dark_mode);
}

static void accent_modal_cancel_and_close(void)
{
    bool has_state = (s_state != NULL);
    bool dark_mode = has_state ? s_state->prefs.dark_mode : true;

    if (has_state && s_accent_modal_has_initial) {
        s_state->prefs.accent_hex = s_accent_modal_initial_accent;
        s_state->prefs.shadow_hex = s_accent_modal_initial_shadow;
        s_state->prefs.button_shadow_strength = s_accent_modal_initial_shadow_strength;
    }

    if (s_accent_modal) {
        lv_obj_del_async(s_accent_modal);
    }
    accent_modal_clear_refs();
    s_accent_modal_has_initial = false;

    if (has_state) {
        lv_async_call(apply_theme_async_cb, (void *)(uintptr_t)dark_mode);
    }
}

static void accent_modal_apply_and_close(void)
{
    bool has_state = (s_state != NULL);
    bool dark_mode = has_state ? s_state->prefs.dark_mode : true;

    if (s_accent_modal) {
        lv_obj_del_async(s_accent_modal);
    }
    accent_modal_clear_refs();
    s_accent_modal_has_initial = false;

    if (has_state) {
        nvs_store_save_app_state(s_state);
        lv_async_call(apply_theme_async_cb, (void *)(uintptr_t)dark_mode);
    }
}

static void accent_modal_cancel_btn_event(lv_event_t *e)
{
    (void)e;
    accent_modal_cancel_and_close();
}

static void accent_modal_apply_btn_event(lv_event_t *e)
{
    (void)e;
    accent_modal_apply_and_close();
}

static void accent_modal_preview_event(lv_event_t *e)
{
    (void)e;
    if (!s_state) {
        return;
    }

    lv_async_call(accent_modal_preview_async_cb, NULL);
}

static void accent_modal_preview_async_cb(void *user_data)
{
    (void)user_data;
    if (!s_state) {
        return;
    }

    if (s_accent_modal) {
        lv_obj_del_async(s_accent_modal);
    }
    accent_modal_clear_refs();
    s_reopen_modal_after_theme_apply = true;
    lv_async_call(apply_theme_async_cb, (void *)(uintptr_t)s_state->prefs.dark_mode);
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

    if (s_wifi_ip_label) {
        char ip[16] = {0};
        bool has_ip = wifi_manager_get_ip(ip, sizeof(ip));
        if (state == WIFI_STATE_CONNECTED && has_ip) {
            char ip_line[32];
            snprintf(ip_line, sizeof(ip_line), "IP: %s", ip);
            lv_label_set_text(s_wifi_ip_label, ip_line);
        } else if (state == WIFI_STATE_CONNECTING) {
            lv_label_set_text(s_wifi_ip_label, "IP: Connecting");
        } else {
            lv_label_set_text(s_wifi_ip_label, "IP: --");
        }
    }
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_wifi_status_label();
}

static void ota_overlay_set_visible(bool visible)
{
    if (!s_ota_overlay) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);
    }

    if (visible && !s_ota_overlay_active) {
        if (s_wifi_status_timer) {
            lv_timer_pause(s_wifi_status_timer);
        }
        if (s_update_status_timer) {
            lv_timer_pause(s_update_status_timer);
        }
        s_ota_overlay_active = true;
    } else if (!visible && s_ota_overlay_active) {
        if (s_wifi_status_timer) {
            lv_timer_resume(s_wifi_status_timer);
        }
        if (s_update_status_timer) {
            lv_timer_resume(s_update_status_timer);
        }
        s_ota_overlay_active = false;
        s_ota_overlay_last_progress = -1;
        s_ota_overlay_last_failed = false;
        s_ota_overlay_last_update_ms = 0;
        s_ota_overlay_last_status[0] = '\0';
    }
}

static void ota_overlay_update(int progress, const char *status_text, bool failed)
{
    if (!s_ota_overlay) {
        return;
    }

    if (progress < 0) {
        progress = 0;
    } else if (progress > 100) {
        progress = 100;
    }

    int64_t now_ms = esp_timer_get_time() / 1000;
    const char *status = status_text ? status_text : "Installing...";
    bool changed = (progress != s_ota_overlay_last_progress) ||
                   (failed != s_ota_overlay_last_failed) ||
                   (strncmp(status, s_ota_overlay_last_status, sizeof(s_ota_overlay_last_status) - 1) != 0);
    bool force_update = failed || progress >= 100 || progress == 0;
    if (!force_update && !changed) {
        return;
    }
    if (!force_update && s_ota_overlay_last_update_ms > 0 &&
        (now_ms - s_ota_overlay_last_update_ms) < 1000 &&
        progress == s_ota_overlay_last_progress) {
        return;
    }

    ota_overlay_set_visible(true);

    if (s_ota_overlay_status) {
        lv_label_set_text(s_ota_overlay_status, status);
    }
    if (s_ota_overlay_arc) {
        lv_arc_set_value(s_ota_overlay_arc, progress);
    }
    if (s_ota_overlay_percent) {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", progress);
        lv_label_set_text(s_ota_overlay_percent, pct);
    }
    if (s_ota_overlay_back_btn) {
        if (failed) {
            lv_obj_clear_flag(s_ota_overlay_back_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_ota_overlay_back_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    s_ota_overlay_last_progress = progress;
    s_ota_overlay_last_failed = failed;
    s_ota_overlay_last_update_ms = now_ms;
    snprintf(s_ota_overlay_last_status, sizeof(s_ota_overlay_last_status), "%s", status);
}

static void ota_overlay_back_event(lv_event_t *e)
{
    (void)e;
    ota_overlay_set_visible(false);
}

static void update_ota_status(void)
{
    ota_status_t status = {0};
    ota_update_get_status(&status);

    int progress = 0;
    char text[128];

    if (status.state == OTA_STATE_DOWNLOADING) {
        progress = status.percent;
        if (progress < 0) {
            progress = 0;
        } else if (progress > 100) {
            progress = 100;
        }
        if (status.message[0] != '\0') {
            snprintf(text, sizeof(text), "Status: %s (%d%%)", status.message, progress);
        } else {
            snprintf(text, sizeof(text), "Status: Downloading (%d%%)", progress);
        }
        ota_overlay_update(progress, text, false);
    } else if (status.state == OTA_STATE_SUCCESS) {
        progress = 100;
        if (status.message[0] != '\0') {
            snprintf(text, sizeof(text), "Status: %s", status.message);
        } else {
            snprintf(text, sizeof(text), "Status: Success");
        }
        ota_overlay_update(progress, text, false);
    } else if (status.state == OTA_STATE_FAILED) {
        if (status.message[0] != '\0') {
            snprintf(text, sizeof(text), "Status: %s (err %d)", status.message, status.last_error);
        } else {
            snprintf(text, sizeof(text), "Status: Failed (err %d)", status.last_error);
        }
        ota_overlay_update(progress, text, true);
    } else {
        snprintf(text, sizeof(text), "Status: Idle");
        if (s_ota_overlay_active) {
            ota_overlay_set_visible(false);
        }
    }

    (void)text;
}

static void ota_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_ota_status();
}

static void update_update_status(void)
{
    github_update_status_t status = {0};
    github_update_get_status(&status);

    if (s_update_status_label) {
        const char *tag = status.latest_tag[0] ? status.latest_tag : "--";
        switch (status.state) {
            case GITHUB_UPDATE_CHECKING:
                lv_label_set_text(s_update_status_label, "Checking GitHub...");
                break;
            case GITHUB_UPDATE_AVAILABLE: {
                char text[64];
                snprintf(text, sizeof(text), "Update available: %s", tag);
                lv_label_set_text(s_update_status_label, text);
                break;
            }
            case GITHUB_UPDATE_UP_TO_DATE:
                lv_label_set_text(s_update_status_label, "Up to date!");
                break;
            case GITHUB_UPDATE_RATE_LIMITED:
                lv_label_set_text(s_update_status_label, "Rate limited, try later");
                break;
            case GITHUB_UPDATE_FAILED:
                lv_label_set_text(s_update_status_label, "Update check failed");
                break;
            default:
                lv_label_set_text(s_update_status_label, "Status: --");
                break;
        }
    }

    if (s_update_last_label) {
        if (status.last_checked_ms <= 0) {
            lv_label_set_text(s_update_last_label, "Last checked: --");
        } else {
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t delta_ms = now_ms - status.last_checked_ms;
            int total_sec = (delta_ms > 0) ? (int)(delta_ms / 1000) : 0;
            int mins = total_sec / 60;
            int secs = total_sec % 60;
            char text[48];
            if (mins > 0) {
                snprintf(text, sizeof(text), "Last checked: %dm %ds ago", mins, secs);
            } else {
                snprintf(text, sizeof(text), "Last checked: %ds ago", secs);
            }
            lv_label_set_text(s_update_last_label, text);
        }
    }

    if (s_update_notes_label) {
        if (status.notes[0] != '\0') {
            lv_label_set_text(s_update_notes_label, status.notes);
        } else {
            lv_label_set_text(s_update_notes_label, "Release notes: --");
        }
    }

    if (s_update_install_btn && s_update_install_label) {
        if (status.state == GITHUB_UPDATE_AVAILABLE && status.download_url[0] != '\0') {
            char text[48];
            const char *tag = status.latest_tag[0] ? status.latest_tag : "Update";
            snprintf(text, sizeof(text), "Install %s", tag);
            lv_label_set_text(s_update_install_label, text);
            lv_obj_clear_flag(s_update_install_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_update_install_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_update_status();
}

static void github_check_event(lv_event_t *e)
{
    (void)e;
    esp_err_t err = github_update_start_check();
    if (err == ESP_OK) {
        if (s_update_status_label) {
            lv_label_set_text(s_update_status_label, "Checking GitHub...");
        }
    } else if (err == ESP_ERR_INVALID_STATE) {
        if (s_update_status_label) {
            lv_label_set_text(s_update_status_label, "Checking GitHub...");
        }
    } else if (s_update_status_label) {
        char text[64];
        snprintf(text, sizeof(text), "Check failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_update_status_label, text);
    }
}

static void github_install_event(lv_event_t *e)
{
    (void)e;
    github_update_status_t status = {0};
    github_update_get_status(&status);

    if (status.state != GITHUB_UPDATE_AVAILABLE || status.download_url[0] == '\0') {
        if (s_update_status_label) {
            lv_label_set_text(s_update_status_label, "No update URL");
        }
        return;
    }

    esp_err_t err = ota_update_start(status.download_url);
    if (err == ESP_OK) {
        if (s_update_status_label) {
            lv_label_set_text(s_update_status_label, "Starting install...");
        }
        ota_overlay_set_visible(true);
        ota_overlay_update(0, "Status: Starting", false);
    } else if (s_update_status_label) {
        char text[96];
        snprintf(text, sizeof(text), "Install failed: %s", esp_err_to_name(err));
        lv_label_set_text(s_update_status_label, text);
    }
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
    int32_t max = 120;

    if (value < min) {
        value = min;
    } else if (value > max) {
        value = max;
    }

    int32_t step = 5;
    value = ((value + (step / 2)) / step) * step;

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

static void demo_portfolio_toggle_event(lv_event_t *e)
{
    lv_obj_t *toggle = lv_event_get_target(e);
    bool enabled = lv_obj_has_state(toggle, LV_STATE_CHECKED);

    if (!s_state) {
        return;
    }

    esp_err_t err = nvs_store_set_demo_portfolio(s_state, enabled);
    if (err != ESP_OK) {
        if (s_state->prefs.demo_portfolio) {
            lv_obj_add_state(toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(toggle, LV_STATE_CHECKED);
        }
        if (s_update_status_label) {
            lv_label_set_text(s_update_status_label, "Demo toggle failed");
        }
        return;
    }

    (void)nvs_store_save_app_state(s_state);
    ui_set_app_state(s_state);
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
    char value_buf[16];
    if (value > 59) {
        int minutes = (int)(value / 60);
        int seconds = (int)(value % 60);
        if (seconds == 0) {
            snprintf(value_buf, sizeof(value_buf), "%dm", minutes);
        } else {
            snprintf(value_buf, sizeof(value_buf), "%dm%ds", minutes, seconds);
        }
    } else {
        snprintf(value_buf, sizeof(value_buf), "%ds", (int)value);
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "Refresh interval: %s", value_buf);
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
    lv_obj_set_style_text_color(s_source_coin_btn, lv_color_hex(kraken ? SETTINGS_MUTED_COLOR : SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_color(s_source_kraken_btn, lv_color_hex(kraken ? SETTINGS_TEXT_COLOR : SETTINGS_MUTED_COLOR), 0);

    if (s_source_coin_label) {
        lv_obj_set_style_text_color(s_source_coin_label, lv_color_hex(kraken ? SETTINGS_MUTED_COLOR : SETTINGS_TEXT_COLOR), 0);
    }
    if (s_source_kraken_label) {
        lv_obj_set_style_text_color(s_source_kraken_label, lv_color_hex(kraken ? SETTINGS_TEXT_COLOR : SETTINGS_MUTED_COLOR), 0);
    }
}

static void apply_refresh_range(void)
{
    if (!s_state || !s_refresh_slider) {
        return;
    }

    lv_slider_set_range(s_refresh_slider, 5, 120);

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

    for (int i = 0; i < SETTINGS_PRESET_COUNT; i++) {
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

    for (int i = 0; i < SETTINGS_PRESET_COUNT; i++) {
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
    }
    if (s_accent_gray_slider) {
        uint8_t value = gray_value_from_hex(accent);
        lv_slider_set_value(s_accent_gray_slider, value, LV_ANIM_OFF);
        update_gray_slider_knob(s_accent_gray_slider, value);
    }
    if (s_accent_hue_slider) {
        uint16_t hue = hue_value_from_hex(accent);
        lv_slider_set_value(s_accent_hue_slider, hue, LV_ANIM_OFF);
        update_hue_slider_knob(s_accent_hue_slider, hue);
    }
    update_accent_dots();
    update_theme_preview_button();
}

static void shadow_modal_select_event(lv_event_t *e)
{
    uint32_t shadow = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_state) {
        s_state->prefs.shadow_hex = shadow;
    }
    if (s_shadow_gray_slider) {
        uint8_t value = gray_value_from_hex(shadow);
        lv_slider_set_value(s_shadow_gray_slider, value, LV_ANIM_OFF);
        update_gray_slider_knob(s_shadow_gray_slider, value);
    }
    if (s_shadow_hue_slider) {
        uint16_t hue = hue_value_from_hex(shadow);
        lv_slider_set_value(s_shadow_hue_slider, hue, LV_ANIM_OFF);
        update_hue_slider_knob(s_shadow_hue_slider, hue);
    }
    update_shadow_dots();
    update_theme_preview_button();
}

static void accent_hue_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    uint16_t hue = (uint16_t)lv_slider_get_value(slider);
    uint32_t hex = hue_hex_from_value(hue);

    if (s_state) {
        s_state->prefs.accent_hex = hex;
    }

    update_hue_slider_knob(slider, hue);
    update_accent_dots();
    update_theme_preview_button();
}

static void shadow_hue_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    uint16_t hue = (uint16_t)lv_slider_get_value(slider);
    uint32_t hex = hue_hex_from_value(hue);

    if (s_state) {
        s_state->prefs.shadow_hex = hex;
    }

    update_hue_slider_knob(slider, hue);
    update_shadow_dots();
    update_theme_preview_button();
}

static void accent_gray_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    uint32_t hex = gray_hex_from_value(value);

    if (s_state) {
        s_state->prefs.accent_hex = hex;
    }
    update_gray_slider_knob(slider, value);
    update_accent_dots();
    update_theme_preview_button();
}

static void shadow_gray_slider_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }

    lv_obj_t *slider = lv_event_get_target(e);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    uint32_t hex = gray_hex_from_value(value);

    if (s_state) {
        s_state->prefs.shadow_hex = hex;
    }
    update_gray_slider_knob(slider, value);
    update_shadow_dots();
    update_theme_preview_button();
}

static void accent_modal_close_event(lv_event_t *e)
{
    (void)e;
    accent_modal_cancel_and_close();
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

    const ui_theme_colors_t *theme = ui_theme_get();
    uint32_t bg = theme ? theme->bg : 0x0F1117;
    uint32_t surface = theme ? theme->surface : 0x1A1D26;
    uint32_t muted = theme ? theme->text_muted : 0x9AA1AD;
    uint32_t inactive = theme ? theme->nav_inactive_bg : 0x2A3142;

    s_accent_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_accent_modal, lv_obj_get_width(lv_layer_top()), lv_obj_get_height(lv_layer_top()));
    lv_obj_set_style_bg_color(s_accent_modal, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(s_accent_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_accent_modal, 0, 0);
    lv_obj_clear_flag(s_accent_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_accent_modal, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(s_accent_modal, accent_modal_close_event, LV_EVENT_CLICKED, NULL);

    s_accent_sheet = lv_obj_create(s_accent_modal);
    lv_obj_set_size(s_accent_sheet, 776, 392);
    lv_obj_center(s_accent_sheet);
    lv_obj_set_style_bg_color(s_accent_sheet, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(s_accent_sheet, 0, 0);
    lv_obj_set_style_radius(s_accent_sheet, 12, 0);
    lv_obj_set_style_pad_all(s_accent_sheet, 12, 0);
    lv_obj_set_style_shadow_width(s_accent_sheet, 0, 0);
    lv_obj_clear_flag(s_accent_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_accent_sheet, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(s_accent_sheet, LV_DIR_NONE);
    lv_obj_add_event_cb(s_accent_sheet, accent_sheet_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_accent_sheet);
    lv_label_set_text(title, "Theme Colors");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 2);

    lv_obj_t *subtitle = lv_label_create(s_accent_sheet);
    lv_label_set_text(subtitle, "Pick a preset or dial in neutral gray");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(muted), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 8, 30);

    lv_obj_t *content = lv_obj_create(s_accent_sheet);
    lv_obj_set_size(content, 752, 236);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_style_pad_column(content, 12, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_OFF);

    uint32_t accent_color = s_state ? s_state->prefs.accent_hex : 0x00FE8F;
    uint32_t shadow_color = s_state ? s_state->prefs.shadow_hex : 0x2A3142;

    if (s_state && !s_accent_modal_has_initial) {
        s_accent_modal_initial_accent = s_state->prefs.accent_hex;
        s_accent_modal_initial_shadow = s_state->prefs.shadow_hex;
        s_accent_modal_initial_shadow_strength = get_shadow_strength_pref();
        s_accent_modal_has_initial = true;
    }

    lv_obj_t *accent_col = lv_obj_create(content);
    lv_obj_set_size(accent_col, 370, 236);
    lv_obj_set_style_bg_color(accent_col, lv_color_hex(surface), 0);
    lv_obj_set_style_border_width(accent_col, 0, 0);
    lv_obj_set_style_radius(accent_col, 12, 0);
    lv_obj_set_style_pad_all(accent_col, 12, 0);
    lv_obj_set_flex_flow(accent_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(accent_col, 8, 0);
    lv_obj_set_flex_align(accent_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(accent_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *accent_title = lv_label_create(accent_col);
    lv_label_set_text(accent_title, "Accent Color");
    lv_obj_set_style_text_font(accent_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(accent_title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    lv_obj_t *accent_hint = lv_label_create(accent_col);
    lv_label_set_text(accent_hint, "Presets");
    lv_obj_set_style_text_color(accent_hint, lv_color_hex(muted), 0);

    lv_obj_t *accent_row = lv_obj_create(accent_col);
    lv_obj_set_size(accent_row, 332, 34);
    lv_obj_set_style_bg_opa(accent_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_row, 0, 0);
    lv_obj_set_style_pad_all(accent_row, 0, 0);
    lv_obj_set_style_pad_column(accent_row, 8, 0);
    lv_obj_set_flex_flow(accent_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accent_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(accent_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_row, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < SETTINGS_PRESET_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(accent_row);
        lv_obj_set_size(dot, 28, 28);
        lv_obj_set_style_radius(dot, 14, 0);
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

    lv_obj_t *accent_custom_title = lv_label_create(accent_col);
    lv_label_set_text(accent_custom_title, "Custom");
    lv_obj_set_style_text_color(accent_custom_title, lv_color_hex(muted), 0);

    lv_obj_t *accent_hue_row = lv_obj_create(accent_col);
    lv_obj_set_size(accent_hue_row, 332, 30);
    lv_obj_set_style_bg_opa(accent_hue_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_hue_row, 0, 0);
    lv_obj_set_style_pad_all(accent_hue_row, 0, 0);
    lv_obj_clear_flag(accent_hue_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_hue_row, LV_SCROLLBAR_MODE_OFF);

    s_accent_hue_slider = lv_slider_create(accent_hue_row);
    lv_obj_set_size(s_accent_hue_slider, 312, 18);
    lv_obj_center(s_accent_hue_slider);
    lv_slider_set_range(s_accent_hue_slider, 0, 359);
    uint16_t accent_hue = hue_value_from_hex(accent_color);
    lv_slider_set_value(s_accent_hue_slider, accent_hue, LV_ANIM_OFF);
    style_hue_slider(s_accent_hue_slider);
    lv_obj_set_style_width(s_accent_hue_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_accent_hue_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(s_accent_hue_slider, 9, LV_PART_KNOB);
    update_hue_slider_knob(s_accent_hue_slider, accent_hue);
    lv_obj_add_event_cb(s_accent_hue_slider, accent_hue_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_accent_hue_slider, accent_hue_slider_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *accent_gray_row = lv_obj_create(accent_col);
    lv_obj_set_size(accent_gray_row, 332, 30);
    lv_obj_set_style_bg_opa(accent_gray_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(accent_gray_row, 0, 0);
    lv_obj_set_style_pad_all(accent_gray_row, 0, 0);
    lv_obj_set_style_pad_column(accent_gray_row, 10, 0);
    lv_obj_set_flex_flow(accent_gray_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accent_gray_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(accent_gray_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(accent_gray_row, LV_SCROLLBAR_MODE_OFF);

    s_accent_gray_slider = lv_slider_create(accent_gray_row);
    lv_obj_set_size(s_accent_gray_slider, 312, 18);
    lv_obj_center(s_accent_gray_slider);
    lv_slider_set_range(s_accent_gray_slider, 0, 255);
    lv_slider_set_value(s_accent_gray_slider, gray_value_from_hex(accent_color), LV_ANIM_OFF);
    style_gray_slider(s_accent_gray_slider);
    lv_obj_set_style_width(s_accent_gray_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_accent_gray_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(s_accent_gray_slider, 9, LV_PART_KNOB);
    update_gray_slider_knob(s_accent_gray_slider, (uint8_t)lv_slider_get_value(s_accent_gray_slider));
    lv_obj_add_event_cb(s_accent_gray_slider, accent_gray_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_accent_gray_slider, accent_gray_slider_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *shadow_col = lv_obj_create(content);
    lv_obj_set_size(shadow_col, 370, 236);
    lv_obj_set_style_bg_color(shadow_col, lv_color_hex(surface), 0);
    lv_obj_set_style_border_width(shadow_col, 0, 0);
    lv_obj_set_style_radius(shadow_col, 12, 0);
    lv_obj_set_style_pad_all(shadow_col, 12, 0);
    lv_obj_set_flex_flow(shadow_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(shadow_col, 8, 0);
    lv_obj_set_flex_align(shadow_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(shadow_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_col, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *shadow_title = lv_label_create(shadow_col);
    lv_label_set_text(shadow_title, "Button Shadows");
    lv_obj_set_style_text_font(shadow_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(shadow_title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);

    lv_obj_t *shadow_hint = lv_label_create(shadow_col);
    lv_label_set_text(shadow_hint, "Presets");
    lv_obj_set_style_text_color(shadow_hint, lv_color_hex(muted), 0);

    lv_obj_t *shadow_row = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_row, 332, 34);
    lv_obj_set_style_bg_opa(shadow_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_row, 0, 0);
    lv_obj_set_style_pad_all(shadow_row, 0, 0);
    lv_obj_set_style_pad_column(shadow_row, 8, 0);
    lv_obj_set_flex_flow(shadow_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shadow_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(shadow_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_row, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < SETTINGS_PRESET_COUNT; i++) {
        lv_obj_t *dot = lv_obj_create(shadow_row);
        lv_obj_set_size(dot, 28, 28);
        lv_obj_set_style_radius(dot, 14, 0);
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

    lv_obj_t *shadow_custom_title = lv_label_create(shadow_col);
    lv_label_set_text(shadow_custom_title, "Custom");
    lv_obj_set_style_text_color(shadow_custom_title, lv_color_hex(muted), 0);

    lv_obj_t *shadow_hue_row = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_hue_row, 332, 30);
    lv_obj_set_style_bg_opa(shadow_hue_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_hue_row, 0, 0);
    lv_obj_set_style_pad_all(shadow_hue_row, 0, 0);
    lv_obj_clear_flag(shadow_hue_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_hue_row, LV_SCROLLBAR_MODE_OFF);

    s_shadow_hue_slider = lv_slider_create(shadow_hue_row);
    lv_obj_set_size(s_shadow_hue_slider, 312, 18);
    lv_obj_center(s_shadow_hue_slider);
    lv_slider_set_range(s_shadow_hue_slider, 0, 359);
    uint16_t shadow_hue = hue_value_from_hex(shadow_color);
    lv_slider_set_value(s_shadow_hue_slider, shadow_hue, LV_ANIM_OFF);
    style_hue_slider(s_shadow_hue_slider);
    lv_obj_set_style_width(s_shadow_hue_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_shadow_hue_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(s_shadow_hue_slider, 9, LV_PART_KNOB);
    update_hue_slider_knob(s_shadow_hue_slider, shadow_hue);
    lv_obj_add_event_cb(s_shadow_hue_slider, shadow_hue_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_shadow_hue_slider, shadow_hue_slider_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *shadow_gray_row = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_gray_row, 332, 30);
    lv_obj_set_style_bg_opa(shadow_gray_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_gray_row, 0, 0);
    lv_obj_set_style_pad_all(shadow_gray_row, 0, 0);
    lv_obj_set_style_pad_column(shadow_gray_row, 10, 0);
    lv_obj_set_flex_flow(shadow_gray_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shadow_gray_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(shadow_gray_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_gray_row, LV_SCROLLBAR_MODE_OFF);

    s_shadow_gray_slider = lv_slider_create(shadow_gray_row);
    lv_obj_set_size(s_shadow_gray_slider, 312, 18);
    lv_obj_center(s_shadow_gray_slider);
    lv_slider_set_range(s_shadow_gray_slider, 0, 255);
    lv_slider_set_value(s_shadow_gray_slider, gray_value_from_hex(shadow_color), LV_ANIM_OFF);
    style_gray_slider(s_shadow_gray_slider);
    lv_obj_set_style_width(s_shadow_gray_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_shadow_gray_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(s_shadow_gray_slider, 9, LV_PART_KNOB);
    update_gray_slider_knob(s_shadow_gray_slider, (uint8_t)lv_slider_get_value(s_shadow_gray_slider));
    lv_obj_add_event_cb(s_shadow_gray_slider, shadow_gray_slider_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_shadow_gray_slider, shadow_gray_slider_event, LV_EVENT_RELEASED, NULL);

    lv_obj_t *shadow_strength_row = lv_obj_create(shadow_col);
    lv_obj_set_size(shadow_strength_row, 332, 26);
    lv_obj_set_style_bg_opa(shadow_strength_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(shadow_strength_row, 0, 0);
    lv_obj_set_style_pad_all(shadow_strength_row, 0, 0);
    lv_obj_set_style_pad_column(shadow_strength_row, 8, 0);
    lv_obj_set_flex_flow(shadow_strength_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(shadow_strength_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(shadow_strength_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(shadow_strength_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *shadow_strength_label = lv_label_create(shadow_strength_row);
    lv_label_set_text(shadow_strength_label, "Button Shadow:");
    lv_obj_set_style_text_color(shadow_strength_label, lv_color_hex(muted), 0);
    lv_obj_set_style_text_font(shadow_strength_label, &lv_font_montserrat_12, 0);

    static const char *shadow_strength_names[3] = {"Min", "Med", "Max"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *chk = lv_checkbox_create(shadow_strength_row);
        lv_checkbox_set_text(chk, shadow_strength_names[i]);
        lv_obj_set_style_text_font(chk, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(chk, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
        lv_obj_set_style_pad_column(chk, 2, 0);
        lv_obj_set_style_bg_opa(chk, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(chk, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(chk, lv_color_hex(inactive), LV_PART_INDICATOR);
        lv_obj_set_style_border_width(chk, 1, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(chk, lv_color_hex(muted), LV_PART_INDICATOR);
        lv_obj_set_style_radius(chk, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(chk, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(chk, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR | LV_STATE_CHECKED);
        lv_obj_add_event_cb(chk, shadow_strength_event, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_shadow_strength_checks[i] = chk;
    }

    update_shadow_strength_checks();

    lv_obj_t *footer = lv_obj_create(s_accent_sheet);
    lv_obj_set_size(footer, 752, 46);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_set_style_pad_left(footer, 8, 0);
    lv_obj_set_style_pad_right(footer, 8, 0);
    lv_obj_set_style_pad_top(footer, 0, 0);
    lv_obj_set_style_pad_bottom(footer, 0, 0);
    lv_obj_set_style_pad_column(footer, 12, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(footer, LV_SCROLLBAR_MODE_OFF);

    s_theme_preview_btn = lv_btn_create(footer);
    lv_obj_set_size(s_theme_preview_btn, 160, 36);
    lv_obj_set_style_radius(s_theme_preview_btn, 18, 0);
    style_button(s_theme_preview_btn);
    lv_obj_add_event_cb(s_theme_preview_btn, accent_modal_preview_event, LV_EVENT_CLICKED, NULL);
    s_theme_preview_label = lv_label_create(s_theme_preview_btn);
    lv_label_set_text(s_theme_preview_label, "Preview");
    style_button_label(s_theme_preview_label);
    lv_obj_center(s_theme_preview_label);

    lv_obj_t *actions_right = lv_obj_create(footer);
    lv_obj_set_size(actions_right, 268, 36);
    lv_obj_set_style_bg_opa(actions_right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions_right, 0, 0);
    lv_obj_set_style_pad_left(actions_right, 8, 0);
    lv_obj_set_style_pad_right(actions_right, 8, 0);
    lv_obj_set_style_pad_top(actions_right, 0, 0);
    lv_obj_set_style_pad_bottom(actions_right, 0, 0);
    lv_obj_set_style_pad_column(actions_right, 10, 0);
    lv_obj_set_flex_flow(actions_right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions_right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(actions_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(actions_right, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(actions_right, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *cancel_btn = lv_btn_create(actions_right);
    lv_obj_set_size(cancel_btn, 120, 36);
    lv_obj_set_style_radius(cancel_btn, 18, 0);
    style_button(cancel_btn);
    lv_obj_add_event_cb(cancel_btn, accent_modal_cancel_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    style_button_label(cancel_label);
    lv_obj_center(cancel_label);

    lv_obj_t *apply_btn = lv_btn_create(actions_right);
    lv_obj_set_size(apply_btn, 120, 36);
    lv_obj_set_style_radius(apply_btn, 18, 0);
    style_button(apply_btn);
    lv_obj_add_event_cb(apply_btn, accent_modal_apply_btn_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apply_label = lv_label_create(apply_btn);
    lv_label_set_text(apply_label, "Apply");
    style_button_label(apply_label);
    lv_obj_center(apply_label);

    update_accent_dots();
    update_shadow_dots();
    update_theme_preview_button();
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
    if (s_demo_portfolio_toggle) {
        if (s_state->prefs.demo_portfolio) {
            lv_obj_add_state(s_demo_portfolio_toggle, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_demo_portfolio_toggle, LV_STATE_CHECKED);
        }
    }
    update_source_buttons();
    update_accent_dots();
    update_shadow_dots();

    if (s_accent_gray_slider) {
        uint8_t accent_gray = gray_value_from_hex(s_state->prefs.accent_hex);
        lv_slider_set_value(s_accent_gray_slider, accent_gray, LV_ANIM_OFF);
        update_gray_slider_knob(s_accent_gray_slider, accent_gray);
    }
    if (s_accent_hue_slider) {
        uint16_t accent_hue = hue_value_from_hex(s_state->prefs.accent_hex);
        lv_slider_set_value(s_accent_hue_slider, accent_hue, LV_ANIM_OFF);
        update_hue_slider_knob(s_accent_hue_slider, accent_hue);
    }
    if (s_shadow_gray_slider) {
        uint8_t shadow_gray = gray_value_from_hex(s_state->prefs.shadow_hex);
        lv_slider_set_value(s_shadow_gray_slider, shadow_gray, LV_ANIM_OFF);
        update_gray_slider_knob(s_shadow_gray_slider, shadow_gray);
    }
    if (s_shadow_hue_slider) {
        uint16_t shadow_hue = hue_value_from_hex(s_state->prefs.shadow_hex);
        lv_slider_set_value(s_shadow_hue_slider, shadow_hue, LV_ANIM_OFF);
        update_hue_slider_knob(s_shadow_hue_slider, shadow_hue);
    }

    update_theme_preview_button();
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

static void close_touch_reset_modal(void)
{
    if (s_touch_reset_modal) {
        lv_obj_del(s_touch_reset_modal);
        s_touch_reset_modal = NULL;
    }
}

static void touch_reset_modal_ok_event(lv_event_t *e)
{
    (void)e;
    close_touch_reset_modal();
}

static void open_touch_reset_modal(void)
{
    if (s_touch_reset_modal) {
        return;
    }

    s_touch_reset_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_touch_reset_modal, 800, 480);
    lv_obj_set_style_bg_color(s_touch_reset_modal, lv_color_hex(0x0B0D12), 0);
    lv_obj_set_style_bg_opa(s_touch_reset_modal, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_touch_reset_modal, 0, 0);

    lv_obj_t *card = lv_obj_create(s_touch_reset_modal);
    lv_obj_set_size(card, 460, 190);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Touch Cal Reset");
    lv_obj_set_style_text_color(title, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_text(body, "Calibration data discarded.\nRun Touch Cal to recalibrate.");
    lv_obj_set_style_text_color(body, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 46);

    lv_obj_t *ok_btn = lv_btn_create(card);
    lv_obj_set_size(ok_btn, 120, 36);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(ok_btn, touch_reset_modal_ok_event, LV_EVENT_CLICKED, NULL);
    style_button(ok_btn);
    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    style_button_label(ok_label);
    lv_obj_center(ok_label);
}

static void touch_calibrate_event(lv_event_t *e)
{
    (void)e;

    if (s_touch_reset_consumed_click) {
        s_touch_reset_consumed_click = false;
        return;
    }

    ui_show_touch_calibration();
}

static void touch_calibrate_reset_event(lv_event_t *e)
{
    (void)e;
    touch_driver_discard_calibration();
    s_touch_reset_consumed_click = true;
    open_touch_reset_modal();

    if (s_wifi_status_label) {
        lv_label_set_text(s_wifi_status_label, "Touch Cal reset to default");
    }
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
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(screen, LV_SCROLLBAR_MODE_OFF);

    if (s_ota_overlay && lv_obj_is_valid(s_ota_overlay)) {
        lv_obj_del(s_ota_overlay);
        s_ota_overlay = NULL;
    }

    s_ota_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ota_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ota_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_ota_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ota_overlay, 0, 0);
    lv_obj_set_style_radius(s_ota_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_ota_overlay, 0, 0);
    lv_obj_clear_flag(s_ota_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ota_overlay, LV_OBJ_FLAG_HIDDEN);

    const ui_theme_colors_t *overlay_theme = ui_theme_get();
    uint32_t accent = overlay_theme ? overlay_theme->accent : 0x00FE8F;

    lv_obj_t *overlay_card = lv_obj_create(s_ota_overlay);
    lv_obj_set_size(overlay_card, 360, 240);
    lv_obj_center(overlay_card);
    lv_obj_set_style_bg_color(overlay_card, lv_color_hex(0x10141E), 0);
    lv_obj_set_style_bg_opa(overlay_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(overlay_card, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(overlay_card, 2, 0);
    lv_obj_set_style_radius(overlay_card, 18, 0);
    lv_obj_set_style_pad_all(overlay_card, 16, 0);
    lv_obj_clear_flag(overlay_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *overlay_title = lv_label_create(overlay_card);
    lv_label_set_text(overlay_title, "Installing Update");
    lv_obj_set_style_text_color(overlay_title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(overlay_title, &lv_font_montserrat_22, 0);
    lv_obj_align(overlay_title, LV_ALIGN_TOP_MID, 0, 0);

    s_ota_overlay_arc = lv_arc_create(overlay_card);
    lv_obj_set_size(s_ota_overlay_arc, 126, 126);
    lv_obj_align(s_ota_overlay_arc, LV_ALIGN_TOP_MID, 0, 36);
    lv_arc_set_rotation(s_ota_overlay_arc, 135);
    lv_arc_set_bg_angles(s_ota_overlay_arc, 0, 270);
    lv_arc_set_range(s_ota_overlay_arc, 0, 100);
    lv_arc_set_value(s_ota_overlay_arc, 0);
    lv_obj_set_style_arc_width(s_ota_overlay_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ota_overlay_arc, lv_color_hex(0x252B3B), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ota_overlay_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_ota_overlay_arc, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_remove_style(s_ota_overlay_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_ota_overlay_arc, LV_OBJ_FLAG_CLICKABLE);

    s_ota_overlay_percent = lv_label_create(overlay_card);
    lv_label_set_text(s_ota_overlay_percent, "0%");
    lv_obj_set_style_text_color(s_ota_overlay_percent, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_ota_overlay_percent, &lv_font_montserrat_16, 0);
    lv_obj_align_to(s_ota_overlay_percent, s_ota_overlay_arc, LV_ALIGN_CENTER, 0, 0);

    s_ota_overlay_status = lv_label_create(overlay_card);
    lv_label_set_text(s_ota_overlay_status, "Status: Starting");
    lv_obj_set_style_text_color(s_ota_overlay_status, lv_color_hex(0xC9D1DD), 0);
    lv_obj_set_style_text_align(s_ota_overlay_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_ota_overlay_status, 320);
    lv_obj_align(s_ota_overlay_status, LV_ALIGN_BOTTOM_MID, 0, -40);

    s_ota_overlay_back_btn = lv_btn_create(overlay_card);
    lv_obj_set_size(s_ota_overlay_back_btn, 110, 34);
    lv_obj_align(s_ota_overlay_back_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_button(s_ota_overlay_back_btn);
    lv_obj_add_event_cb(s_ota_overlay_back_btn, ota_overlay_back_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *overlay_back_label = lv_label_create(s_ota_overlay_back_btn);
    lv_label_set_text(overlay_back_label, "Back");
    style_button_label(overlay_back_label);
    lv_obj_center(overlay_back_label);
    lv_obj_add_flag(s_ota_overlay_back_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *display_card = lv_obj_create(screen);
    lv_obj_set_size(display_card, 376, 220);
    lv_obj_align(display_card, LV_ALIGN_TOP_LEFT, 16, 12);
    lv_obj_set_style_bg_color(display_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(display_card, 0, 0);
    lv_obj_set_style_radius(display_card, 12, 0);
    lv_obj_set_style_pad_all(display_card, 12, 0);

    lv_obj_t *display_header = lv_label_create(display_card);
    lv_label_set_text(display_header, "Display");
    lv_obj_set_style_text_color(display_header, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(display_header, &lv_font_montserrat_16, 0);
    lv_obj_align(display_header, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *brightness_label = lv_label_create(display_card);
    lv_label_set_text(brightness_label, "Brightness");
    lv_obj_set_style_text_color(brightness_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(brightness_label, LV_ALIGN_TOP_LEFT, 0, 24);

    s_brightness_slider = lv_slider_create(display_card);
    lv_obj_set_width(s_brightness_slider, 336);
    lv_obj_align(s_brightness_slider, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_slider_set_range(s_brightness_slider, 0, 100);
    lv_slider_set_value(s_brightness_slider, 60, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_brightness_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_brightness_slider, brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_refresh_value = lv_label_create(display_card);
    lv_label_set_text(s_refresh_value, "Refresh interval: 20s");
    lv_obj_set_style_text_color(s_refresh_value, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_refresh_value, LV_ALIGN_TOP_LEFT, 0, 90);

    s_refresh_slider = lv_slider_create(display_card);
    lv_obj_set_width(s_refresh_slider, 336);
    lv_obj_align(s_refresh_slider, LV_ALIGN_TOP_LEFT, 0, 116);
    lv_slider_set_range(s_refresh_slider, 5, 120);
    lv_slider_set_value(s_refresh_slider, 20, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_BTN_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_refresh_slider, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB);
    lv_obj_set_style_border_width(s_refresh_slider, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_refresh_slider, refresh_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *source_label = lv_label_create(display_card);
    lv_label_set_text(source_label, "Data source");
    lv_obj_set_style_text_color(source_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(source_label, LV_ALIGN_TOP_LEFT, 0, 146);

    s_source_coin_btn = lv_btn_create(display_card);
    lv_obj_set_size(s_source_coin_btn, 166, 26);
    lv_obj_align(s_source_coin_btn, LV_ALIGN_TOP_LEFT, 0, 170);
    style_button(s_source_coin_btn);
    lv_obj_add_event_cb(s_source_coin_btn, data_source_event, LV_EVENT_CLICKED, NULL);
    s_source_coin_label = lv_label_create(s_source_coin_btn);
    lv_label_set_text(s_source_coin_label, "CoinGecko");
    style_button_label(s_source_coin_label);
    lv_obj_center(s_source_coin_label);

    s_source_kraken_btn = lv_btn_create(display_card);
    lv_obj_set_size(s_source_kraken_btn, 166, 26);
    lv_obj_align(s_source_kraken_btn, LV_ALIGN_TOP_LEFT, 186, 170);
    style_button(s_source_kraken_btn);
    lv_obj_add_event_cb(s_source_kraken_btn, data_source_event, LV_EVENT_CLICKED, NULL);
    s_source_kraken_label = lv_label_create(s_source_kraken_btn);
    lv_label_set_text(s_source_kraken_label, "Kraken");
    style_button_label(s_source_kraken_label);
    lv_obj_center(s_source_kraken_label);

    lv_obj_t *system_card = lv_obj_create(screen);
    lv_obj_set_size(system_card, 376, 156);
    lv_obj_align(system_card, LV_ALIGN_TOP_LEFT, 16, 244);
    lv_obj_set_style_bg_color(system_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(system_card, 0, 0);
    lv_obj_set_style_radius(system_card, 12, 0);
    lv_obj_set_style_pad_all(system_card, 12, 0);
    lv_obj_clear_flag(system_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(system_card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(system_card, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *system_header = lv_label_create(system_card);
    lv_label_set_text(system_header, "System");
    lv_obj_set_style_text_color(system_header, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(system_header, &lv_font_montserrat_16, 0);
    lv_obj_align(system_header, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *actions_row = lv_obj_create(system_card);
    lv_obj_set_size(actions_row, 352, 44);
    lv_obj_align(actions_row, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_set_style_bg_opa(actions_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions_row, 0, 0);
    lv_obj_set_flex_flow(actions_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(actions_row, 0, 0);
    lv_obj_set_style_pad_column(actions_row, 12, 0);
    lv_obj_clear_flag(actions_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(actions_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(actions_row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    lv_obj_t *touch_btn = lv_btn_create(actions_row);
    lv_obj_set_size(touch_btn, 170, 36);
    lv_obj_set_style_radius(touch_btn, 18, 0);
    lv_obj_set_style_pad_left(touch_btn, 8, 0);
    lv_obj_set_style_pad_right(touch_btn, 8, 0);
    style_button(touch_btn);
    lv_obj_clear_flag(touch_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(touch_btn, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(touch_btn, touch_calibrate_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(touch_btn, touch_calibrate_reset_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *touch_label = lv_label_create(touch_btn);
    lv_label_set_text(touch_label, "Touch Cal");
    style_button_label(touch_label);
    lv_obj_center(touch_label);

    lv_obj_t *accent_btn = lv_btn_create(actions_row);
    lv_obj_set_size(accent_btn, 170, 36);
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
    lv_label_set_text(accent_label, "Theme");
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
    lv_obj_set_style_text_color(button3d_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(button3d_label, LV_ALIGN_TOP_LEFT, 0, 76);

    s_button3d_toggle = lv_switch_create(system_card);
    lv_obj_align(s_button3d_toggle, LV_ALIGN_TOP_RIGHT, 0, 70);
    style_settings_switch(s_button3d_toggle);
    lv_obj_set_style_bg_color(s_button3d_toggle, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_button3d_toggle, button3d_toggle_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dark_label = lv_label_create(system_card);
    lv_label_set_text(dark_label, "Dark Mode");
    lv_obj_set_style_text_color(dark_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(dark_label, LV_ALIGN_TOP_LEFT, 0, 108);

    s_dark_toggle = lv_switch_create(system_card);
    lv_obj_align(s_dark_toggle, LV_ALIGN_TOP_RIGHT, 0, 102);
    style_settings_switch(s_dark_toggle);
    lv_obj_set_style_bg_color(s_dark_toggle, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dark_toggle, dark_mode_toggle_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wifi_card = lv_obj_create(screen);
    lv_obj_set_size(wifi_card, 376, 160);
    lv_obj_align(wifi_card, LV_ALIGN_TOP_LEFT, 408, 12);
    lv_obj_set_style_bg_color(wifi_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(wifi_card, 0, 0);
    lv_obj_set_style_radius(wifi_card, 12, 0);
    lv_obj_set_style_pad_all(wifi_card, 12, 0);

    lv_obj_t *wifi_header = lv_label_create(wifi_card);
    lv_label_set_text(wifi_header, "Connectivity");
    lv_obj_set_style_text_color(wifi_header, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(wifi_header, &lv_font_montserrat_16, 0);
    lv_obj_align(wifi_header, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *wifi_btn = lv_btn_create(wifi_card);
    lv_obj_set_size(wifi_btn, 180, 36);
    lv_obj_align(wifi_btn, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_add_event_cb(wifi_btn, scan_wifi_event, LV_EVENT_CLICKED, NULL);
    style_button(wifi_btn);
    lv_obj_t *wifi_btn_label = lv_label_create(wifi_btn);
    lv_label_set_text(wifi_btn_label, "Wi-Fi Settings");
    style_button_label(wifi_btn_label);
    lv_obj_center(wifi_btn_label);

    s_wifi_status_label = lv_label_create(wifi_card);
    lv_label_set_text(s_wifi_status_label, "Network Status: ...");
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_wifi_status_label, LV_ALIGN_TOP_LEFT, 0, 70);

    s_wifi_ssid_label = lv_label_create(wifi_card);
    lv_label_set_text(s_wifi_ssid_label, "SSID: --");
    lv_obj_set_style_text_color(s_wifi_ssid_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_wifi_ssid_label, LV_ALIGN_TOP_LEFT, 0, 94);

    s_wifi_ip_label = lv_label_create(wifi_card);
    lv_label_set_text(s_wifi_ip_label, "IP: --");
    lv_obj_set_style_text_color(s_wifi_ip_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_wifi_ip_label, LV_ALIGN_TOP_LEFT, 0, 118);

    lv_obj_t *ota_card = lv_obj_create(screen);
    lv_obj_set_size(ota_card, 376, 216);
    lv_obj_align(ota_card, LV_ALIGN_TOP_LEFT, 408, 184);
    lv_obj_set_style_bg_color(ota_card, lv_color_hex(0x1A1D26), 0);
    lv_obj_set_style_border_width(ota_card, 0, 0);
    lv_obj_set_style_radius(ota_card, 12, 0);
    lv_obj_set_style_pad_all(ota_card, 12, 0);
    lv_obj_clear_flag(ota_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(ota_card, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *ota_header = lv_label_create(ota_card);
    lv_label_set_text(ota_header, "Firmware Update");
    lv_obj_set_style_text_color(ota_header, lv_color_hex(SETTINGS_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(ota_header, &lv_font_montserrat_16, 0);
    lv_obj_align(ota_header, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *demo_label = lv_label_create(ota_card);
    lv_label_set_text(demo_label, "Demo Portfolio");
    lv_obj_set_style_text_color(demo_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(demo_label, LV_ALIGN_TOP_RIGHT, -60, 2);

    s_demo_portfolio_toggle = lv_switch_create(ota_card);
    lv_obj_align(s_demo_portfolio_toggle, LV_ALIGN_TOP_RIGHT, 0, 0);
    style_settings_switch(s_demo_portfolio_toggle);
    lv_obj_set_style_bg_color(s_demo_portfolio_toggle, lv_color_hex(SETTINGS_TEXT_COLOR), LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_demo_portfolio_toggle, demo_portfolio_toggle_event, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *version_label = lv_label_create(ota_card);
    char version_text[32];
    snprintf(version_text, sizeof(version_text), "Current: %s", APP_VERSION);
    lv_label_set_text(version_label, version_text);
    lv_obj_set_style_text_color(version_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 0, 24);

    lv_obj_t *button_row = lv_obj_create(ota_card);
    lv_obj_set_size(button_row, 352, 30);
    lv_obj_align(button_row, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_bg_opa(button_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button_row, 0, 0);
    lv_obj_set_style_pad_all(button_row, 0, 0);
    lv_obj_set_style_pad_column(button_row, 12, 0);
    lv_obj_set_flex_flow(button_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(button_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(button_row, LV_SCROLLBAR_MODE_OFF);

    s_update_check_btn = lv_btn_create(button_row);
    lv_obj_set_size(s_update_check_btn, 170, 28);
    style_button(s_update_check_btn);
    lv_obj_add_event_cb(s_update_check_btn, github_check_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *check_label = lv_label_create(s_update_check_btn);
    lv_label_set_text(check_label, "Check for update");
    style_button_label(check_label);
    lv_obj_center(check_label);

    s_update_install_btn = lv_btn_create(button_row);
    lv_obj_set_size(s_update_install_btn, 170, 28);
    style_button(s_update_install_btn);
    lv_obj_add_event_cb(s_update_install_btn, github_install_event, LV_EVENT_CLICKED, NULL);
    s_update_install_label = lv_label_create(s_update_install_btn);
    lv_label_set_text(s_update_install_label, "Install");
    style_button_label(s_update_install_label);
    lv_obj_center(s_update_install_label);
    lv_obj_add_flag(s_update_install_btn, LV_OBJ_FLAG_HIDDEN);

    s_update_status_label = lv_label_create(ota_card);
    lv_label_set_text(s_update_status_label, "Status: --");
    lv_obj_set_style_text_color(s_update_status_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_update_status_label, LV_ALIGN_TOP_LEFT, 0, 88);

    s_update_last_label = lv_label_create(ota_card);
    lv_label_set_text(s_update_last_label, "Last checked: --");
    lv_obj_set_style_text_color(s_update_last_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_align(s_update_last_label, LV_ALIGN_TOP_LEFT, 0, 108);

    lv_obj_t *notes_box = lv_obj_create(ota_card);
    lv_obj_set_size(notes_box, 352, 66);
    lv_obj_align(notes_box, LV_ALIGN_TOP_LEFT, 0, 126);
    lv_obj_set_style_bg_color(notes_box, lv_color_hex(0x0F1117), 0);
    lv_obj_set_style_border_width(notes_box, 0, 0);
    lv_obj_set_style_radius(notes_box, 6, 0);
    lv_obj_set_style_pad_all(notes_box, 6, 0);
    lv_obj_set_scroll_dir(notes_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(notes_box, LV_SCROLLBAR_MODE_AUTO);

    s_update_notes_label = lv_label_create(notes_box);
    lv_label_set_text(s_update_notes_label, "Release notes: --");
    lv_label_set_long_mode(s_update_notes_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_update_notes_label, 336);
    lv_obj_set_style_text_color(s_update_notes_label, lv_color_hex(SETTINGS_MUTED_COLOR), 0);

    lv_obj_t *footer_row = lv_obj_create(screen);
    lv_obj_set_size(footer_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(footer_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer_row, 0, 0);
    lv_obj_set_flex_flow(footer_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(footer_row, 4, 0);
    lv_obj_align(footer_row, LV_ALIGN_BOTTOM_RIGHT, 0, 18);

    lv_obj_t *footer_text = lv_label_create(footer_row);
    lv_label_set_text(footer_text, "CrowPanel Advance 7 | PCLK 16 MHz |");
    lv_obj_set_style_text_color(footer_text, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_set_style_text_font(footer_text, &lv_font_montserrat_16, 0);

    lv_obj_t *footer_branch = lv_label_create(footer_row);
    lv_label_set_text(footer_branch, CT_BUILD_BRANCH_STR);
    lv_obj_set_style_text_color(footer_branch, lv_color_hex(SETTINGS_MUTED_COLOR), 0);
    lv_obj_set_style_text_font(footer_branch, &lv_font_montserrat_12, 0);

    update_wifi_status_label();
    if (!s_wifi_status_timer) {
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 1000, NULL);
    }

    update_ota_status();
    if (!s_ota_status_timer) {
        s_ota_status_timer = lv_timer_create(ota_status_timer_cb, 1000, NULL);
    }

    update_update_status();
    if (!s_update_status_timer) {
        s_update_status_timer = lv_timer_create(update_status_timer_cb, 1000, NULL);
    }

    apply_prefs_to_controls();

    if (s_reopen_modal_after_theme_apply) {
        s_reopen_modal_after_theme_apply = false;
        accent_modal_open_event(NULL);
    }

    ui_nav_attach(screen, UI_NAV_SETTINGS);
    return screen;
}
