#include "services/control_mcu.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "services/i2c_bus.h"

static const char *TAG = "control_mcu";

static esp_err_t control_mcu_send_command(uint8_t command)
{
    return i2c_master_write_to_device(CT_I2C_PORT, CONTROL_MCU_I2C_ADDR, &command, 1, pdMS_TO_TICKS(500));
}

esp_err_t control_mcu_init(void)
{
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %d", err);
    }
    return err;
}

esp_err_t control_mcu_force_backlight(void)
{
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < 5; attempt++) {
        err = control_mcu_send_command(CONTROL_MCU_BACKLIGHT_MAX);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Backlight command accepted");
            return err;
        }
        ESP_LOGW(TAG, "Backlight command failed (attempt %d): %d", attempt + 1, err);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    return err;
}

esp_err_t control_mcu_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }

    uint16_t inverted = (uint16_t)CONTROL_MCU_BACKLIGHT_OFF - ((uint16_t)percent * CONTROL_MCU_BACKLIGHT_OFF / 100);
    uint8_t command = (uint8_t)inverted;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < 3; attempt++) {
        err = control_mcu_send_command(command);
        if (err == ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "Brightness write failed (attempt %d): %d", attempt + 1, err);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    return err;
}

esp_err_t control_mcu_set_buzzer(bool enabled)
{
    uint8_t command = enabled ? 0xF6 : 0xF7;
    esp_err_t err = control_mcu_send_command(command);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer write failed: %d", err);
    }

    return err;
}

esp_err_t control_mcu_buzzer_beep(uint16_t duration_ms)
{
    esp_err_t err = control_mcu_send_command(0xF6);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer start failed: %d", err);
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    err = control_mcu_send_command(0xF7);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer stop failed: %d", err);
    }

    return err;
}

esp_err_t control_mcu_touch_enable(void)
{
    esp_err_t err = control_mcu_send_command(CONTROL_MCU_CMD_TOUCH_ENABLE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Touch enable failed: %d", err);
    }

    return err;
}
