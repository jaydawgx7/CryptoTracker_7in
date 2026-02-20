#include "ui/ui_touch_cal.h"

#include <stdio.h>
#include <string.h>

#include "services/display_driver.h"
#include "services/touch_driver.h"
#include "ui/ui.h"

#define CAL_TARGET_COUNT 5
#define CAL_MARGIN_X 70
#define CAL_MARGIN_BOTTOM 70
#define CAL_HEADER_Y 8
#define CAL_HEADER_H 86

typedef enum {
    CAL_STAGE_CAPTURE = 0,
    CAL_STAGE_TEST,
    CAL_STAGE_REVIEW
} cal_stage_t;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_target_ring = NULL;
static lv_obj_t *s_target_cross_h = NULL;
static lv_obj_t *s_target_cross_v = NULL;
static lv_obj_t *s_test_ring = NULL;
static lv_obj_t *s_test_cross_h = NULL;
static lv_obj_t *s_test_cross_v = NULL;
static lv_obj_t *s_hint = NULL;
static lv_obj_t *s_progress = NULL;
static lv_obj_t *s_status = NULL;
static lv_obj_t *s_capture_surface = NULL;
static lv_obj_t *s_action_row = NULL;
static lv_timer_t *s_test_timer = NULL;

static uint16_t s_target_x[CAL_TARGET_COUNT] = {0};
static uint16_t s_target_y[CAL_TARGET_COUNT] = {0};
static uint16_t s_raw_x[CAL_TARGET_COUNT] = {0};
static uint16_t s_raw_y[CAL_TARGET_COUNT] = {0};
static int s_step = 0;
static cal_stage_t s_stage = CAL_STAGE_CAPTURE;
static touch_calibration_t s_session_start_calibration = {0};
static bool s_session_start_valid = false;
static bool s_pending_runtime_calibration = false;

static void update_target_visual(void);

static void initial_target_refresh_cb(void *arg)
{
    (void)arg;
    update_target_visual();
    if (s_target_ring) {
        lv_obj_invalidate(s_target_ring);
    }
    if (s_target_cross_h) {
        lv_obj_invalidate(s_target_cross_h);
    }
    if (s_target_cross_v) {
        lv_obj_invalidate(s_target_cross_v);
    }
}

static void enter_capture_stage(void);
static void enter_test_stage(void);
static void enter_review_stage(void);

static void set_crosshair_pos(lv_obj_t *ring, lv_obj_t *cross_h, lv_obj_t *cross_v,
                              lv_coord_t x, lv_coord_t y)
{
    if (!ring || !cross_h || !cross_v) {
        return;
    }

    lv_coord_t ring_w = lv_obj_get_width(ring);
    lv_coord_t ring_h = lv_obj_get_height(ring);
    lv_coord_t cross_h_w = lv_obj_get_width(cross_h);
    lv_coord_t cross_h_h = lv_obj_get_height(cross_h);
    lv_coord_t cross_v_w = lv_obj_get_width(cross_v);
    lv_coord_t cross_v_h = lv_obj_get_height(cross_v);

    lv_obj_set_pos(ring, x - (ring_w / 2), y - (ring_h / 2));
    lv_obj_set_pos(cross_h, x - (cross_h_w / 2), y - (cross_h_h / 2));
    lv_obj_set_pos(cross_v, x - (cross_v_w / 2), y - (cross_v_h / 2));
}

static void set_crosshair_visible(lv_obj_t *ring, lv_obj_t *cross_h, lv_obj_t *cross_v, bool visible)
{
    if (!ring || !cross_h || !cross_v) {
        return;
    }

    if (visible) {
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cross_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(cross_v, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cross_h, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(cross_v, LV_OBJ_FLAG_HIDDEN);
    }
}

static void compute_target_positions(void)
{
    int32_t left = CAL_MARGIN_X;
    int32_t right = CT_TOUCH_TARGET_H_RES - CAL_MARGIN_X;

    int32_t top = CAL_HEADER_Y + CAL_HEADER_H + 34;
    int32_t bottom = CT_TOUCH_TARGET_V_RES - CAL_MARGIN_BOTTOM;

    if (bottom <= top + 80) {
        top = CAL_HEADER_Y + CAL_HEADER_H + 16;
        bottom = CT_TOUCH_TARGET_V_RES - 48;
    }

    s_target_x[0] = (uint16_t)left;
    s_target_y[0] = (uint16_t)top;
    s_target_x[1] = (uint16_t)right;
    s_target_y[1] = (uint16_t)top;
    s_target_x[2] = (uint16_t)right;
    s_target_y[2] = (uint16_t)bottom;
    s_target_x[3] = (uint16_t)left;
    s_target_y[3] = (uint16_t)bottom;
    s_target_x[4] = (uint16_t)(CT_TOUCH_TARGET_H_RES / 2);
    s_target_y[4] = (uint16_t)(CT_TOUCH_TARGET_V_RES / 2);
}

static void finish_timer_cb(lv_timer_t *timer)
{
    if (timer) {
        lv_timer_del(timer);
    }
    ui_show_settings();
}

static void update_target_visual(void)
{
    if (!s_target_ring || !s_target_cross_h || !s_target_cross_v) {
        return;
    }

    if (s_stage != CAL_STAGE_CAPTURE || s_step < 0 || s_step >= CAL_TARGET_COUNT) {
        set_crosshair_visible(s_target_ring, s_target_cross_h, s_target_cross_v, false);
        return;
    }

    set_crosshair_visible(s_target_ring, s_target_cross_h, s_target_cross_v, true);

    lv_coord_t x = (lv_coord_t)s_target_x[s_step];
    lv_coord_t y = (lv_coord_t)s_target_y[s_step];

    set_crosshair_pos(s_target_ring, s_target_cross_h, s_target_cross_v, x, y);

    if (s_hint) {
        char text[64];
        snprintf(text, sizeof(text), "Tap target %d of %d", s_step + 1, CAL_TARGET_COUNT);
        lv_label_set_text(s_hint, text);
    }
    if (s_progress) {
        char text[64];
        snprintf(text, sizeof(text), "%d/%d", s_step + 1, CAL_TARGET_COUNT);
        lv_label_set_text(s_progress, text);
    }
}

static bool compute_calibration(touch_calibration_t *out)
{
    if (!out) {
        return false;
    }

    int32_t left = ((int32_t)s_raw_x[0] + (int32_t)s_raw_x[3]) / 2;
    int32_t right = ((int32_t)s_raw_x[1] + (int32_t)s_raw_x[2]) / 2;
    int32_t top = ((int32_t)s_raw_y[0] + (int32_t)s_raw_y[1]) / 2;
    int32_t bottom = ((int32_t)s_raw_y[3] + (int32_t)s_raw_y[2]) / 2;

    if (right - left < 20 || bottom - top < 20) {
        return false;
    }

    const int32_t target_left_x = (int32_t)s_target_x[0];
    const int32_t target_right_x = (int32_t)s_target_x[1];
    const int32_t target_top_y = (int32_t)s_target_y[0];
    const int32_t target_bottom_y = (int32_t)s_target_y[2];

    const int32_t phys_w = CT_TOUCH_PHYS_H_RES;
    const int32_t phys_h = CT_TOUCH_PHYS_V_RES;

    const int32_t target_span_x = target_right_x - target_left_x;
    const int32_t target_span_y = target_bottom_y - target_top_y;
    if (target_span_x <= 0 || target_span_y <= 0) {
        return false;
    }

    double scale_raw_per_px_x = (double)(right - left) / (double)target_span_x;
    double scale_raw_per_px_y = (double)(bottom - top) / (double)target_span_y;

    double raw_min_x = (double)left - (scale_raw_per_px_x * (double)target_left_x);
    double raw_max_x = raw_min_x + (scale_raw_per_px_x * (double)(phys_w - 1));

    double raw_min_y = (double)top - (scale_raw_per_px_y * (double)target_top_y);
    double raw_max_y = raw_min_y + (scale_raw_per_px_y * (double)(phys_h - 1));

    out->cal_x_min = (int32_t)(raw_min_x + (raw_min_x >= 0.0 ? 0.5 : -0.5));
    out->cal_x_max = (int32_t)(raw_max_x + (raw_max_x >= 0.0 ? 0.5 : -0.5));
    out->cal_y_min = (int32_t)(raw_min_y + (raw_min_y >= 0.0 ? 0.5 : -0.5));
    out->cal_y_max = (int32_t)(raw_max_y + (raw_max_y >= 0.0 ? 0.5 : -0.5));

    if (out->cal_x_max <= out->cal_x_min || out->cal_y_max <= out->cal_y_min) {
        return false;
    }

    int32_t center_raw_x = s_raw_x[4];
    int32_t center_raw_y = s_raw_y[4];

    int32_t mapped_center_x = (center_raw_x - out->cal_x_min) * (phys_w - 1) /
                              (out->cal_x_max - out->cal_x_min);
    int32_t mapped_center_y = (center_raw_y - out->cal_y_min) * (phys_h - 1) /
                              (out->cal_y_max - out->cal_y_min);

    int32_t expected_center_x = CT_TOUCH_TARGET_H_RES / 2;
    int32_t expected_center_y = CT_TOUCH_TARGET_V_RES / 2;

    out->offset_x = expected_center_x - mapped_center_x;
    out->offset_y = expected_center_y - mapped_center_y;

    if (out->offset_x < -80) out->offset_x = -80;
    if (out->offset_x > 80) out->offset_x = 80;
    if (out->offset_y < -80) out->offset_y = -80;
    if (out->offset_y > 80) out->offset_y = 80;

    return true;
}

static void test_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s_stage != CAL_STAGE_TEST) {
        set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, false);
        return;
    }

    bool pressed = false;
    int16_t x = 0;
    int16_t y = 0;
    touch_driver_get_state(&pressed, &x, &y);

    if (!pressed) {
        set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, false);
        return;
    }

    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }
    if (x >= CT_TOUCH_TARGET_H_RES) {
        x = CT_TOUCH_TARGET_H_RES - 1;
    }
    if (y >= CT_TOUCH_TARGET_V_RES) {
        y = CT_TOUCH_TARGET_V_RES - 1;
    }

    set_crosshair_pos(s_test_ring, s_test_cross_h, s_test_cross_v, (lv_coord_t)x, (lv_coord_t)y);
    set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, true);
}

static void ensure_test_timer(void)
{
    if (s_test_timer) {
        return;
    }
    s_test_timer = lv_timer_create(test_timer_cb, 30, NULL);
}

static void enter_capture_stage(void)
{
    s_stage = CAL_STAGE_CAPTURE;
    s_step = 0;

    touch_driver_reset_calibration_defaults();

    memset(s_raw_x, 0, sizeof(s_raw_x));
    memset(s_raw_y, 0, sizeof(s_raw_y));

    if (s_title) {
        lv_label_set_text(s_title, "Touch Calibration");
    }
    if (s_hint) {
        lv_label_set_text(s_hint, "Tap target 1 of 5");
    }
    if (s_progress) {
        lv_label_set_text(s_progress, "1/5");
    }
    if (s_status) {
        lv_label_set_text(s_status, "Touch each crosshair precisely");
    }

    if (s_action_row) {
        lv_obj_add_flag(s_action_row, LV_OBJ_FLAG_HIDDEN);
    }

    set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, false);
    update_target_visual();
}

static void enter_test_stage(void)
{
    s_stage = CAL_STAGE_TEST;

    if (s_title) {
        lv_label_set_text(s_title, "Touch Input Test");
    }
    if (s_hint) {
        lv_label_set_text(s_hint, "Touch anywhere to test input. Longpress to exit.");
    }
    if (s_progress) {
        lv_label_set_text(s_progress, "");
    }
    if (s_status) {
        lv_label_set_text(s_status, "Use the crosshair to verify touch tracking");
    }

    if (s_action_row) {
        lv_obj_add_flag(s_action_row, LV_OBJ_FLAG_HIDDEN);
    }

    set_crosshair_visible(s_target_ring, s_target_cross_h, s_target_cross_v, false);
    ensure_test_timer();
}

static void enter_review_stage(void)
{
    s_stage = CAL_STAGE_REVIEW;

    if (s_title) {
        lv_label_set_text(s_title, "Touch Calibration");
    }
    if (s_hint) {
        lv_label_set_text(s_hint, "Calibration test complete");
    }
    if (s_progress) {
        lv_label_set_text(s_progress, "Done");
    }
    if (s_status) {
        lv_label_set_text(s_status, "Choose Recalibrate or Confirm");
    }

    if (s_action_row) {
        lv_obj_clear_flag(s_action_row, LV_OBJ_FLAG_HIDDEN);
    }

    set_crosshair_visible(s_target_ring, s_target_cross_h, s_target_cross_v, false);
    set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, false);
}

static void complete_calibration(void)
{
    touch_calibration_t calibration = {0};
    if (!compute_calibration(&calibration)) {
        if (s_status) {
            lv_label_set_text(s_status, "Calibration failed. Try again.");
        }
        s_step = 0;
        update_target_visual();
        return;
    }

    if (touch_driver_set_calibration(&calibration) != ESP_OK) {
        if (s_status) {
            lv_label_set_text(s_status, "Apply failed. Try again.");
        }
        s_step = 0;
        update_target_visual();
        return;
    }

    s_pending_runtime_calibration = true;
    enter_test_stage();
}

static void capture_event(lv_event_t *e)
{
    (void)e;

    if (s_stage != CAL_STAGE_CAPTURE || s_step < 0 || s_step >= CAL_TARGET_COUNT) {
        return;
    }

    bool pressed = false;
    int16_t raw_x = 0;
    int16_t raw_y = 0;
    touch_driver_get_raw_state(&pressed, &raw_x, &raw_y);

    if (raw_x <= 0 && raw_y <= 0) {
        return;
    }

    int16_t mapped_x = 0;
    int16_t mapped_y = 0;
    touch_driver_get_state(NULL, &mapped_x, &mapped_y);

    int32_t dx = (int32_t)mapped_x - (int32_t)s_target_x[s_step];
    int32_t dy = (int32_t)mapped_y - (int32_t)s_target_y[s_step];
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }

    if (dx > 70 || dy > 70) {
        if (s_status) {
            lv_label_set_text(s_status, "Tap directly on the crosshair");
        }
        return;
    }

    s_raw_x[s_step] = (uint16_t)raw_x;
    s_raw_y[s_step] = (uint16_t)raw_y;

    s_step++;
    if (s_step >= CAL_TARGET_COUNT) {
        complete_calibration();
        return;
    }

    if (s_status) {
        lv_label_set_text(s_status, "Captured");
    }
    update_target_visual();
}

static void test_long_press_event(lv_event_t *e)
{
    (void)e;

    if (s_stage != CAL_STAGE_TEST) {
        return;
    }

    enter_review_stage();
}

static void recalibrate_event(lv_event_t *e)
{
    (void)e;
    s_pending_runtime_calibration = false;
    enter_capture_stage();
}

static void confirm_event(lv_event_t *e)
{
    (void)e;

    if (touch_driver_promote_current_to_default() != ESP_OK) {
        if (s_status) {
            lv_label_set_text(s_status, "Save failed. Try again.");
        }
        return;
    }

    if (touch_driver_save_calibration() != ESP_OK) {
        if (s_status) {
            lv_label_set_text(s_status, "Save failed. Try again.");
        }
        return;
    }

    touch_driver_get_calibration(&s_session_start_calibration);
    s_session_start_valid = true;
    s_pending_runtime_calibration = false;

    if (s_status) {
        lv_label_set_text(s_status, "Calibration saved.");
    }

    lv_timer_t *timer = lv_timer_create_basic();
    lv_timer_set_period(timer, 350);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_cb(timer, finish_timer_cb);
}

static void cancel_event(lv_event_t *e)
{
    (void)e;

    if (s_pending_runtime_calibration && s_session_start_valid) {
        touch_driver_set_calibration(&s_session_start_calibration);
        s_pending_runtime_calibration = false;
    }

    ui_show_settings();
}

lv_obj_t *ui_touch_cal_screen_create(void)
{
    touch_driver_get_calibration(&s_session_start_calibration);
    s_session_start_valid = true;
    s_pending_runtime_calibration = false;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x090C12), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    compute_target_positions();

    lv_obj_t *header = lv_obj_create(s_screen);
    lv_obj_set_size(header, 760, CAL_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, CAL_HEADER_Y);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x111827), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_70, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 14, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_CLICKABLE);

    s_title = lv_label_create(header);
    lv_label_set_text(s_title, "Touch Calibration");
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xE6E6E6), 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_22, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 16, 6);

    s_hint = lv_label_create(header);
    lv_label_set_text(s_hint, "Tap target 1 of 5");
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x9AA1AD), 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_LEFT, 16, 42);

    s_progress = lv_label_create(header);
    lv_label_set_text(s_progress, "1/5");
    lv_obj_set_style_text_color(s_progress, lv_color_hex(0x00FE8F), 0);
    lv_obj_align(s_progress, LV_ALIGN_TOP_RIGHT, -16, 42);

    lv_obj_t *cancel_btn = lv_btn_create(s_screen);
    lv_obj_set_size(cancel_btn, 120, 38);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_radius(cancel_btn, 19, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(cancel_btn, 0, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xE6E6E6), 0);
    lv_obj_center(cancel_lbl);

    s_status = lv_label_create(s_screen);
    lv_label_set_text(s_status, "Touch each crosshair precisely");
    lv_obj_set_style_text_color(s_status, lv_color_hex(0x6B7280), 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -66);

    s_capture_surface = lv_obj_create(s_screen);
    lv_obj_set_size(s_capture_surface, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_capture_surface, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_capture_surface, 0, 0);
    lv_obj_add_event_cb(s_capture_surface, capture_event, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_capture_surface, test_long_press_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_move_background(s_capture_surface);

    s_target_ring = lv_obj_create(s_screen);
    lv_obj_set_size(s_target_ring, 44, 44);
    lv_obj_set_style_radius(s_target_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_target_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_target_ring, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_target_ring, 3, 0);
    lv_obj_set_style_shadow_width(s_target_ring, 12, 0);
    lv_obj_set_style_shadow_color(s_target_ring, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_shadow_opa(s_target_ring, LV_OPA_40, 0);
    lv_obj_clear_flag(s_target_ring, LV_OBJ_FLAG_CLICKABLE);

    s_target_cross_h = lv_obj_create(s_screen);
    lv_obj_set_size(s_target_cross_h, 40, 2);
    lv_obj_set_style_bg_color(s_target_cross_h, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_target_cross_h, 0, 0);
    lv_obj_clear_flag(s_target_cross_h, LV_OBJ_FLAG_CLICKABLE);

    s_target_cross_v = lv_obj_create(s_screen);
    lv_obj_set_size(s_target_cross_v, 2, 40);
    lv_obj_set_style_bg_color(s_target_cross_v, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_target_cross_v, 0, 0);
    lv_obj_clear_flag(s_target_cross_v, LV_OBJ_FLAG_CLICKABLE);

    s_test_ring = lv_obj_create(s_screen);
    lv_obj_set_size(s_test_ring, 33, 33);
    lv_obj_set_style_radius(s_test_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_test_ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_test_ring, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_test_ring, 2, 0);
    lv_obj_set_style_shadow_width(s_test_ring, 0, 0);
    lv_obj_clear_flag(s_test_ring, LV_OBJ_FLAG_CLICKABLE);

    s_test_cross_h = lv_obj_create(s_screen);
    lv_obj_set_size(s_test_cross_h, 30, 2);
    lv_obj_set_style_bg_color(s_test_cross_h, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_test_cross_h, 0, 0);
    lv_obj_clear_flag(s_test_cross_h, LV_OBJ_FLAG_CLICKABLE);

    s_test_cross_v = lv_obj_create(s_screen);
    lv_obj_set_size(s_test_cross_v, 2, 30);
    lv_obj_set_style_bg_color(s_test_cross_v, lv_color_hex(0x00FE8F), 0);
    lv_obj_set_style_border_width(s_test_cross_v, 0, 0);
    lv_obj_clear_flag(s_test_cross_v, LV_OBJ_FLAG_CLICKABLE);

    s_action_row = lv_obj_create(s_screen);
    lv_obj_set_size(s_action_row, 360, 46);
    lv_obj_align(s_action_row, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(s_action_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_action_row, 0, 0);
    lv_obj_set_style_pad_all(s_action_row, 0, 0);
    lv_obj_set_style_pad_column(s_action_row, 12, 0);
    lv_obj_set_flex_flow(s_action_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_action_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_action_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_action_row, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *recal_btn = lv_btn_create(s_action_row);
    lv_obj_set_size(recal_btn, 170, 38);
    lv_obj_set_style_radius(recal_btn, 19, 0);
    lv_obj_set_style_bg_color(recal_btn, lv_color_hex(0x1F2937), 0);
    lv_obj_set_style_border_width(recal_btn, 0, 0);
    lv_obj_add_event_cb(recal_btn, recalibrate_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *recal_lbl = lv_label_create(recal_btn);
    lv_label_set_text(recal_lbl, "Recalibrate");
    lv_obj_set_style_text_color(recal_lbl, lv_color_hex(0xE6E6E6), 0);
    lv_obj_center(recal_lbl);

    lv_obj_t *confirm_btn = lv_btn_create(s_action_row);
    lv_obj_set_size(confirm_btn, 170, 38);
    lv_obj_set_style_radius(confirm_btn, 19, 0);
    lv_obj_set_style_bg_color(confirm_btn, lv_color_hex(0x2A3142), 0);
    lv_obj_set_style_border_width(confirm_btn, 0, 0);
    lv_obj_add_event_cb(confirm_btn, confirm_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_lbl = lv_label_create(confirm_btn);
    lv_label_set_text(confirm_lbl, "Confirm");
    lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0x00FE8F), 0);
    lv_obj_center(confirm_lbl);

    lv_obj_add_flag(s_action_row, LV_OBJ_FLAG_HIDDEN);

    set_crosshair_visible(s_test_ring, s_test_cross_h, s_test_cross_v, false);
    ensure_test_timer();
    lv_obj_update_layout(s_screen);
    enter_capture_stage();
    lv_async_call(initial_target_refresh_cb, NULL);

    return s_screen;
}
