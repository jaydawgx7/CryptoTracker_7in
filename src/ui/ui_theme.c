#include "ui/ui_theme.h"

#include "lvgl.h"

void ui_theme_init(bool dark_mode)
{
    lv_theme_t *theme = lv_theme_default_init(NULL,
                                             dark_mode ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x0F1117),
                                             dark_mode ? lv_color_hex(0x101218) : lv_color_hex(0xFFFFFF),
                                             dark_mode,
                                             &lv_font_montserrat_16);
    lv_disp_set_theme(lv_disp_get_default(), theme);
}
