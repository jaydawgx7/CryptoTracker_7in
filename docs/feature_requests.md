# Feature Requests

## Touch Calibration (On-Device)

### Goal
Allow users to calibrate touch on-device by tapping guided crosshairs, then save calibration values to NVS and apply them at runtime.

### User Flow
1) Settings -> "Calibrate Touch".
2) Fullscreen calibration mode with 5 targets (top-left, top-right, bottom-right, bottom-left, center).
3) User taps each target. Show a confirmation dot and move to next target.
4) Compute calibration values and store in NVS.
5) Exit calibration, apply values immediately, and return to Settings with a success message.

### Data Collected
- Raw touch samples for each target.
- Optional: multiple samples per target (e.g., 5) and average for stability.

### Computation
- Prefer simple min/max scaling:
  - x_min/x_max from left/right targets.
  - y_min/y_max from top/bottom targets.
- Apply optional offset if needed (derived from center target vs expected center).
- If needed later, allow affine transform (2D linear) for rotation/skew correction.

### Storage
- NVS keys:
  - touch_cal_x_min
  - touch_cal_x_max
  - touch_cal_y_min
  - touch_cal_y_max
  - touch_offset_x
  - touch_offset_y
  - touch_cal_valid (bool)

### Runtime Integration
- On boot, read calibration values from NVS.
- If valid, override compile-time defaults.
- Fallback to compile-time defaults if NVS values are missing or invalid.

### UI Notes
- Use bright high-contrast crosshairs and a short instruction label.
- Hide normal UI elements during calibration.
- Provide a cancel option to abort without saving.

### Implementation Pointers
- Add a Settings entry and handler in settings screen.
- Reuse LVGL overlay layer for calibration UI.
- Add helper in touch driver to accept runtime calibration overrides.
- Extend nvs_store with get/set helpers for calibration values.
