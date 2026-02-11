#include "ui/ui_theme.h"

#include "lvgl.h"

#include <string.h>

static lv_theme_t s_custom_theme;
static lv_style_t s_pressed_style;
static lv_style_t s_button_shadow_style;
static bool s_pressed_style_inited = false;
static bool s_button_shadow_inited = false;
static bool s_buttons_3d = true;

static ui_theme_colors_t s_theme = {
    .accent = 0x00FE8F,
    .text_primary = 0xE6E6E6,
    .text_muted = 0x9AA1AD,
    .bg = 0x0F1117,
    .surface = 0x1A1D26,
    .nav_active_bg = 0x424242,
    .nav_inactive_bg = 0x212121,
    .nav_text_active = 0x00FE8F,
    .nav_text_inactive = 0x00FE8F,
    .shadow_color = 0x2A3142,
    .dark_mode = true
};

static void ui_theme_update_palette(bool dark_mode)
{
    s_theme.dark_mode = dark_mode;

    if (dark_mode) {
        s_theme.text_primary = 0xE6E6E6;
        s_theme.text_muted = 0x9AA1AD;
        s_theme.bg = 0x0F1117;
        s_theme.surface = 0x1A1D26;
        s_theme.nav_active_bg = 0x424242;
        s_theme.nav_inactive_bg = 0x212121;
        s_theme.nav_text_active = s_theme.accent;
        s_theme.nav_text_inactive = s_theme.accent;
    } else {
        s_theme.text_primary = 0x0F1117;
        s_theme.text_muted = 0x6B7280;
        s_theme.bg = 0xF5F7FB;
        s_theme.surface = 0xFFFFFF;
        s_theme.nav_active_bg = 0xE2E8F0;
        s_theme.nav_inactive_bg = 0xF1F5F9;
        s_theme.nav_text_active = s_theme.accent;
        s_theme.nav_text_inactive = 0x6B7280;
    }
}

static void theme_apply_cb(lv_theme_t *theme, lv_obj_t *obj)
{
    if (theme->parent && theme->parent->apply_cb) {
        theme->parent->apply_cb(theme->parent, obj);
    }

    if (lv_obj_check_type(obj, &lv_btn_class)) {
        lv_obj_add_style(obj, &s_pressed_style, LV_PART_MAIN | LV_STATE_PRESSED);
        lv_obj_add_style(obj, &s_button_shadow_style, LV_PART_MAIN);
    }
}

static void update_button_shadow_style(void)
{
    if (!s_button_shadow_inited) {
        return;
    }

    if (s_buttons_3d) {
        lv_style_set_shadow_width(&s_button_shadow_style, 10);
        lv_style_set_shadow_ofs_y(&s_button_shadow_style, 4);
        lv_style_set_shadow_opa(&s_button_shadow_style, LV_OPA_80);
        lv_style_set_shadow_color(&s_button_shadow_style, lv_color_hex(s_theme.shadow_color));
    } else {
        lv_style_set_shadow_width(&s_button_shadow_style, 0);
        lv_style_set_shadow_ofs_y(&s_button_shadow_style, 0);
        lv_style_set_shadow_opa(&s_button_shadow_style, LV_OPA_TRANSP);
    }
}

void ui_theme_init(bool dark_mode)
{
    ui_theme_update_palette(dark_mode);

    lv_theme_t *base = lv_theme_default_init(NULL,
                                             lv_color_hex(s_theme.text_primary),
                                             dark_mode ? lv_color_hex(0x101218) : lv_color_hex(0xFFFFFF),
                                             dark_mode,
                                             &lv_font_montserrat_16);

    if (!s_pressed_style_inited) {
        lv_style_init(&s_pressed_style);
        lv_style_set_bg_opa(&s_pressed_style, LV_OPA_COVER);
        s_pressed_style_inited = true;
    }
    lv_style_set_bg_color(&s_pressed_style, lv_color_hex(s_theme.nav_active_bg));

    if (!s_button_shadow_inited) {
        lv_style_init(&s_button_shadow_style);
        s_button_shadow_inited = true;
    }
    update_button_shadow_style();

    memset(&s_custom_theme, 0, sizeof(s_custom_theme));
    s_custom_theme.disp = lv_disp_get_default();
    if (base) {
        s_custom_theme.color_primary = base->color_primary;
        s_custom_theme.color_secondary = base->color_secondary;
        s_custom_theme.font_small = base->font_small;
        s_custom_theme.font_normal = base->font_normal;
        s_custom_theme.font_large = base->font_large;
        lv_theme_set_parent(&s_custom_theme, base);
    }
    lv_theme_set_apply_cb(&s_custom_theme, theme_apply_cb);
    lv_disp_set_theme(lv_disp_get_default(), &s_custom_theme);
}

void ui_theme_set_accent(uint32_t accent_hex)
{
    s_theme.accent = accent_hex;
    s_theme.nav_text_active = accent_hex;
    s_theme.nav_text_inactive = s_theme.dark_mode ? accent_hex : 0x6B7280;
    if (s_pressed_style_inited) {
        lv_style_set_bg_color(&s_pressed_style, lv_color_hex(s_theme.nav_active_bg));
    }
}

void ui_theme_set_shadow_color(uint32_t shadow_hex)
{
    s_theme.shadow_color = shadow_hex;
    update_button_shadow_style();
    lv_obj_report_style_change(&s_button_shadow_style);
}

void ui_theme_set_dark_mode(bool dark_mode)
{
    ui_theme_update_palette(dark_mode);
}

void ui_theme_set_buttons_3d(bool enabled)
{
    s_buttons_3d = enabled;
    update_button_shadow_style();
    lv_obj_report_style_change(&s_button_shadow_style);
}

const ui_theme_colors_t *ui_theme_get(void)
{
    return &s_theme;
}
