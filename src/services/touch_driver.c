#include "services/touch_driver.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
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
static lv_point_t s_point = {0};
static uint16_t s_touch_max_x = CT_LCD_H_RES;
static uint16_t s_touch_max_y = CT_LCD_V_RES;
static volatile bool s_int_pending = false;
static int64_t s_last_read_us = 0;
static esp_lcd_panel_io_handle_t s_touch_io = NULL;
static esp_lcd_touch_handle_t s_touch = NULL;
static esp_lcd_touch_io_gt911_config_t s_gt911_cfg = {0};
static lv_point_t s_raw_point = {0};

static touch_calibration_t s_default_calibration = {
    .cal_x_min = CT_TOUCH_CAL_X_MIN,
    .cal_x_max = CT_TOUCH_CAL_X_MAX,
    .cal_y_min = CT_TOUCH_CAL_Y_MIN,
    .cal_y_max = CT_TOUCH_CAL_Y_MAX,
    .offset_x = CT_TOUCH_OFFSET_X,
    .offset_y = CT_TOUCH_OFFSET_Y,
};

static touch_calibration_t s_runtime_calibration = {
    .cal_x_min = CT_TOUCH_CAL_X_MIN,
    .cal_x_max = CT_TOUCH_CAL_X_MAX,
    .cal_y_min = CT_TOUCH_CAL_Y_MIN,
    .cal_y_max = CT_TOUCH_CAL_Y_MAX,
    .offset_x = CT_TOUCH_OFFSET_X,
    .offset_y = CT_TOUCH_OFFSET_Y,
};

#define NVS_NAMESPACE "ct"
#define NVS_KEY_TOUCH_VALID "touch_cal_valid"
#define NVS_KEY_TOUCH_X_MIN "touch_cal_xmin"
#define NVS_KEY_TOUCH_X_MAX "touch_cal_xmax"
#define NVS_KEY_TOUCH_Y_MIN "touch_cal_ymin"
#define NVS_KEY_TOUCH_Y_MAX "touch_cal_ymax"
#define NVS_KEY_TOUCH_OFS_X "touch_ofs_x"
#define NVS_KEY_TOUCH_OFS_Y "touch_ofs_y"

#define NVS_KEY_DEF_VALID "touch_def_valid"
#define NVS_KEY_DEF_X_MIN "touch_def_xmin"
#define NVS_KEY_DEF_X_MAX "touch_def_xmax"
#define NVS_KEY_DEF_Y_MIN "touch_def_ymin"
#define NVS_KEY_DEF_Y_MAX "touch_def_ymax"
#define NVS_KEY_DEF_OFS_X "touch_def_ofsx"
#define NVS_KEY_DEF_OFS_Y "touch_def_ofsy"

static bool touch_calibration_is_valid(const touch_calibration_t *cal)
{
    if (!cal) {
        return false;
    }

    int32_t span_x = cal->cal_x_max - cal->cal_x_min;
    int32_t span_y = cal->cal_y_max - cal->cal_y_min;

    if (span_x <= 0 || span_y <= 0) {
        return false;
    }

    if (span_x < 20 || span_y < 20) {
        return false;
    }

    const int32_t phys_w = CT_TOUCH_PHYS_H_RES;
    const int32_t phys_h = CT_TOUCH_PHYS_V_RES;

    const int32_t min_span_x = (phys_w * 45) / 100;
    const int32_t max_span_x = (phys_w * 160) / 100;
    const int32_t min_span_y = (phys_h * 45) / 100;
    const int32_t max_span_y = (phys_h * 160) / 100;

    if (span_x < min_span_x || span_x > max_span_x || span_y < min_span_y || span_y > max_span_y) {
        return false;
    }

    if (cal->cal_x_min < -phys_w || cal->cal_x_min > phys_w ||
        cal->cal_x_max < 0 || cal->cal_x_max > (phys_w * 2) ||
        cal->cal_y_min < -phys_h || cal->cal_y_min > phys_h ||
        cal->cal_y_max < 0 || cal->cal_y_max > (phys_h * 2)) {
        return false;
    }

    if (cal->offset_x < -120 || cal->offset_x > 120 ||
        cal->offset_y < -120 || cal->offset_y > 120) {
        return false;
    }

    return true;
}

static esp_err_t touch_open_nvs(nvs_handle_t *handle)
{
    return nvs_open(NVS_NAMESPACE, NVS_READWRITE, handle);
}

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

    if (s_runtime_calibration.cal_x_max > s_runtime_calibration.cal_x_min) {
        tx = (tx - s_runtime_calibration.cal_x_min) * (phys_h - 1) /
             (s_runtime_calibration.cal_x_max - s_runtime_calibration.cal_x_min);
    }
    if (s_runtime_calibration.cal_y_max > s_runtime_calibration.cal_y_min) {
        ty = (ty - s_runtime_calibration.cal_y_min) * (phys_v - 1) /
             (s_runtime_calibration.cal_y_max - s_runtime_calibration.cal_y_min);
    }

    tx += s_runtime_calibration.offset_x;
    ty += s_runtime_calibration.offset_y;

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
        if (i == 0) {
            s_raw_point.x = (lv_coord_t)x[i];
            s_raw_point.y = (lv_coord_t)y[i];
        }
        apply_transform(&x[i], &y[i]);
    }
}

esp_err_t touch_driver_load_calibration(bool *loaded)
{
    if (loaded) {
        *loaded = false;
    }

    nvs_handle_t handle;
    esp_err_t err = touch_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(handle, NVS_KEY_TOUCH_VALID, &valid);
    if (err != ESP_OK || valid == 0) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    touch_calibration_t cal = s_runtime_calibration;
    if ((err = nvs_get_i32(handle, NVS_KEY_TOUCH_X_MIN, &cal.cal_x_min)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_TOUCH_X_MAX, &cal.cal_x_max)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_TOUCH_Y_MIN, &cal.cal_y_min)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_TOUCH_Y_MAX, &cal.cal_y_max)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_TOUCH_OFS_X, &cal.offset_x)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_TOUCH_OFS_Y, &cal.offset_y)) != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    if (!touch_calibration_is_valid(&cal)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_runtime_calibration = cal;
    if (loaded) {
        *loaded = true;
    }
    ESP_LOGI(TAG, "Touch calibration loaded: x[%ld..%ld] y[%ld..%ld] ofs(%ld,%ld)",
             (long)cal.cal_x_min, (long)cal.cal_x_max,
             (long)cal.cal_y_min, (long)cal.cal_y_max,
             (long)cal.offset_x, (long)cal.offset_y);
    return ESP_OK;
}

esp_err_t touch_driver_save_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = touch_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    const touch_calibration_t *cal = &s_runtime_calibration;
    err = nvs_set_i32(handle, NVS_KEY_TOUCH_X_MIN, cal->cal_x_min);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_TOUCH_X_MAX, cal->cal_x_max);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_TOUCH_Y_MIN, cal->cal_y_min);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_TOUCH_Y_MAX, cal->cal_y_max);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_TOUCH_OFS_X, cal->offset_x);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_TOUCH_OFS_Y, cal->offset_y);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_KEY_TOUCH_VALID, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    return err;
}

esp_err_t touch_driver_set_default_calibration(const touch_calibration_t *calibration)
{
    if (!touch_calibration_is_valid(calibration)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_default_calibration = *calibration;
    return ESP_OK;
}

esp_err_t touch_driver_save_default_calibration(void)
{
    nvs_handle_t handle;
    esp_err_t err = touch_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    const touch_calibration_t *cal = &s_default_calibration;
    err = nvs_set_i32(handle, NVS_KEY_DEF_X_MIN, cal->cal_x_min);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_DEF_X_MAX, cal->cal_x_max);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_DEF_Y_MIN, cal->cal_y_min);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_DEF_Y_MAX, cal->cal_y_max);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_DEF_OFS_X, cal->offset_x);
    if (err == ESP_OK) err = nvs_set_i32(handle, NVS_KEY_DEF_OFS_Y, cal->offset_y);
    if (err == ESP_OK) err = nvs_set_u8(handle, NVS_KEY_DEF_VALID, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    return err;
}

esp_err_t touch_driver_promote_current_to_default(void)
{
    s_default_calibration = s_runtime_calibration;
    if (!touch_calibration_is_valid(&s_default_calibration)) {
        return ESP_ERR_INVALID_STATE;
    }
    return touch_driver_save_default_calibration();
}

static esp_err_t touch_driver_load_default_calibration(bool *loaded)
{
    if (loaded) {
        *loaded = false;
    }

    nvs_handle_t handle;
    esp_err_t err = touch_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t valid = 0;
    err = nvs_get_u8(handle, NVS_KEY_DEF_VALID, &valid);
    if (err != ESP_OK || valid == 0) {
        nvs_close(handle);
        return ESP_ERR_NOT_FOUND;
    }

    touch_calibration_t cal = s_default_calibration;
    if ((err = nvs_get_i32(handle, NVS_KEY_DEF_X_MIN, &cal.cal_x_min)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_DEF_X_MAX, &cal.cal_x_max)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_DEF_Y_MIN, &cal.cal_y_min)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_DEF_Y_MAX, &cal.cal_y_max)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_DEF_OFS_X, &cal.offset_x)) != ESP_OK ||
        (err = nvs_get_i32(handle, NVS_KEY_DEF_OFS_Y, &cal.offset_y)) != ESP_OK) {
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);

    if (!touch_calibration_is_valid(&cal)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    s_default_calibration = cal;
    if (loaded) {
        *loaded = true;
    }

    ESP_LOGI(TAG, "Touch default calibration loaded: x[%ld..%ld] y[%ld..%ld] ofs(%ld,%ld)",
             (long)cal.cal_x_min, (long)cal.cal_x_max,
             (long)cal.cal_y_min, (long)cal.cal_y_max,
             (long)cal.offset_x, (long)cal.offset_y);
    return ESP_OK;
}

esp_err_t touch_driver_discard_calibration(void)
{
    touch_driver_reset_calibration_defaults();

    nvs_handle_t handle;
    esp_err_t err = touch_open_nvs(&handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, NVS_KEY_TOUCH_VALID, 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

void touch_driver_get_calibration(touch_calibration_t *out)
{
    if (!out) {
        return;
    }
    *out = s_runtime_calibration;
}

void touch_driver_get_default_calibration(touch_calibration_t *out)
{
    if (!out) {
        return;
    }
    *out = s_default_calibration;
}

esp_err_t touch_driver_set_calibration(const touch_calibration_t *calibration)
{
    if (!touch_calibration_is_valid(calibration)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_runtime_calibration = *calibration;
    return ESP_OK;
}

void touch_driver_reset_calibration_defaults(void)
{
    s_runtime_calibration = s_default_calibration;
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
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    esp_lcd_touch_point_data_t touch_data[1] = {0};
    uint8_t touch_cnt = 0;
    err = esp_lcd_touch_get_data(s_touch, touch_data, &touch_cnt, 1);
    if (err != ESP_OK) {
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

    data->state = s_touched ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->point = s_point;

}

#if CT_TOUCH_USE_INT
static void IRAM_ATTR touch_isr_handler(void *arg)
{
    (void)arg;
    s_int_pending = true;
}
#endif

esp_err_t touch_driver_init(void)
{
#if !CT_TOUCH_ENABLED
    // Touch disabled by config
    return ESP_OK;
#endif
    bool defaults_loaded = false;
    touch_driver_load_default_calibration(&defaults_loaded);

    if (CT_TOUCH_FORCE_DEFAULTS) {
        touch_driver_reset_calibration_defaults();
    } else {
        bool loaded = false;
        touch_driver_load_calibration(&loaded);
        if (!loaded) {
            touch_driver_reset_calibration_defaults();
        }
    }

    // Touch config
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
            // GPIO ISR service init failed: err
        }
        gpio_isr_handler_add(CT_TOUCH_INT_GPIO, touch_isr_handler, NULL);
    }
#endif

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    s_indev = lv_indev_drv_register(&s_indev_drv);
    if (!s_indev) {
        // LVGL indev register failed
        return ESP_FAIL;
    }

    // GT911 touch initialized
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

void touch_driver_get_raw_state(bool *pressed, int16_t *x, int16_t *y)
{
    if (pressed) {
        *pressed = s_touched;
    }
    if (x) {
        *x = (int16_t)s_raw_point.x;
    }
    if (y) {
        *y = (int16_t)s_raw_point.y;
    }
}
