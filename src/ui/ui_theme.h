#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
	uint32_t accent;
	uint32_t text_primary;
	uint32_t text_muted;
	uint32_t bg;
	uint32_t surface;
	uint32_t nav_active_bg;
	uint32_t nav_inactive_bg;
	uint32_t nav_text_active;
	uint32_t nav_text_inactive;
	bool dark_mode;
} ui_theme_colors_t;

void ui_theme_init(bool dark_mode);
void ui_theme_set_accent(uint32_t accent_hex);
const ui_theme_colors_t *ui_theme_get(void);
