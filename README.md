# CryptoTracker

[![Build](https://img.shields.io/badge/build-manual-lightgrey)](https://github.com/jaydawgx7/CryptoTracker_7in/actions)

ESP-IDF + LVGL crypto portfolio tracker for the Elecrow CrowPanel Advance 7.0" ESP32-S3 (PCB V1.3).

## Highlights

- Dual data sources: Kraken (prices) with CoinGecko fallback + percent sync.
- Modern LVGL UI: watchlist, coin detail, add coin, alerts, settings.
- Persistent preferences (theme, sort, refresh rate, show values, brightness).
- Privacy toggle to hide portfolio values.
- Alerts with toast + optional buzzer.

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

- Boot to Home.
- Open Settings and confirm I2C scan shows `0x30`, `0x51`, `0x5D`.
- Connect WiFi (Settings -> WiFi).
- Add coins from Add Coin.

## Features

### Home
- Watchlist with sorting (Symbol, Price, 1h, 24h, 7d, Value).
- Long-press row actions: edit holdings, alerts, pin, remove.
- Show values toggle (privacy mode).

### Coin Detail
- Title with live price, holdings/value summary.
- Percent chips: 1h, 24h, 7d, 30d, 1y.
- Chart with axis labels + range buttons.

### Settings
- WiFi manager.
- Brightness slider (control MCU).
- Theme and UI preferences.
- Refresh interval.
- Buzzer test.

## Data Sources

- Kraken REST ticker provides prices and 24h change.
- CoinGecko provides percent sync (1h/24h/7d/30d/1y) and full fallback when Kraken is missing or fails.
- Dynamic Kraken pair mapping from AssetPairs for non-static symbols.

## Hardware Notes

- I2C uses GPIO15 (SDA) and GPIO16 (SCL).
- Touch controller is GT911-class on address `0x5D` with INT on GPIO1.
- Brightness and buzzer controlled by the V1.3 control MCU at `0x30`.
- V1.3 control MCU commands:
  - Brightness: 0 = max, 244 = min, 245 = off.
  - Buzzer: 246 = on, 247 = off.

## Display + Touch (Working Configuration)

This project uses the ESP-IDF RGB panel driver with a tuned timing profile for the CrowPanel Advance 7". LVGL uses a vertical offset to compensate for panel alignment. Touch input uses the ESP `esp_lcd_touch_gt911` component with runtime calibration.

### Display
- Panel: 800x480, RGB interface (`esp_lcd_panel_rgb`).
- Pixel clock: 16 MHz with PLL240M source.
- Timing: HSYNC 4/40/40, VSYNC 10/30/1, `pclk_active_neg=1`.
- Bounce buffer enabled (10 * H_RES).
- LVGL vertical compensation: `ver_res = 480 + 40`, `offset_y = -40`.

### Touch
- Driver: `esp_lcd_touch` + `esp_lcd_touch_gt911` (vendored under `components/`).
- I2C address: 0x5D, INT: GPIO1.
- INT wake pulse applied before touch init.
- Register address byte swap disabled for `esp_lcd_panel_io_i2c`.

### Calibration Flags (platformio.ini)
- `CT_TOUCH_CAL_X_MIN`, `CT_TOUCH_CAL_X_MAX`
- `CT_TOUCH_CAL_Y_MIN`, `CT_TOUCH_CAL_Y_MAX`
- `CT_TOUCH_OFFSET_Y`
- `CT_TOUCH_PHYS_V_RES=480`
- `CT_TOUCH_TARGET_V_RES=520`

If touch feels vertically stretched, adjust `CT_TOUCH_CAL_Y_MAX` by small increments. If the entire touch map is shifted, adjust `CT_TOUCH_OFFSET_Y`.
