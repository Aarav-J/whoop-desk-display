#include "display.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include <stdio.h>

#define TAG "display"

#define LCD_HOST              SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ    (40 * 1000 * 1000)
#define LCD_CMD_BITS          8
#define LCD_PARAM_BITS        8

// 240×280 panel sits in a 240×320 GRAM — 20-row vertical gap (Waveshare / ST7789V2)
#define LCD_X_GAP             0
#define LCD_Y_GAP             20

#define PIN_SCLK              CONFIG_WHOOP_LCD_PIN_SCLK
#define PIN_MOSI              CONFIG_WHOOP_LCD_PIN_MOSI
#define PIN_CS                CONFIG_WHOOP_LCD_PIN_CS
#define PIN_DC                CONFIG_WHOOP_LCD_PIN_DC
#define PIN_RST               CONFIG_WHOOP_LCD_PIN_RST
#define PIN_BL                CONFIG_WHOOP_LCD_PIN_BL

static lv_display_t *g_disp;
static lv_obj_t *g_hr_label;
static lv_obj_t *g_bpm_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_recovery_label;

static esp_err_t backlight_on(void)
{
    gpio_config_t bk = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << PIN_BL,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk), TAG, "BL gpio");
    // Waveshare 1.69" BL is active-high
    gpio_set_level(PIN_BL, 1);
    return ESP_OK;
}

static void ui_create(void)
{
    lv_obj_t *scr = lv_display_get_screen_active(g_disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0A0A), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    g_status_label = lv_label_create(scr);
    lv_label_set_text(g_status_label, "SCANNING…");
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(g_status_label, LV_ALIGN_TOP_MID, 0, 28);

    g_hr_label = lv_label_create(scr);
    lv_label_set_text(g_hr_label, "--");
    lv_obj_set_style_text_color(g_hr_label, lv_color_hex(0xFF3B30), 0);
    lv_obj_set_style_text_font(g_hr_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_hr_label, LV_ALIGN_CENTER, 0, -8);

    g_bpm_label = lv_label_create(scr);
    lv_label_set_text(g_bpm_label, "bpm");
    lv_obj_set_style_text_color(g_bpm_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(g_bpm_label, &lv_font_montserrat_18, 0);
    lv_obj_align(g_bpm_label, LV_ALIGN_CENTER, 0, 48);

    g_recovery_label = lv_label_create(scr);
    lv_label_set_text(g_recovery_label, "");
    lv_obj_set_style_text_color(g_recovery_label, lv_color_hex(0x34C759), 0);
    lv_obj_set_style_text_font(g_recovery_label, &lv_font_montserrat_14, 0);
    lv_obj_align(g_recovery_label, LV_ALIGN_BOTTOM_MID, 0, -24);
}

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Init Waveshare 1.69\" ST7789V2 (%dx%d)", DISPLAY_H_RES, DISPLAY_V_RES);

    ESP_RETURN_ON_ERROR(backlight_on(), TAG, "backlight");

    const spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_H_RES * 40 * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle),
                        TAG, "panel io");

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle),
                        TAG, "st7789");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "init");
    // IPS ST7789 panels (Waveshare 1.69) need color invert + GRAM gap
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel_handle, LCD_X_GAP, LCD_Y_GAP), TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_handle, false, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "disp on");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl port");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = DISPLAY_H_RES * 40,
        .double_buffer = true,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .swap_bytes = true,  // SPI ST7789 expects big-endian RGB565
        },
    };
    g_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(g_disp != NULL, ESP_FAIL, TAG, "add disp");

    if (lvgl_port_lock(0)) {
        ui_create();
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display ready (SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d)",
             PIN_SCLK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BL);
    return ESP_OK;
}

void display_set_hr(uint16_t hr_bpm, bool contact)
{
    if (!g_disp || !g_hr_label) {
        return;
    }

    if (!lvgl_port_lock(50)) {
        return;
    }

    if (contact && hr_bpm > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)hr_bpm);
        lv_label_set_text(g_hr_label, buf);
        lv_obj_set_style_text_color(g_hr_label, lv_color_hex(0xFF3B30), 0);
        lv_label_set_text(g_status_label, "HEART RATE");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x888888), 0);
    } else {
        lv_label_set_text(g_hr_label, "--");
        lv_obj_set_style_text_color(g_hr_label, lv_color_hex(0x555555), 0);
        lv_label_set_text(g_status_label, contact ? "NO SIGNAL" : "OFF BODY");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x666666), 0);
    }

    lvgl_port_unlock();
}

void display_set_recovery(uint8_t recovery_score)
{
    if (!g_disp || !g_recovery_label) {
        return;
    }

    if (!lvgl_port_lock(50)) {
        return;
    }

    if (recovery_score <= 100) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Recovery  %u%%", (unsigned)recovery_score);
        lv_label_set_text(g_recovery_label, buf);
        uint32_t color = recovery_score >= 67 ? 0x34C759
                       : recovery_score >= 34 ? 0xFFCC00
                       : 0xFF3B30;
        lv_obj_set_style_text_color(g_recovery_label, lv_color_hex(color), 0);
    } else {
        lv_label_set_text(g_recovery_label, "");
    }

    lvgl_port_unlock();
}
