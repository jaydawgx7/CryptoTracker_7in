#include "services/touch_driver.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "services/control_mcu.h"
#include "services/display_driver.h"
#include "services/i2c_bus.h"

static const char *TAG = "touch_driver";

static lv_indev_t *s_indev = NULL;
static lv_indev_drv_t s_indev_drv;
static bool s_touched = false;
static bool s_last_reported_touched = false;
static lv_point_t s_point = {0};
static uint16_t s_touch_max_x = CT_LCD_H_RES;
static uint16_t s_touch_max_y = CT_LCD_V_RES;
static volatile bool s_int_pending = false;
static int64_t s_last_read_us = 0;
static int64_t s_last_log_us = 0;
static int64_t s_last_poll_log_us = 0;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static esp_lcd_touch_io_gt911_config_t s_gt911_cfg = {0};

static void apply_transform(uint16_t *x, uint16_t *y)
{
    int32_t tx = *x;
    int32_t ty = *y;
    int32_t target_h = CT_TOUCH_TARGET_H_RES;
    int32_t target_v = CT_TOUCH_TARGET_V_RES;
    int32_t phys_h = CT_TOUCH_PHYS_H_RES;
    int32_t phys_v = CT_TOUCH_PHYS_V_RES;

    if (s_touch_max_x > 1 && s_touch_max_x != (uint16_t)phys_h) {
        tx = (tx * (phys_h - 1)) / (s_touch_max_x - 1);
    }
    if (s_touch_max_y > 1 && s_touch_max_y != (uint16_t)phys_v) {
        ty = (ty * (phys_v - 1)) / (s_touch_max_y - 1);
    }

    if (CT_TOUCH_CAL_X_MAX > CT_TOUCH_CAL_X_MIN) {
        tx = (tx - CT_TOUCH_CAL_X_MIN) * (phys_h - 1) / (CT_TOUCH_CAL_X_MAX - CT_TOUCH_CAL_X_MIN);
    }
    if (CT_TOUCH_CAL_Y_MAX > CT_TOUCH_CAL_Y_MIN) {
        ty = (ty - CT_TOUCH_CAL_Y_MIN) * (phys_v - 1) / (CT_TOUCH_CAL_Y_MAX - CT_TOUCH_CAL_Y_MIN);
    }

    tx += CT_TOUCH_OFFSET_X;
    ty += CT_TOUCH_OFFSET_Y;

    if (tx < 0) {
        tx = 0;
    }
    if (ty < 0) {
        ty = 0;
    }
    if (tx >= target_h) {
        tx = target_h - 1;
    }
    if (ty >= target_v) {
        ty = target_v - 1;
    }

    *x = (uint16_t)tx;
    *y = (uint16_t)ty;
}

static void touch_process_coordinates(esp_lcd_touch_handle_t tp, uint16_t *x, uint16_t *y,
                                      uint16_t *strength, uint8_t *point_num, uint8_t max_point_num)
{
    (void)tp;
    (void)strength;
    (void)max_point_num;

    for (uint8_t i = 0; i < *point_num; i++) {
        apply_transform(&x[i], &y[i]);
    }
}

static void touch_int_wakeup_pulse(void)
{
#if CT_TOUCH_INT_GPIO >= 0
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CT_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(CT_TOUCH_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CT_TOUCH_INT_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(CT_TOUCH_INT_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = CT_TOUCH_INT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);
    vTaskDelay(pdMS_TO_TICKS(100));
#endif
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    int64_t now = esp_timer_get_time();
    bool should_read = true;

#if CT_TOUCH_USE_INT
    int64_t delta_ms = (now - s_last_read_us) / 1000;
    should_read = s_int_pending || s_touched || (delta_ms >= CT_TOUCH_POLL_FALLBACK_MS);
#endif

    if (!should_read) {
        data->state = s_touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->point = s_point;
        return;
    }

    s_int_pending = false;
    s_last_read_us = now;

    esp_err_t err = esp_lcd_touch_read_data(s_touch);
    if (err != ESP_OK) {
#if CT_TOUCH_LOG
        int64_t now_log = esp_timer_get_time();
        if ((now_log - s_last_poll_log_us) > 1000000) {
            s_last_poll_log_us = now_log;
            ESP_LOGW(TAG, "GT911 read failed: %d", err);
        }
#endif
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t touch_data[1] = {0};
    uint8_t touch_cnt = 0;
    err = esp_lcd_touch_get_data(s_touch, touch_data, &touch_cnt, 1);
    if (err != ESP_OK) {
#if CT_TOUCH_LOG
        int64_t now_log = esp_timer_get_time();
        if ((now_log - s_last_poll_log_us) > 1000000) {
            s_last_poll_log_us = now_log;
            ESP_LOGW(TAG, "GT911 get data failed: %d", err);
        }
#endif
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    if (touch_cnt > 0) {
        s_point.x = (lv_coord_t)touch_data[0].x;
        s_point.y = (lv_coord_t)touch_data[0].y;
        s_touched = true;
    } else {
        s_touched = false;
    }

#if CT_TOUCH_LOG
    int64_t now_log = esp_timer_get_time();
    if ((now_log - s_last_poll_log_us) > 1000000) {
        s_last_poll_log_us = now_log;
        int int_level = -1;
#if CT_TOUCH_INT_GPIO >= 0
        int_level = gpio_get_level(CT_TOUCH_INT_GPIO);
#endif
        ESP_LOGI(TAG, "GT911 points=%u int=%d", (unsigned)touch_cnt, int_level);
    }
#endif

    data->state = s_touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = s_point;

#if CT_TOUCH_LOG
    {
        int64_t now_log = esp_timer_get_time();
        if (s_touched) {
            if (!s_last_reported_touched || (now_log - s_last_log_us) > 200000) {
                s_last_log_us = now_log;
                ESP_LOGI(TAG, "Touch: x=%u y=%u", (unsigned)s_point.x, (unsigned)s_point.y);
            }
        } else if (s_last_reported_touched) {
            ESP_LOGI(TAG, "Touch: released");
        }
        s_last_reported_touched = s_touched;
    }
#endif

}

static void IRAM_ATTR touch_isr_handler(void *arg)
{
    (void)arg;
    s_int_pending = true;
}

esp_err_t touch_driver_init(void)
{
#if !CT_TOUCH_ENABLED
    ESP_LOGW(TAG, "Touch disabled by config");
    return ESP_OK;
#endif
    ESP_LOGI(TAG, "Touch config: addr=0x%02X use_int=%d int_gpio=%d poll_fallback_ms=%d",
             CT_TOUCH_I2C_ADDR, CT_TOUCH_USE_INT, CT_TOUCH_INT_GPIO, CT_TOUCH_POLL_FALLBACK_MS);
    ESP_ERROR_CHECK(i2c_bus_init());
    control_mcu_touch_enable();
    touch_int_wakeup_pulse();

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_config.dev_addr = CT_TOUCH_I2C_ADDR;

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)CT_I2C_PORT, &io_config, &s_touch_io),
                        TAG, "Touch IO init failed");

    s_gt911_cfg.dev_addr = io_config.dev_addr;

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = CT_LCD_H_RES,
        .y_max = CT_LCD_V_RES,
        .rst_gpio_num = (CT_TOUCH_RESET_GPIO >= 0) ? CT_TOUCH_RESET_GPIO : GPIO_NUM_NC,
        .int_gpio_num = (CT_TOUCH_INT_GPIO >= 0) ? CT_TOUCH_INT_GPIO : GPIO_NUM_NC,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = CT_TOUCH_SWAP_XY ? 1U : 0U,
            .mirror_x = CT_TOUCH_INVERT_X ? 1U : 0U,
            .mirror_y = CT_TOUCH_INVERT_Y ? 1U : 0U,
        },
        .process_coordinates = touch_process_coordinates,
        .driver_data = &s_gt911_cfg,
    };

    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(s_touch_io, &tp_cfg, &s_touch), TAG,
                        "GT911 init failed");

#if CT_TOUCH_USE_INT
    if (CT_TOUCH_INT_GPIO >= 0) {
        gpio_config_t io_conf = {
            .pin_bit_mask = 1ULL << CT_TOUCH_INT_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .pull_up_en = CT_TOUCH_INT_PULLUP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE
        };
        gpio_config(&io_conf);

        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "GPIO ISR service init failed: %d", err);
        }
        gpio_isr_handler_add(CT_TOUCH_INT_GPIO, touch_isr_handler, NULL);
    }
#endif

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    if (!s_indev) {
        ESP_LOGE(TAG, "LVGL indev register failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GT911 touch initialized (esp_lcd_touch)");
    return ESP_OK;
}

void touch_driver_get_state(bool *pressed, int16_t *x, int16_t *y)
{
    if (pressed) {
        *pressed = s_touched;
    }
    if (x) {
        *x = (int16_t)s_point.x;
    }
    if (y) {
        *y = (int16_t)s_point.y;
    }
}
