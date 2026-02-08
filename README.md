# CryptoTracker

ESP-IDF + LVGL crypto portfolio tracker for the Elecrow CrowPanel Advance 7.0" ESP32-S3 (PCB V1.3).

## Build and Flash (PlatformIO)

1. Open the project in VS Code with PlatformIO installed.
2. Set the correct serial port in PlatformIO if needed.
3. Build and upload:

```
pio run
pio run -t upload
pio device monitor -b 115200
```

## First Run Flow

- Boot to the home screen.
- Open Settings to confirm the I2C scan shows `0x30`, `0x51`, and `0x5D`.
- Connect WiFi (UI stub currently).
- Add BTC and XRP to the watchlist (default watchlist to be wired in NVS).

## Hardware Notes

- I2C uses GPIO15 (SDA) and GPIO16 (SCL).
- Touch controller is GT911-class on address `0x5D` with INT on GPIO1.
- Brightness, speaker enable, and buzzer are controlled by the V1.3 control MCU at `0x30`.

## Display + Touch (Working Configuration)

This project uses the ESP-IDF RGB panel driver with a tuned timing profile for the CrowPanel Advance 7". LVGL uses a vertical offset to compensate for panel alignment. Touch input uses the ESP `esp_lcd_touch_gt911` component with runtime calibration.

### Display
- Panel: 800x480, RGB interface (`esp_lcd_panel_rgb`).
- Pixel clock: 21 MHz with PLL240M source.
- Bounce buffer: enabled (10 * H_RES) to avoid rolling.
- LVGL vertical compensation: `ver_res = 480 + 40`, `offset_y = -40`.

### Touch
- Driver: `esp_lcd_touch` + `esp_lcd_touch_gt911` (vendored under `components/`).
- I2C address: 0x5D, INT: GPIO1.
- INT wake pulse applied before touch init to bring the controller online.
- Register address byte swap: disabled for the `esp_lcd_panel_io_i2c` path.
- Touch mapping uses separate physical vs. LVGL target heights to handle the LVGL vertical offset.

### Calibration Flags (platformio.ini)
- `CT_TOUCH_CAL_X_MIN`, `CT_TOUCH_CAL_X_MAX`
- `CT_TOUCH_CAL_Y_MIN`, `CT_TOUCH_CAL_Y_MAX`
- `CT_TOUCH_OFFSET_Y`
- `CT_TOUCH_PHYS_V_RES=480`
- `CT_TOUCH_TARGET_V_RES=520`

If touch feels vertically stretched, adjust `CT_TOUCH_CAL_Y_MAX` by small increments. If the entire touch map is shifted, adjust `CT_TOUCH_OFFSET_Y`.

## TODO

- Replace RGB panel pin mapping and timing constants in `display_driver.h`.
- Implement GT911 touch driver and optional reset GPIO.
- Complete WiFi manager, CoinGecko client, NVS persistence, and alert handling.
- Build the remaining UI screens (watchlist, coin detail, add coin, alerts).
