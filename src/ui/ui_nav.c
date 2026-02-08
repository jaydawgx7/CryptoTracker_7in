#include "ui/ui_nav.h"

#include "ui/ui.h"

static void nav_event(lv_event_t *e)
{
	ui_nav_page_t page = (ui_nav_page_t)(uintptr_t)lv_event_get_user_data(e);
	switch (page) {
		case UI_NAV_HOME:
			ui_show_home();
			break;
		case UI_NAV_ADD:
			ui_show_add_coin();
			break;
		case UI_NAV_ALERTS:
			ui_nav_set_alert_badge(false);
			ui_show_alerts();
			break;
		case UI_NAV_SETTINGS:
			ui_show_settings();
			break;
		default:
			break;
	}
}

static void anim_set_text_opa(void *obj, int32_t value)
{
	lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void anim_set_transform_zoom(void *obj, int32_t value)
{
	lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (lv_coord_t)value, 0);
}

static lv_obj_t *create_nav_button(lv_obj_t *parent, const char *label, ui_nav_page_t page, bool active)
{
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, 180, 40);
	lv_obj_set_style_radius(btn, 10, 0);
	lv_obj_set_style_bg_color(btn, active ? lv_color_hex(0x2A3142) : lv_color_hex(0x151A24), 0);
	lv_obj_add_event_cb(btn, nav_event, LV_EVENT_CLICKED, (void *)(uintptr_t)page);

	lv_obj_t *text = lv_label_create(btn);
	lv_label_set_text(text, label);
	lv_obj_set_style_text_color(text, active ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x9AA1AD), 0);
	lv_obj_center(text);
	return btn;
}

static lv_coord_t s_last_indicator_x = -1;
static bool s_alert_badge_visible = false;

void ui_nav_set_alert_badge(bool visible)
{
	s_alert_badge_visible = visible;
}

void ui_nav_attach(lv_obj_t *screen, ui_nav_page_t active_page)
{
	lv_obj_t *nav = lv_obj_create(screen);
	lv_obj_set_size(nav, 800, 56);
	lv_obj_align(nav, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(nav, lv_color_hex(0x0F131B), 0);
	lv_obj_set_style_border_width(nav, 0, 0);
	lv_obj_set_style_pad_left(nav, 10, 0);
	lv_obj_set_style_pad_right(nav, 10, 0);
	lv_obj_set_style_pad_top(nav, 8, 0);
	lv_obj_set_style_pad_bottom(nav, 8, 0);
	lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *buttons[4] = {0};
	buttons[0] = create_nav_button(nav, "Home", UI_NAV_HOME, active_page == UI_NAV_HOME);
	buttons[1] = create_nav_button(nav, "Add", UI_NAV_ADD, active_page == UI_NAV_ADD);
	buttons[2] = create_nav_button(nav, "Alerts", UI_NAV_ALERTS, active_page == UI_NAV_ALERTS);
	buttons[3] = create_nav_button(nav, "Settings", UI_NAV_SETTINGS, active_page == UI_NAV_SETTINGS);

	if (active_page < 4 && buttons[active_page]) {
		lv_obj_t *label = lv_obj_get_child(buttons[active_page], 0);
		if (label) {
			lv_obj_set_style_text_opa(label, LV_OPA_0, 0);
			lv_obj_set_style_transform_zoom(label, 220, 0);

			anim_set_text_opa(label, LV_OPA_COVER);
			anim_set_transform_zoom(label, 256);
		}

		lv_obj_t *indicator = lv_obj_create(nav);
		lv_obj_add_flag(indicator, LV_OBJ_FLAG_FLOATING);
		lv_obj_set_size(indicator, 70, 4);
		lv_obj_set_style_bg_color(indicator, lv_color_hex(0x4F77FF), 0);
		lv_obj_set_style_border_width(indicator, 0, 0);
		lv_obj_set_style_radius(indicator, 2, 0);
		lv_obj_update_layout(nav);
		lv_obj_align_to(indicator, buttons[active_page], LV_ALIGN_BOTTOM_MID, 0, 7);

		lv_coord_t target_x = lv_obj_get_x(indicator);
		s_last_indicator_x = target_x;
	}

	if (s_alert_badge_visible && buttons[UI_NAV_ALERTS]) {
		lv_obj_t *badge = lv_obj_create(buttons[UI_NAV_ALERTS]);
		lv_obj_set_size(badge, 10, 10);
		lv_obj_set_style_bg_color(badge, lv_color_hex(0xE74C3C), 0);
		lv_obj_set_style_border_width(badge, 0, 0);
		lv_obj_set_style_radius(badge, 5, 0);
		lv_obj_align(badge, LV_ALIGN_TOP_RIGHT, -6, 4);

		anim_set_transform_zoom(badge, 256);
	}
}
