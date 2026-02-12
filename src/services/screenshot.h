#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

bool screenshot_init(uint16_t width, uint16_t height);
void screenshot_update(const lv_area_t *area, const lv_color_t *color_map);
bool screenshot_get_size(uint16_t *out_width, uint16_t *out_height);
bool screenshot_read_row(uint16_t y, uint16_t *out_row, size_t pixel_count);
