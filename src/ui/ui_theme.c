#include "ui/ui_theme.h"

#include "lvgl.h"

#include <string.h>

static lv_theme_t s_custom_theme;
static lv_style_t s_pressed_style;
static bool s_pressed_style_inited = false;

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
    .dark_mode = true
};

static void theme_apply_cb(lv_theme_t *theme, lv_obj_t *obj)
{
    if (theme->parent && theme->parent->apply_cb) {
        theme->parent->apply_cb(theme->parent, obj);
    }

    if (lv_obj_check_type(obj, &lv_btn_class)) {
        lv_obj_add_style(obj, &s_pressed_style, LV_PART_MAIN | LV_STATE_PRESSED);
    }
}

void ui_theme_init(bool dark_mode)
{
    s_theme.dark_mode = dark_mode;

    lv_theme_t *base = lv_theme_default_init(NULL,
                                             dark_mode ? lv_color_hex(s_theme.text_primary) : lv_color_hex(0x0F1117),
                                             dark_mode ? lv_color_hex(0x101218) : lv_color_hex(0xFFFFFF),
                                             dark_mode,
                                             &lv_font_montserrat_16);

    if (!s_pressed_style_inited) {
        lv_style_init(&s_pressed_style);
        lv_style_set_bg_opa(&s_pressed_style, LV_OPA_COVER);
        s_pressed_style_inited = true;
    }
    lv_style_set_bg_color(&s_pressed_style, lv_color_hex(s_theme.nav_active_bg));

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
    s_theme.nav_text_inactive = accent_hex;
    if (s_pressed_style_inited) {
        lv_style_set_bg_color(&s_pressed_style, lv_color_hex(s_theme.nav_active_bg));
    }
}

const ui_theme_colors_t *ui_theme_get(void)
{
    return &s_theme;
}
