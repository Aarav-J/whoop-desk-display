#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "whoop_api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Waveshare 1.69" LCD Module — ST7789V2, 240×280 SPI
#define DISPLAY_H_RES  240
#define DISPLAY_V_RES  280

esp_err_t display_init(void);

// Live heart rate from BLE. hr_bpm=0 or contact=false → off-body indicator.
void display_set_hr(uint16_t hr_bpm, bool contact);

// Full WHOOP snapshot from API — updates all metric cells.
void display_set_snapshot(const whoop_snapshot_t *snap);

// Mark HR as stale (no recent update). seconds_ago=0 clears the indicator.
void display_set_hr_stale(uint32_t seconds_ago);

// Wi‑Fi connection status for the status bar.
void display_set_wifi(bool connected);

#ifdef __cplusplus
}
#endif
