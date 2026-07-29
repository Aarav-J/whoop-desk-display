#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Waveshare 1.69" LCD Module — ST7789V2, 240×280 SPI
#define DISPLAY_H_RES  240
#define DISPLAY_V_RES  280

esp_err_t display_init(void);

// Update live heart-rate UI. hr_bpm=0 or contact=false → "—" / off-body.
void display_set_hr(uint16_t hr_bpm, bool contact);

// Optional recovery score from API (0–100). Pass 255 to hide.
void display_set_recovery(uint8_t recovery_score);

#ifdef __cplusplus
}
#endif
