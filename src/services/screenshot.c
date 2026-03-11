#include "services/screenshot.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "screenshot";

static uint16_t *s_shadow_fb = NULL;
static uint16_t s_width = 0;
static uint16_t s_height = 0;
static SemaphoreHandle_t s_mutex = NULL;

static bool clamp_area(const lv_area_t *area, int32_t *x1, int32_t *y1, int32_t *x2, int32_t *y2)
{
    if (!area || !x1 || !y1 || !x2 || !y2) {
        return false;
    }

    *x1 = area->x1 < 0 ? 0 : area->x1;
    *y1 = area->y1 < 0 ? 0 : area->y1;
    *x2 = area->x2 >= s_width ? (int32_t)s_width - 1 : area->x2;
    *y2 = area->y2 >= s_height ? (int32_t)s_height - 1 : area->y2;
    return *x2 >= *x1 && *y2 >= *y1;
}

bool screenshot_init(uint16_t width, uint16_t height)
{
    if (width == 0 || height == 0) {
        return false;
    }

    size_t bytes = (size_t)width * (size_t)height * sizeof(uint16_t);
    s_shadow_fb = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_shadow_fb) {
        ESP_LOGE(TAG, "Shadow framebuffer alloc failed (%u bytes)", (unsigned)bytes);
        return false;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "Shadow framebuffer mutex alloc failed");
        heap_caps_free(s_shadow_fb);
        s_shadow_fb = NULL;
        return false;
    }

    s_width = width;
    s_height = height;
    memset(s_shadow_fb, 0, bytes);
    ESP_LOGI(TAG, "Shadow framebuffer ready (%ux%u)", (unsigned)width, (unsigned)height);
    return true;
}

void screenshot_update(const lv_area_t *area, const lv_color_t *color_map)
{
    if (!s_shadow_fb || !area || !color_map) {
        return;
    }

    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    if (!clamp_area(area, &x1, &y1, &x2, &y2)) {
        return;
    }

    size_t row_pixels = (size_t)(x2 - x1 + 1);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    for (int32_t y = y1; y <= y2; y++) {
        size_t dst_offset = (size_t)y * s_width + (size_t)x1;
        size_t src_offset = (size_t)(y - y1) * row_pixels;
        memcpy(&s_shadow_fb[dst_offset], &color_map[src_offset], row_pixels * sizeof(uint16_t));
    }

    xSemaphoreGive(s_mutex);
}

void screenshot_update_framebuffer(const lv_area_t *area, const lv_color_t *framebuffer, uint16_t stride)
{
    if (!s_shadow_fb || !area || !framebuffer || stride == 0) {
        return;
    }

    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
    if (!clamp_area(area, &x1, &y1, &x2, &y2)) {
        return;
    }

    size_t row_pixels = (size_t)(x2 - x1 + 1);

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    for (int32_t y = y1; y <= y2; y++) {
        size_t dst_offset = (size_t)y * s_width + (size_t)x1;
        size_t src_offset = (size_t)y * stride + (size_t)x1;
        memcpy(&s_shadow_fb[dst_offset], &framebuffer[src_offset], row_pixels * sizeof(uint16_t));
    }

    xSemaphoreGive(s_mutex);
}

bool screenshot_update_full_frame(const lv_color_t *framebuffer, uint16_t stride)
{
    if (!s_shadow_fb || !framebuffer || stride < s_width) {
        return false;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return false;
    }

    for (uint16_t y = 0; y < s_height; y++) {
        size_t dst_offset = (size_t)y * s_width;
        size_t src_offset = (size_t)y * stride;
        memcpy(&s_shadow_fb[dst_offset], &framebuffer[src_offset], (size_t)s_width * sizeof(uint16_t));
    }

    xSemaphoreGive(s_mutex);
    return true;
}

bool screenshot_get_size(uint16_t *out_width, uint16_t *out_height)
{
    if (!s_shadow_fb) {
        return false;
    }

    if (out_width) {
        *out_width = s_width;
    }
    if (out_height) {
        *out_height = s_height;
    }
    return true;
}

bool screenshot_lock(uint32_t timeout_ms)
{
    if (!s_shadow_fb || !s_mutex) {
        return false;
    }

    return xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void screenshot_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

bool screenshot_read_row_locked(uint16_t y, uint16_t *out_row, size_t pixel_count)
{
    if (!s_shadow_fb || !out_row || y >= s_height || pixel_count < s_width) {
        return false;
    }

    memcpy(out_row, &s_shadow_fb[(size_t)y * s_width], s_width * sizeof(uint16_t));
    return true;
}

bool screenshot_read_row(uint16_t y, uint16_t *out_row, size_t pixel_count)
{
    if (!screenshot_lock(50)) {
        return false;
    }

    bool ok = screenshot_read_row_locked(y, out_row, pixel_count);
    screenshot_unlock();
    return ok;
}
