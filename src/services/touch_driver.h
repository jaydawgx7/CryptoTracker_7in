#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef CT_TOUCH_I2C_ADDR
#define CT_TOUCH_I2C_ADDR 0x5D
#endif

#ifndef CT_TOUCH_ENABLED
#define CT_TOUCH_ENABLED 1
#endif

#ifndef CT_TOUCH_INT_GPIO
#define CT_TOUCH_INT_GPIO -1
#endif

#ifndef CT_TOUCH_RESET_GPIO
#define CT_TOUCH_RESET_GPIO -1
#endif

#ifndef CT_TOUCH_SWAP_XY
#define CT_TOUCH_SWAP_XY 0
#endif

#ifndef CT_TOUCH_INVERT_X
#define CT_TOUCH_INVERT_X 0
#endif

#ifndef CT_TOUCH_INVERT_Y
#define CT_TOUCH_INVERT_Y 0
#endif

#ifndef CT_TOUCH_USE_INT
#define CT_TOUCH_USE_INT 0
#endif

#ifndef CT_TOUCH_INT_PULLUP
#define CT_TOUCH_INT_PULLUP 1
#endif

#ifndef CT_TOUCH_POLL_FALLBACK_MS
#define CT_TOUCH_POLL_FALLBACK_MS 50
#endif

#ifndef CT_TOUCH_LOG
#define CT_TOUCH_LOG 0
#endif

#ifndef CT_TOUCH_REG_ADDR_SWAP
#define CT_TOUCH_REG_ADDR_SWAP 0
#endif

#ifndef CT_TOUCH_CAL_X_MIN
#define CT_TOUCH_CAL_X_MIN 0
#endif

#ifndef CT_TOUCH_CAL_X_MAX
#define CT_TOUCH_CAL_X_MAX CT_LCD_H_RES
#endif

#ifndef CT_TOUCH_CAL_Y_MIN
#define CT_TOUCH_CAL_Y_MIN 0
#endif

#ifndef CT_TOUCH_CAL_Y_MAX
#define CT_TOUCH_CAL_Y_MAX CT_LCD_V_RES
#endif

#ifndef CT_TOUCH_OFFSET_X
#define CT_TOUCH_OFFSET_X 0
#endif

#ifndef CT_TOUCH_OFFSET_Y
#define CT_TOUCH_OFFSET_Y 0
#endif

#ifndef CT_TOUCH_TARGET_H_RES
#define CT_TOUCH_TARGET_H_RES CT_LCD_H_RES
#endif

#ifndef CT_TOUCH_TARGET_V_RES
#define CT_TOUCH_TARGET_V_RES CT_LCD_V_RES
#endif

#ifndef CT_TOUCH_PHYS_H_RES
#define CT_TOUCH_PHYS_H_RES CT_LCD_H_RES
#endif

#ifndef CT_TOUCH_PHYS_V_RES
#define CT_TOUCH_PHYS_V_RES CT_LCD_V_RES
#endif

esp_err_t touch_driver_init(void);
void touch_driver_get_state(bool *pressed, int16_t *x, int16_t *y);
