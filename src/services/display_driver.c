#include "services/display_driver.h"

#include "esp_err.h"
#include "esp_attr.h"
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
#include "ui/ui.h"

static const char *TAG = "display_driver";

static esp_lcd_panel_handle_t s_panel = NULL;
static SemaphoreHandle_t s_lvgl_mutex = NULL;
static TaskHandle_t s_lvgl_task = NULL;
#if CT_LVGL_HANDLER_ENABLE && !CT_LVGL_HANDLER_IN_TASK
static esp_timer_handle_t s_lvgl_handler_timer = NULL;
#endif
static lv_disp_t *s_disp = NULL;
static lv_disp_drv_t s_disp_drv;
static bool s_refr_paused = false;
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_frame_buffers[2] = {NULL, NULL};
static size_t s_frame_buffer_pixels = 0;
static SemaphoreHandle_t s_vsync_sem = NULL;
static StaticSemaphore_t s_vsync_sem_buf;
static portMUX_TYPE s_present_mux = portMUX_INITIALIZER_UNLOCKED;
static lv_disp_drv_t *s_pending_flush_drv = NULL;
static lv_color_t *s_pending_frame_buffer = NULL;
static lv_color_t *s_presented_frame_buffer = NULL;
static TaskHandle_t s_present_task = NULL;
static StaticTask_t s_lvgl_task_buf;
static StaticTask_t s_present_task_buf;
static StackType_t s_lvgl_stack[16384 / sizeof(StackType_t)] DRAM_ATTR;
static StackType_t s_present_stack[4096 / sizeof(StackType_t)] DRAM_ATTR;
static int64_t s_last_slow_handler_log_us = 0;

static void mirror_flush_area_to_peer_buffer(const lv_area_t *area, lv_color_t *frame_buffer)
{
#if CT_LCD_RGB_FULL_FRAME_BUFFERS
    if (!area || !frame_buffer || !s_frame_buffers[0] || !s_frame_buffers[1]) {
        return;
    }

    lv_color_t *peer_buffer = NULL;
    if (frame_buffer == s_frame_buffers[0]) {
        peer_buffer = s_frame_buffers[1];
    } else if (frame_buffer == s_frame_buffers[1]) {
        peer_buffer = s_frame_buffers[0];
    }

    if (!peer_buffer) {
        return;
    }

    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 >= CT_LCD_H_RES ? CT_LCD_H_RES - 1 : area->x2;
    int32_t y2 = area->y2 >= CT_LCD_V_RES ? CT_LCD_V_RES - 1 : area->y2;
    if (x2 < x1 || y2 < y1) {
        return;
    }

    size_t row_pixels = (size_t)(x2 - x1 + 1);
    for (int32_t y = y1; y <= y2; y++) {
        size_t offset = (size_t)y * CT_LCD_H_RES + (size_t)x1;
        memcpy(peer_buffer + offset, frame_buffer + offset, row_pixels * sizeof(lv_color_t));
    }
#else
    (void)area;
    (void)frame_buffer;
#endif
}

#ifndef CT_LVGL_DISP_BUF_LINES
#define CT_LVGL_DISP_BUF_LINES 120
#endif

#define DISP_BUF_LINES CT_LVGL_DISP_BUF_LINES
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

#ifndef CT_LVGL_TASK_CORE
#define CT_LVGL_TASK_CORE 1
#endif

#ifndef CT_LVGL_TASK_PRIORITY
#define CT_LVGL_TASK_PRIORITY 2
#endif

#ifndef CT_LCD_PRESENT_TASK_PRIORITY
#define CT_LCD_PRESENT_TASK_PRIORITY 4
#endif

#ifndef CT_LCD_PRESENT_TASK_CORE
#define CT_LCD_PRESENT_TASK_CORE CT_LVGL_TASK_CORE
#endif

#ifndef CT_LVGL_HANDLER_WARN_MS
#define CT_LVGL_HANDLER_WARN_MS 250
#endif

#ifndef CT_LVGL_TASK_SLEEP_MIN_MS
#define CT_LVGL_TASK_SLEEP_MIN_MS 16
#endif

#ifndef CT_LVGL_TASK_SLEEP_MAX_MS
#define CT_LVGL_TASK_SLEEP_MAX_MS 33
#endif

#ifndef CT_LCD_RGB_FULL_FRAME_BUFFERS
#define CT_LCD_RGB_FULL_FRAME_BUFFERS 1
#endif

#ifndef CT_LCD_RGB_VSYNC_PRESENTATION
#define CT_LCD_RGB_VSYNC_PRESENTATION 1
#endif

#if CT_LVGL_ENABLE && CT_LVGL_TASK_ENABLE && !CT_LVGL_TASK_TICK
static void lv_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(2);
}
#endif

#if CT_LVGL_HANDLER_ENABLE && !CT_LVGL_HANDLER_IN_TASK
static void lvgl_handler_timer_cb(void *arg)
{
    (void)arg;
    if (display_driver_lock(0)) {
        lv_timer_handler();
        display_driver_unlock();
    }
}
#endif

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)drv;
#if CT_LVGL_DRY_RUN
    (void)area;
    (void)color_map;
    lv_disp_flush_ready(drv);
    return;
#endif

    lv_color_t *frame_buffer = NULL;

#if CT_LCD_RGB_FULL_FRAME_BUFFERS
    if (s_disp_drv.direct_mode) {
        for (size_t i = 0; i < 2; i++) {
            lv_color_t *candidate = s_frame_buffers[i];
            if (!candidate) {
                continue;
            }
            if (color_map >= candidate && color_map < (candidate + s_frame_buffer_pixels)) {
                frame_buffer = candidate;
                break;
            }
        }

        if (frame_buffer) {
            mirror_flush_area_to_peer_buffer(area, frame_buffer);
            screenshot_update_framebuffer(area, frame_buffer, CT_LCD_H_RES);
        } else {
            screenshot_update(area, color_map);
        }

        if (!lv_disp_flush_is_last(drv)) {
            lv_disp_flush_ready(drv);
            return;
        }

        if (frame_buffer && s_present_task) {
            portENTER_CRITICAL(&s_present_mux);
            s_pending_frame_buffer = frame_buffer;
            s_pending_flush_drv = drv;
            portEXIT_CRITICAL(&s_present_mux);
            return;
        }
    } else {
        screenshot_update(area, color_map);
    }
#else
    screenshot_update(area, color_map);
#endif

    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2 + 1;
    int y2 = area->y2 + 1;

    esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, color_map);
    lv_disp_flush_ready(drv);
}

#if CT_LCD_RGB_VSYNC_PRESENTATION
static bool IRAM_ATTR rgb_panel_vsync_cb(esp_lcd_panel_handle_t panel,
                                         const esp_lcd_rgb_panel_event_data_t *edata,
                                         void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;

    if (!s_vsync_sem) {
        return false;
    }

    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}
#endif

static void present_task(void *arg)
{
    (void)arg;

    while (true) {
        if (xSemaphoreTake(s_vsync_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        lv_disp_drv_t *pending_drv = NULL;
        lv_color_t *pending_frame = NULL;

        portENTER_CRITICAL(&s_present_mux);
        pending_drv = s_pending_flush_drv;
        pending_frame = s_pending_frame_buffer;
        s_pending_flush_drv = NULL;
        s_pending_frame_buffer = NULL;
        portEXIT_CRITICAL(&s_present_mux);

        if (!pending_drv || !pending_frame || !s_panel) {
            continue;
        }

        esp_lcd_panel_draw_bitmap(s_panel, 0, 0, CT_LCD_H_RES, CT_LCD_V_RES, pending_frame);
        portENTER_CRITICAL(&s_present_mux);
        s_presented_frame_buffer = pending_frame;
        portEXIT_CRITICAL(&s_present_mux);
        lv_disp_flush_ready(pending_drv);
    }
}

static void lvgl_task(void *arg)
{
    (void)arg;
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "LVGL task stack watermark: %u", (unsigned)watermark);
    int64_t last_tick_us = esp_timer_get_time();

    while (true) {
        uint32_t sleep_ms = CT_LVGL_TASK_SLEEP_MIN_MS;
        if (display_driver_lock(100)) {
            int64_t start_us = esp_timer_get_time();
            ui_process_pending();
#if CT_LVGL_TASK_TICK
            uint32_t delta_ms = 0;
            if (start_us > last_tick_us) {
                delta_ms = (uint32_t)((start_us - last_tick_us) / 1000LL);
            }
            if (delta_ms == 0) {
                delta_ms = CT_LVGL_TASK_SLEEP_MIN_MS;
            }
            lv_tick_inc(delta_ms);
            last_tick_us = start_us;
#endif
#if CT_LVGL_HANDLER_ENABLE
    #if CT_LVGL_HANDLER_IN_TASK
            uint32_t wait_ms = lv_timer_handler();
            if (wait_ms > 0) {
                sleep_ms = wait_ms;
            }
    #endif
#endif
            int64_t elapsed_us = esp_timer_get_time() - start_us;
            display_driver_unlock();

            if (elapsed_us > (int64_t)CT_LVGL_HANDLER_WARN_MS * 1000LL) {
                int64_t now_us = esp_timer_get_time();
                if ((now_us - s_last_slow_handler_log_us) >= 5000000LL) {
                    ESP_LOGW(TAG,
                             "Slow LVGL handler: %lld ms, suggested sleep=%u ms",
                             (long long)(elapsed_us / 1000LL),
                             (unsigned)sleep_ms);
                    s_last_slow_handler_log_us = now_us;
                }
            }
        }

        if (sleep_ms < CT_LVGL_TASK_SLEEP_MIN_MS) {
            sleep_ms = CT_LVGL_TASK_SLEEP_MIN_MS;
        } else if (sleep_ms > CT_LVGL_TASK_SLEEP_MAX_MS) {
            sleep_ms = CT_LVGL_TASK_SLEEP_MAX_MS;
        }

        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}

#if CT_DISPLAY_TEST_PATTERN
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
#endif

esp_err_t display_driver_init(void)
{
    lv_init();

    screenshot_init(CT_LCD_H_RES, CT_LCD_V_RES);


#if !CT_LVGL_SKIP_DISPLAY
    lv_color_t *buf1 = NULL;
    lv_color_t *buf2 = NULL;

#if CT_LCD_RGB_VSYNC_PRESENTATION
    if (!s_vsync_sem) {
        s_vsync_sem = xSemaphoreCreateBinaryStatic(&s_vsync_sem_buf);
        if (!s_vsync_sem) {
            ESP_LOGE(TAG, "VSYNC semaphore create failed");
            return ESP_ERR_NO_MEM;
        }
    }
#endif

#if !CT_LVGL_DUMMY_DISPLAY

    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = 16,
        .num_fbs = CT_LCD_RGB_FULL_FRAME_BUFFERS ? 2 : 0,
        .psram_trans_align = 64,
        .sram_trans_align = 64,
        .bounce_buffer_size_px = CT_LCD_BOUNCE_LINES * CT_LCD_H_RES,
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

#if CT_LCD_RGB_VSYNC_PRESENTATION
    const esp_lcd_rgb_panel_event_callbacks_t rgb_callbacks = {
        .on_vsync = rgb_panel_vsync_cb,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(s_panel, &rgb_callbacks, NULL));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    esp_err_t disp_err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (disp_err != ESP_OK && disp_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(disp_err);
    }

#if CT_LCD_RGB_FULL_FRAME_BUFFERS
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, (void **)&buf1, (void **)&buf2));
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "RGB frame buffer acquisition failed: fb0=%p fb1=%p", (void *)buf1, (void *)buf2);
        return ESP_FAIL;
    }

    s_frame_buffers[0] = buf1;
    s_frame_buffers[1] = buf2;
    s_presented_frame_buffer = buf1;
    s_frame_buffer_pixels = (size_t)CT_LCD_H_RES * (size_t)CT_LCD_V_RES;
    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, CT_LCD_H_RES * CT_LCD_V_RES);
    ESP_LOGI(TAG,
             "Using RGB driver frame buffers for LVGL: fb0=%p fb1=%p size=%u px",
             (void *)buf1,
             (void *)buf2,
             (unsigned)(CT_LCD_H_RES * CT_LCD_V_RES));
#else
    size_t buf_pixels = CT_LCD_H_RES * DISP_BUF_LINES;
    size_t buf_bytes = buf_pixels * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

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
#endif

    #if CT_DISPLAY_TEST_PATTERN
    display_test_pattern();
    vTaskDelay(pdMS_TO_TICKS(500));
    #endif

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = CT_LCD_H_RES;
    s_disp_drv.ver_res = CT_LCD_LVGL_V_RES;
    s_disp_drv.offset_x = CT_LCD_X_OFFSET;
    s_disp_drv.offset_y = CT_LCD_Y_OFFSET;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.full_refresh = 0;
    s_disp_drv.direct_mode = CT_LCD_RGB_FULL_FRAME_BUFFERS ? 1 : 0;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp_drv.user_data = s_panel;

    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        ESP_LOGE(TAG, "LVGL display register failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "LVGL display registered: disp=%p inv_areas=%p", (void *)s_disp, (void *)s_disp->inv_areas);
#else
    size_t buf_pixels = CT_LCD_H_RES * DISP_BUF_LINES;
    size_t buf_bytes = buf_pixels * sizeof(lv_color_t);
    buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf1) {
        ESP_LOGW(TAG, "PSRAM buffer alloc failed, falling back to internal RAM");
        buf1 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf2) {
        ESP_LOGW(TAG, "Second PSRAM buffer alloc failed, falling back to internal RAM");
        buf2 = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }

    if (!buf1) {
        ESP_LOGE(TAG, "LVGL dummy buffer allocation failed (bytes=%u)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }

    if (!buf2) {
        ESP_LOGW(TAG, "Second LVGL dummy buffer unavailable, using single buffer");
    }

    lv_disp_draw_buf_init(&s_draw_buf, buf1, buf2, CT_LCD_H_RES * DISP_BUF_LINES);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = CT_LCD_H_RES;
    s_disp_drv.ver_res = CT_LCD_LVGL_V_RES;
    s_disp_drv.offset_x = CT_LCD_X_OFFSET;
    s_disp_drv.offset_y = CT_LCD_Y_OFFSET;
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
    if (!s_present_task && CT_LCD_RGB_FULL_FRAME_BUFFERS && CT_LCD_RGB_VSYNC_PRESENTATION) {
        s_present_task = xTaskCreateStaticPinnedToCore(
            present_task,
            "lcd_present",
            (uint32_t)(sizeof(s_present_stack) / sizeof(StackType_t)),
            NULL,
            CT_LCD_PRESENT_TASK_PRIORITY,
            s_present_stack,
            &s_present_task_buf,
            CT_LCD_PRESENT_TASK_CORE
        );
        if (!s_present_task) {
            ESP_LOGE(TAG, "Failed to create present task");
        } else {
            ESP_LOGI(TAG, "Present task pinned to core %d priority %d", CT_LCD_PRESENT_TASK_CORE, CT_LCD_PRESENT_TASK_PRIORITY);
        }
    }

    if (!s_lvgl_task) {
        s_lvgl_task = xTaskCreateStaticPinnedToCore(
            lvgl_task,
            "lvgl",
            (uint32_t)(sizeof(s_lvgl_stack) / sizeof(StackType_t)),
            NULL,
            CT_LVGL_TASK_PRIORITY,
            s_lvgl_stack,
            &s_lvgl_task_buf,
            CT_LVGL_TASK_CORE
        );
        if (!s_lvgl_task) {
            ESP_LOGE(TAG, "Failed to create LVGL task");
        } else {
            ESP_LOGI(TAG, "LVGL task pinned to core %d priority %d", CT_LVGL_TASK_CORE, CT_LVGL_TASK_PRIORITY);
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

bool display_driver_capture_screenshot(void)
{
#if CT_LCD_RGB_FULL_FRAME_BUFFERS
    lv_color_t *framebuffer = NULL;

    portENTER_CRITICAL(&s_present_mux);
    framebuffer = s_presented_frame_buffer ? s_presented_frame_buffer : s_pending_frame_buffer;
    portEXIT_CRITICAL(&s_present_mux);

    if (framebuffer) {
        return screenshot_update_full_frame(framebuffer, CT_LCD_H_RES);
    }
#endif

    return screenshot_get_size(NULL, NULL);
}
