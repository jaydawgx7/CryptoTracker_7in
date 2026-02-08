#include "ui/ui.h"

#include "services/display_driver.h"
#include "ui/ui_add_coin.h"
#include "ui/ui_alerts.h"
#include "ui/ui_coin_detail.h"
#include "ui/ui_home.h"
#include "ui/ui_settings.h"
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
static lv_obj_t *s_settings_screen = NULL;
static lv_obj_t *s_add_coin_screen = NULL;
static lv_obj_t *s_alerts_screen = NULL;
static lv_obj_t *s_coin_detail_screen = NULL;
static lv_obj_t *s_touch_indicator = NULL;
static lv_timer_t *s_touch_timer = NULL;

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
#if !CT_UI_MINIMAL
    s_settings_screen = ui_settings_screen_create();
    s_add_coin_screen = ui_add_coin_screen_create();
    s_alerts_screen = ui_alerts_screen_create();
    s_coin_detail_screen = ui_coin_detail_screen_create();
#endif

#if CT_UI_BORDER_DEBUG
    apply_screen_border(s_home_screen);
    apply_screen_border(s_settings_screen);
    apply_screen_border(s_add_coin_screen);
    apply_screen_border(s_alerts_screen);
    apply_screen_border(s_coin_detail_screen);
#endif

    if (s_home_screen) {
        lv_scr_load(s_home_screen);
    }

#if CT_UI_TOUCH_DEBUG
    touch_debug_init();
#endif

    display_driver_resume_refresh();

    display_driver_unlock();
}

void ui_show_home(void)
{
    if (s_home_screen) {
        lv_scr_load(s_home_screen);
    }
}

void ui_set_app_state(const app_state_t *state)
{
    if (!display_driver_lock(500)) {
        return;
    }

    ui_home_set_state(state);
    ui_add_coin_set_state((app_state_t *)state);
    ui_coin_detail_set_state((app_state_t *)state);

    display_driver_unlock();
}

void ui_show_add_coin(void)
{
    if (s_add_coin_screen) {
        lv_scr_load(s_add_coin_screen);
    }
}

void ui_show_alerts(void)
{
    if (s_alerts_screen) {
        lv_scr_load(s_alerts_screen);
    }
}

void ui_show_coin_detail(size_t index)
{
    if (s_coin_detail_screen) {
        ui_coin_detail_show_index(index);
        lv_scr_load(s_coin_detail_screen);
    }
}

void ui_show_settings(void)
{
    if (s_settings_screen) {
        lv_scr_load(s_settings_screen);
    }
}
