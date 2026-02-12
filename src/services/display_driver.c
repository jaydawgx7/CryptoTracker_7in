#include "services/display_driver.h"

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "services/screenshot.h"

static const char *TAG = "display_driver";

static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static TaskHandle_t s_lvgl_task = NULL;
static esp_timer_handle_t s_lvgl_handler_timer = NULL;
static lv_disp_t *s_disp = NULL;
static lv_disp_drv_t s_disp_drv;
static bool s_refr_paused = false;
static lv_disp_draw_buf_t s_draw_buf;
static StaticTask_t s_lvgl_task_buf;
static StackType_t s_lvgl_stack[16384 / sizeof(StackType_t)] DRAM_ATTR;

#define DISP_BUF_LINES 40
#define CT_DISPLAY_TEST_PATTERN 0

#ifndef CT_LVGL_TASK_ENABLE
#define CT_LVGL_TASK_ENABLE 1
#endif

#ifndef CT_LVGL_DRY_RUN
#define CT_LVGL_DRY_RUN 0
#endif

#ifndef CT_LVGL_TASK_TICK
#define CT_LVGL_TASK_TICK 0
#endif

#ifndef CT_LVGL_HANDLER_ENABLE
#define CT_LVGL_HANDLER_ENABLE 1
#endif

#ifndef CT_LVGL_HANDLER_IN_TASK
#define CT_LVGL_HANDLER_IN_TASK 0
#endif

#ifndef CT_LVGL_SKIP_DISPLAY
#define CT_LVGL_SKIP_DISPLAY 0
#endif

#ifndef CT_LVGL_DUMMY_DISPLAY
#define CT_LVGL_DUMMY_DISPLAY 0
#endif

#ifndef CT_LVGL_PAUSE_REFR_TIMER
#define CT_LVGL_PAUSE_REFR_TIMER 0
#endif

static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}

static void lvgl_handler_timer_cb(void *arg)
{
    (void)arg;
    if (display_driver_lock(0)) {
        lv_timer_handler();
        display_driver_unlock();
    }
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)drv;
#if CT_LVGL_DRY_RUN
    (void)area;
    (void)color_map;
    lv_disp_flush_ready(drv);
    return;
#endif

    screenshot_update(area, color_map);

    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;

    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "LVGL task stack watermark: %u", (unsigned)watermark);

    while (true) {
        if (display_driver_lock(100)) {
#if CT_LVGL_TASK_TICK
            lv_tick_inc(10);
#endif
#if CT_LVGL_HANDLER_ENABLE
    #if CT_LVGL_HANDLER_IN_TASK
            lv_timer_handler();
    #endif
#endif
            display_driver_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void display_test_pattern(void)
{
    lv_color_t *line = heap_caps_malloc(CT_LCD_H_RES * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!line) {
        return;
    }

    lv_color_t color = lv_color_hex(0x00FF00);
    for (int x = 0; x < CT_LCD_H_RES; x++) {
        line[x] = color;
    }

    for (int y = 0; y < CT_LCD_V_RES; y++) {
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, CT_LCD_H_RES, y + 1, line);
    }

    heap_caps_free(line);
}

esp_err_t display_driver_init(void)
{
    lv_init();

    screenshot_init(CT_LCD_H_RES, CT_LCD_V_RES);


#if !CT_LVGL_SKIP_DISPLAY
    size_t buf_pixels = CT_LCD_H_RES * DISP_BUF_LINES;
    size_t buf_bytes = buf_pixels * sizeof(lv_color_t);
    lv_color_t *buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lv_color_t *buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf1) {
        ESP_LOGW(TAG, "PSRAM buffer alloc failed, falling back to internal RAM");
        buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf2) {
        ESP_LOGW(TAG, "Second PSRAM buffer alloc failed, falling back to internal RAM");
        buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf1) {
        ESP_LOGE(TAG, "LVGL buffer allocation failed (bytes=%u)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }

    if (!buf2) {
        ESP_LOGW(TAG, "Second LVGL buffer unavailable, using single buffer");
    }

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, CT_LCD_H_RES * DISP_BUF_LINES);

#if !CT_LVGL_DUMMY_DISPLAY

    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
        .psram_trans_align = 64,
        .sram_trans_align = 64,
        .bounce_buffer_size_px = 10 * CT_LCD_H_RES,
        .clk_src = LCD_CLK_SRC_PLL240M,
        .disp_gpio_num = -1,
        .pclk_gpio_num = CT_LCD_PIN_PCLK,
        .vsync_gpio_num = CT_LCD_PIN_VSYNC,
        .hsync_gpio_num = CT_LCD_PIN_HSYNC,
        .de_gpio_num = CT_LCD_PIN_DE,
        .data_gpio_nums = {
            CT_LCD_PIN_D0, CT_LCD_PIN_D1, CT_LCD_PIN_D2, CT_LCD_PIN_D3,
            CT_LCD_PIN_D4, CT_LCD_PIN_D5, CT_LCD_PIN_D6, CT_LCD_PIN_D7,
            CT_LCD_PIN_D8, CT_LCD_PIN_D9, CT_LCD_PIN_D10, CT_LCD_PIN_D11,
            CT_LCD_PIN_D12, CT_LCD_PIN_D13, CT_LCD_PIN_D14, CT_LCD_PIN_D15
        },
        .timings = {
            .pclk_hz = CT_LCD_PCLK_HZ,
            .h_res = CT_LCD_H_RES,
            .v_res = CT_LCD_V_RES,
            .hsync_pulse_width = CT_LCD_HSYNC_PW,
            .hsync_back_porch = CT_LCD_HSYNC_BP,
            .hsync_front_porch = CT_LCD_HSYNC_FP,
            .vsync_pulse_width = CT_LCD_VSYNC_PW,
            .vsync_back_porch = CT_LCD_VSYNC_BP,
            .vsync_front_porch = CT_LCD_VSYNC_FP,
            .flags = {
                .hsync_idle_low = false,
                .vsync_idle_low = false,
                .de_idle_high = false,
                .pclk_active_neg = CT_LCD_PCLK_ACTIVE_NEG,
                .pclk_idle_high = CT_LCD_PCLK_IDLE_HIGH
            }
        },
        .flags = {
            .fb_in_psram = CT_LCD_FB_IN_PSRAM
        }
    };

    ESP_LOGI(TAG,
             "LCD cfg: PCLK=%u Hz HSYNC=%u/%u/%u VSYNC=%u/%u/%u pclk_neg=%u idle_high=%u fb_psram=%u",
             (unsigned)CT_LCD_PCLK_HZ,
             (unsigned)CT_LCD_HSYNC_PW,
             (unsigned)CT_LCD_HSYNC_BP,
             (unsigned)CT_LCD_HSYNC_FP,
             (unsigned)CT_LCD_VSYNC_PW,
             (unsigned)CT_LCD_VSYNC_BP,
             (unsigned)CT_LCD_VSYNC_FP,
             (unsigned)CT_LCD_PCLK_ACTIVE_NEG,
             (unsigned)CT_LCD_PCLK_IDLE_HIGH,
             (unsigned)CT_LCD_FB_IN_PSRAM);

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_err_t disp_err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (disp_err != ESP_OK && disp_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(disp_err);
    }

    #if CT_DISPLAY_TEST_PATTERN
    display_test_pattern();
    vTaskDelay(pdMS_TO_TICKS(500));
    #endif

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = CT_LCD_H_RES;
    s_disp_drv.ver_res = CT_LCD_V_RES + 40;
    // Offset to compensate for panel vertical positioning
    s_disp_drv.offset_x = 0;
    s_disp_drv.offset_y = -40;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.full_refresh = 0;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;

    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL display register failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL display registered: disp=%p inv_areas=%p", (void *)s_disp, (void *)s_disp->inv_areas);
#else
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = CT_LCD_H_RES;
    s_disp_drv.ver_res = CT_LCD_V_RES + 40;
    // Offset to compensate for panel vertical positioning
    s_disp_drv.offset_x = 0;
    s_disp_drv.offset_y = -40;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = NULL;

    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL dummy display register failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL dummy display registered: disp=%p inv_areas=%p", (void *)s_disp, (void *)s_disp->inv_areas);
    ESP_LOGW(TAG, "LVGL dummy display enabled (panel init skipped)");
#endif
#if CT_LVGL_PAUSE_REFR_TIMER
    lv_timer_t *refr_timer = _lv_disp_get_refr_timer(s_disp);
    if (refr_timer) {
        lv_timer_pause(refr_timer);
        s_refr_paused = true;
        ESP_LOGW(TAG, "LVGL refresh timer paused");
    }
#endif
#else
    ESP_LOGW(TAG, "LVGL display registration skipped by config");
#endif

#if CT_LVGL_ENABLE && CT_LVGL_TASK_ENABLE && !CT_LVGL_TASK_TICK
    const esp_timer_create_args_t tick_args = {
        .callback = lv_tick_cb,
        .name = "lv_tick"
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));
#endif

    s_lvgl_mutex = xSemaphoreCreateMutex();
    if (!s_lvgl_mutex) {
        ESP_LOGE(TAG, "LVGL mutex create failed");
        return ESP_ERR_NO_MEM;
    }

#if CT_LVGL_HANDLER_ENABLE && !CT_LVGL_HANDLER_IN_TASK
    const esp_timer_create_args_t handler_args = {
        .callback = lvgl_handler_timer_cb,
        .name = "lv_handler"
    };
    ESP_ERROR_CHECK(esp_timer_create(&handler_args, &s_lvgl_handler_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_handler_timer, 16));
#endif

    ESP_LOGI(TAG, "Display initialized");
    return ESP_OK;
}

void display_driver_start(void)
{
#if CT_LVGL_ENABLE
    if (!s_lvgl_task) {
        s_lvgl_task = xTaskCreateStaticPinnedToCore(
            lvgl_task,
            "lvgl",
            (uint32_t)(sizeof(s_lvgl_stack) / sizeof(StackType_t)),
            NULL,
            4,
            s_lvgl_stack,
            &s_lvgl_task_buf,
            0
        );
        if (!s_lvgl_task) {
            ESP_LOGE(TAG, "Failed to create LVGL task");
        }
    }
#else
    ESP_LOGW(TAG, "LVGL task disabled by config");
#endif
}

bool display_driver_lock(uint32_t timeout_ms)
{
    if (!s_lvgl_mutex) {
        return false;
    }
    return xSemaphoreTake(s_lvgl_mutex, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void display_driver_unlock(void)
{
    if (s_lvgl_mutex) {
        xSemaphoreGive(s_lvgl_mutex);
    }
}

void display_driver_resume_refresh(void)
{
    if (!s_disp || !s_refr_paused) {
        return;
    }

    lv_timer_t *refr_timer = _lv_disp_get_refr_timer(s_disp);
    if (!refr_timer) {
        return;
    }

    lv_timer_resume(refr_timer);
    s_refr_paused = false;
    ESP_LOGI(TAG, "LVGL refresh timer resumed");
}
