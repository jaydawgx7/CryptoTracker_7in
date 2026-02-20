#include "ui/ui.h"

#include "services/display_driver.h"
#include "ui/ui_add_coin.h"
#include "ui/ui_alerts.h"
#include "ui/ui_coin_detail.h"
#include "ui/ui_dashboard.h"
#include "ui/ui_home.h"
#include "ui/ui_settings.h"
#include "ui/ui_touch_cal.h"
#include "ui/ui_theme.h"
#include "services/touch_driver.h"

#ifndef CT_UI_MINIMAL
#define CT_UI_MINIMAL 0
#endif

#ifndef CT_UI_TOUCH_DEBUG
#define CT_UI_TOUCH_DEBUG 0
#endif

#ifndef CT_UI_BORDER_DEBUG
#define CT_UI_BORDER_DEBUG 0
#endif

#define CT_UI_TOUCH_DOT_RADIUS 10

static lv_obj_t *s_home_screen = NULL;
static lv_obj_t *s_dashboard_screen = NULL;
static lv_obj_t *s_settings_screen = NULL;
static lv_obj_t *s_add_coin_screen = NULL;
static lv_obj_t *s_alerts_screen = NULL;
static lv_obj_t *s_coin_detail_screen = NULL;
static lv_obj_t *s_touch_cal_screen = NULL;
static const app_state_t *s_app_state = NULL;
static bool s_theme_rebuild_pending = false;
#if CT_UI_TOUCH_DEBUG
static lv_obj_t *s_touch_indicator = NULL;
static lv_timer_t *s_touch_timer = NULL;
#endif

static void load_screen(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }
    lv_scr_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

static void apply_state_to_screens(const app_state_t *state)
{
    if (!state) {
        return;
    }

    ui_home_set_state(state);
    ui_dashboard_set_state(state);
    ui_add_coin_set_state((app_state_t *)state);
    ui_coin_detail_set_state((app_state_t *)state);
    ui_settings_set_state((app_state_t *)state);
}

static void delete_screen_if_inactive(lv_obj_t *screen, lv_obj_t *active)
{
    if (!screen || screen == active) {
        return;
    }
    if (lv_obj_is_valid(screen)) {
        lv_obj_del(screen);
    }
}

static void rebuild_screens(void)
{
    lv_obj_t *active_old = lv_scr_act();
    lv_obj_t *old_home = s_home_screen;
    lv_obj_t *old_dashboard = s_dashboard_screen;
    lv_obj_t *old_settings = s_settings_screen;
    lv_obj_t *old_add = s_add_coin_screen;
    lv_obj_t *old_alerts = s_alerts_screen;
    lv_obj_t *old_detail = s_coin_detail_screen;
    lv_obj_t *old_touch_cal = s_touch_cal_screen;

    bool show_dashboard = (active_old == old_dashboard);
    bool show_settings = (active_old == old_settings);
    bool show_add = (active_old == old_add);
    bool show_alerts = (active_old == old_alerts);
    bool show_detail = (active_old == old_detail);
    bool show_touch_cal = (active_old == old_touch_cal);

    delete_screen_if_inactive(old_home, active_old);
    delete_screen_if_inactive(old_dashboard, active_old);
    delete_screen_if_inactive(old_settings, active_old);
    delete_screen_if_inactive(old_add, active_old);
    delete_screen_if_inactive(old_alerts, active_old);
    delete_screen_if_inactive(old_detail, active_old);
    delete_screen_if_inactive(old_touch_cal, active_old);

    s_home_screen = ui_home_screen_create();
    s_dashboard_screen = ui_dashboard_screen_create();
#if !CT_UI_MINIMAL
    s_settings_screen = ui_settings_screen_create();
    s_add_coin_screen = ui_add_coin_screen_create();
    s_alerts_screen = ui_alerts_screen_create();
    s_coin_detail_screen = ui_coin_detail_screen_create();
    s_touch_cal_screen = ui_touch_cal_screen_create();
#endif

#if CT_UI_BORDER_DEBUG
    apply_screen_border(s_home_screen);
    apply_screen_border(s_settings_screen);
    apply_screen_border(s_add_coin_screen);
    apply_screen_border(s_alerts_screen);
    apply_screen_border(s_coin_detail_screen);
#endif

    apply_state_to_screens(s_app_state);

    if (show_dashboard && s_dashboard_screen) {
        load_screen(s_dashboard_screen);
        ui_dashboard_refresh();
    } else if (show_settings && s_settings_screen) {
        load_screen(s_settings_screen);
    } else if (show_add && s_add_coin_screen) {
        load_screen(s_add_coin_screen);
    } else if (show_alerts && s_alerts_screen) {
        load_screen(s_alerts_screen);
        ui_alerts_refresh();
    } else if (show_detail && s_coin_detail_screen) {
        load_screen(s_coin_detail_screen);
    } else if (show_touch_cal && s_touch_cal_screen) {
        load_screen(s_touch_cal_screen);
    } else {
        load_screen(s_home_screen);
        ui_home_refresh();
    }

    if (active_old && lv_obj_is_valid(active_old)) {
        lv_obj_del(active_old);
    }
}

static void rebuild_if_pending(void)
{
    if (!s_theme_rebuild_pending) {
        return;
    }

    s_theme_rebuild_pending = false;
    rebuild_screens();
}

#if CT_UI_BORDER_DEBUG
static void apply_screen_border(lv_obj_t *screen)
{
    if (!screen) {
        return;
    }

    lv_obj_set_style_border_width(screen, 4, 0);
    lv_obj_set_style_border_color(screen, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(screen, LV_BORDER_SIDE_FULL, 0);
}
#endif

#if CT_UI_TOUCH_DEBUG
static lv_indev_t *find_pointer_indev(void)
{
    lv_indev_t *indev = NULL;
    while ((indev = lv_indev_get_next(indev)) != NULL) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            return indev;
        }
    }
    return NULL;
}

static void touch_debug_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_touch_indicator) {
        return;
    }

    lv_indev_t *indev = find_pointer_indev();
    if (!indev) {
        return;
    }

    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
    touch_driver_get_state(&pressed, &x, &y);

    if (pressed) {
        lv_obj_clear_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_touch_indicator, x - CT_UI_TOUCH_DOT_RADIUS, y - CT_UI_TOUCH_DOT_RADIUS);
    } else {
        lv_obj_add_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

static void touch_debug_init(void)
{
    if (s_touch_indicator || s_touch_timer) {
        return;
    }

    s_touch_indicator = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_touch_indicator, CT_UI_TOUCH_DOT_RADIUS * 2, CT_UI_TOUCH_DOT_RADIUS * 2);
    lv_obj_set_style_radius(s_touch_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_touch_indicator, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(s_touch_indicator, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_touch_indicator, 0, 0);
    lv_obj_clear_flag(s_touch_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_touch_indicator, LV_OBJ_FLAG_HIDDEN);

    s_touch_timer = lv_timer_create(touch_debug_timer_cb, 30, NULL);
}
#endif

void ui_init(void)
{
    if (!display_driver_lock(1000)) {
        return;
    }

    ui_theme_init(true);

    s_home_screen = ui_home_screen_create();
    s_dashboard_screen = ui_dashboard_screen_create();
#if !CT_UI_MINIMAL
    s_settings_screen = ui_settings_screen_create();
    s_add_coin_screen = ui_add_coin_screen_create();
    s_alerts_screen = ui_alerts_screen_create();
    s_coin_detail_screen = ui_coin_detail_screen_create();
    s_touch_cal_screen = ui_touch_cal_screen_create();
#endif

#if CT_UI_BORDER_DEBUG
    apply_screen_border(s_home_screen);
    apply_screen_border(s_settings_screen);
    apply_screen_border(s_add_coin_screen);
    apply_screen_border(s_alerts_screen);
    apply_screen_border(s_coin_detail_screen);
    apply_screen_border(s_touch_cal_screen);
#endif

    load_screen(s_dashboard_screen ? s_dashboard_screen : s_home_screen);
    ui_dashboard_refresh();

#if CT_UI_TOUCH_DEBUG
    touch_debug_init();
#endif

    display_driver_resume_refresh();

    display_driver_unlock();
}

void ui_show_home(void)
{
    rebuild_if_pending();
    load_screen(s_home_screen);
    ui_home_refresh();
}

void ui_show_dashboard(void)
{
    rebuild_if_pending();
    if (s_dashboard_screen) {
        load_screen(s_dashboard_screen);
        ui_dashboard_refresh();
    }
}

void ui_set_app_state(const app_state_t *state)
{
    if (!display_driver_lock(500)) {
        return;
    }

    s_app_state = state;

    ui_theme_set_accent(state->prefs.accent_hex);
    ui_theme_set_shadow_color(state->prefs.shadow_hex);
    ui_theme_set_shadow_strength((uint8_t)state->prefs.button_shadow_strength);
    ui_theme_set_dark_mode(state->prefs.dark_mode);
    ui_theme_init(state->prefs.dark_mode);

    apply_state_to_screens(s_app_state);

    if (lv_scr_act() == s_dashboard_screen) {
        ui_dashboard_refresh();
    } else if (lv_scr_act() == s_home_screen) {
        ui_home_refresh();
    } else if (lv_scr_act() == s_alerts_screen) {
        ui_alerts_refresh();
    }

    display_driver_unlock();
}

void ui_apply_theme(bool dark_mode)
{
    bool locked = display_driver_lock(0);

    ui_theme_set_dark_mode(dark_mode);
    if (s_app_state) {
        ui_theme_set_accent(s_app_state->prefs.accent_hex);
        ui_theme_set_shadow_color(s_app_state->prefs.shadow_hex);
        ui_theme_set_shadow_strength((uint8_t)s_app_state->prefs.button_shadow_strength);
    }
    ui_theme_init(dark_mode);

    if (lv_scr_act() == s_settings_screen) {
        rebuild_screens();
        s_theme_rebuild_pending = false;
    } else {
        s_theme_rebuild_pending = true;
    }

    if (locked) {
        display_driver_unlock();
    }
}

void ui_show_add_coin(void)
{
    rebuild_if_pending();
    load_screen(s_add_coin_screen);
}

void ui_show_alerts(void)
{
    rebuild_if_pending();
    load_screen(s_alerts_screen);
    ui_alerts_refresh();
}

void ui_show_coin_detail(size_t index)
{
    rebuild_if_pending();
    if (s_coin_detail_screen) {
        ui_coin_detail_show_index(index);
        load_screen(s_coin_detail_screen);
    }
}

void ui_show_settings(void)
{
    rebuild_if_pending();
    load_screen(s_settings_screen);
}

void ui_show_touch_calibration(void)
{
    rebuild_if_pending();
    if (!s_touch_cal_screen) {
        s_touch_cal_screen = ui_touch_cal_screen_create();
    }
    load_screen(s_touch_cal_screen);
}
