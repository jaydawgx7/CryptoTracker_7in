#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifndef CT_LVGL_ENABLE
#define CT_LVGL_ENABLE 1
#endif

#define CT_LCD_H_RES 800
#define CT_LCD_V_RES 480

#define CT_LCD_PROFILE_ADVANCE 0
#define CT_LCD_PROFILE_WZ8048C070 1
#define CT_LCD_PROFILE CT_LCD_PROFILE_ADVANCE

#if CT_LCD_PROFILE == CT_LCD_PROFILE_ADVANCE
#define CT_LCD_PIN_PCLK 39
#define CT_LCD_PIN_HSYNC 40
#define CT_LCD_PIN_VSYNC 41
#define CT_LCD_PIN_DE 42

#define CT_LCD_PIN_D0 21
#define CT_LCD_PIN_D1 47
#define CT_LCD_PIN_D2 48
#define CT_LCD_PIN_D3 45
#define CT_LCD_PIN_D4 38
#define CT_LCD_PIN_D5 9
#define CT_LCD_PIN_D6 10
#define CT_LCD_PIN_D7 11
#define CT_LCD_PIN_D8 12
#define CT_LCD_PIN_D9 13
#define CT_LCD_PIN_D10 14
#define CT_LCD_PIN_D11 7
#define CT_LCD_PIN_D12 17
#define CT_LCD_PIN_D13 18
#define CT_LCD_PIN_D14 3
#define CT_LCD_PIN_D15 46

// Stable timing configuration for CrowPanel Advance 7" (800x480)
// Keep this aligned with the actual boot-time panel config shown in Settings.
#define CT_LCD_PCLK_HZ 13000000
#define CT_LCD_HSYNC_PW 4
#define CT_LCD_HSYNC_BP 40
#define CT_LCD_HSYNC_FP 40
#define CT_LCD_VSYNC_PW 10
#define CT_LCD_VSYNC_BP 30
#define CT_LCD_VSYNC_FP 1
#define CT_LCD_PCLK_ACTIVE_NEG 1
#define CT_LCD_PCLK_IDLE_HIGH 1
#define CT_LCD_FB_IN_PSRAM 1

#else
#define CT_LCD_PIN_PCLK 0
#define CT_LCD_PIN_HSYNC 39
#define CT_LCD_PIN_VSYNC 40
#define CT_LCD_PIN_DE 41

#define CT_LCD_PIN_D0 15
#define CT_LCD_PIN_D1 7
#define CT_LCD_PIN_D2 6
#define CT_LCD_PIN_D3 5
#define CT_LCD_PIN_D4 4
#define CT_LCD_PIN_D5 9
#define CT_LCD_PIN_D6 46
#define CT_LCD_PIN_D7 3
#define CT_LCD_PIN_D8 8
#define CT_LCD_PIN_D9 16
#define CT_LCD_PIN_D10 1
#define CT_LCD_PIN_D11 14
#define CT_LCD_PIN_D12 21
#define CT_LCD_PIN_D13 47
#define CT_LCD_PIN_D14 48
#define CT_LCD_PIN_D15 45

#define CT_LCD_PCLK_HZ 12000000
#define CT_LCD_HSYNC_PW 48
#define CT_LCD_HSYNC_BP 40
#define CT_LCD_HSYNC_FP 40
#define CT_LCD_VSYNC_PW 31
#define CT_LCD_VSYNC_BP 13
#define CT_LCD_VSYNC_FP 1
#define CT_LCD_PCLK_ACTIVE_NEG 1
#define CT_LCD_PCLK_IDLE_HIGH 1
#define CT_LCD_FB_IN_PSRAM 1
#endif

#ifndef CT_LCD_X_OFFSET
#define CT_LCD_X_OFFSET 0
#endif

#ifndef CT_LCD_Y_OFFSET
#define CT_LCD_Y_OFFSET 0
#endif

esp_err_t display_driver_init(void);
void display_driver_start(void);
bool display_driver_lock(uint32_t timeout_ms);
void display_driver_unlock(void);
void display_driver_resume_refresh(void);
bool display_driver_capture_screenshot(void);
