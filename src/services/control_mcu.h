#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define CONTROL_MCU_I2C_ADDR 0x30

#define CONTROL_MCU_CMD_TOUCH_ENABLE 250
#define CONTROL_MCU_BACKLIGHT_MAX 0
#define CONTROL_MCU_BACKLIGHT_OFF 245

esp_err_t control_mcu_init(void);
esp_err_t control_mcu_force_backlight(void);
esp_err_t control_mcu_set_brightness(uint8_t percent);
esp_err_t control_mcu_set_buzzer(bool enabled);
esp_err_t control_mcu_buzzer_beep(uint16_t duration_ms);
esp_err_t control_mcu_touch_enable(void);
