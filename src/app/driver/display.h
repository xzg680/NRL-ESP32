#ifndef DRIVER_DISPLAY_H
#define DRIVER_DISPLAY_H

// On-device LCD UI for the 格子派 board (ST7789 240x240 SPI panel + LVGL).
// The panel/wiring matches the 小智 (xiaozhi) 格子派 board. On boards without
// a display these calls are compiled to no-ops, so callers may invoke them
// unconditionally or behind `#if NRL_HAS_DISPLAY`.

#ifdef __cplusplus
extern "C" {
#endif

// Bring up the SPI bus, ST7789 panel and LVGL, then build the UI. Safe to call
// once from setup(); a failure is logged and leaves Display_Poll() inert.
void Display_Init(void);

// Refresh the on-screen values (caller callsign/SSID, clock, WiFi signal,
// battery voltage, IP address). Call periodically from loop(); the work is
// internally throttled so a tight 20 ms poll loop is fine.
void Display_Poll(void);

// Battery sense (gezipai only; returns 0 on boards without a battery ADC).
// Both readings are in millivolts. The raw reading is the uncalibrated ADC
// voltage * 3 (divider); the calibrated reading additionally multiplies by
// the configured battery_cal_milli / 1000 correction factor.
// These take a fresh sample, so callers do not have to wait for the next
// Display_Poll() refresh.
int Display_GetBatteryRawMv(void);
int Display_GetBatteryCalibratedMv(void);

#ifdef __cplusplus
}
#endif

#endif // DRIVER_DISPLAY_H
