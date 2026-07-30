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

// ── Colour palette ──────────────────────────────────────────────────────────
#define CLR_BG         0x0A0A0A   // screen background
#define CLR_CELL_BG    0x141414   // metric card background
#define CLR_TEXT_DIM   0x666666   // labels, status bar
#define CLR_TEXT_BODY  0xCCCCCC   // secondary values
#define CLR_TEXT_BRIGHT 0xEEEEEE  // primary values
#define CLR_HR         0xFF3B30   // heart rate number
#define CLR_GREEN      0x34C759   // recovery >= 67, sleep >= 85, spo2 >= 95
#define CLR_YELLOW     0xFFCC00   // mid-range
#define CLR_RED        0xFF3B30   // low / alert
#define CLR_WIFI_ON    0x34C759   // Wi‑Fi indicator
#define CLR_WIFI_OFF   0xFF3B30
#define CLR_DIVIDER    0x1E1E1E

// ── Screen layout constants ─────────────────────────────────────────────────
#define STATUS_H       22
#define HR_TOP         26
#define HR_H           90
#define DIVIDER_Y      118
#define GRID_Y         124
#define CELL_W         116
#define CELL_H         48
#define CELL_GAP       4
#define GRID_PAD_X     ((DISPLAY_H_RES - (2 * CELL_W + CELL_GAP)) / 2)  // centred

// ── Per-cell state ──────────────────────────────────────────────────────────
typedef struct {
    lv_obj_t *container;
    lv_obj_t *title_label;
    lv_obj_t *value_label;
    lv_obj_t *unit_label;
    lv_obj_t *bar;        // colour strip
} metric_cell_t;

// ── Global LVGL objects ─────────────────────────────────────────────────────
static lv_display_t *g_disp;
static lv_obj_t *g_hr_label;
static lv_obj_t *g_bpm_label;
static lv_obj_t *g_status_left;
static lv_obj_t *g_status_wifi;
static lv_obj_t *g_stale_label;

static metric_cell_t g_cells[6];  // recovery, strain, hrv, rhr, sleep, spo2

// ── Helpers ─────────────────────────────────────────────────────────────────

static uint32_t _recovery_color(uint8_t score) {
    if (score >= 67) return CLR_GREEN;
    if (score >= 34) return CLR_YELLOW;
    return CLR_RED;
}

static uint32_t _strain_color(float strain) {
    if (strain < 10.0f) return CLR_GREEN;
    if (strain < 14.0f) return CLR_YELLOW;
    return CLR_RED;
}

static uint32_t _sleep_color(uint8_t perf) {
    if (perf >= 85) return CLR_GREEN;
    if (perf >= 70) return CLR_YELLOW;
    return CLR_RED;
}

static uint32_t _spo2_color(uint8_t spo2) {
    if (spo2 >= 95) return CLR_GREEN;
    if (spo2 >= 90) return CLR_YELLOW;
    return CLR_RED;
}

static uint32_t _rhr_color(uint8_t rhr) {
    if (rhr <= 60) return CLR_GREEN;
    if (rhr <= 80) return CLR_YELLOW;
    return CLR_RED;
}

static uint32_t _hrv_color(float hrv) {
    // No universal scale — use recovery context if available.
    // Default: >50 ms is decent, <30 ms is low.
    if (hrv > 50.0f) return CLR_GREEN;
    if (hrv > 30.0f) return CLR_YELLOW;
    return CLR_RED;
}

// ── Backlight ───────────────────────────────────────────────────────────────
static esp_err_t backlight_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(PIN_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "BL gpio config");
    ESP_RETURN_ON_ERROR(gpio_set_level(PIN_BL, 1), TAG, "BL on");
    return ESP_OK;
}

// ── Metric cell factory ─────────────────────────────────────────────────────
static void _create_cell(metric_cell_t *cell, lv_obj_t *parent,
                         int x, int y, const char *title)
{
    // Container
    cell->container = lv_obj_create(parent);
    lv_obj_set_size(cell->container, CELL_W, CELL_H);
    lv_obj_set_pos(cell->container, x, y);
    lv_obj_set_style_bg_color(cell->container, lv_color_hex(CLR_CELL_BG), 0);
    lv_obj_set_style_bg_opa(cell->container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell->container, 0, 0);
    lv_obj_set_style_radius(cell->container, 8, 0);
    lv_obj_set_style_pad_all(cell->container, 0, 0);
    lv_obj_set_scrollbar_mode(cell->container, LV_SCROLLBAR_MODE_OFF);

    // Colour strip (left edge)
    cell->bar = lv_obj_create(cell->container);
    lv_obj_set_size(cell->bar, 4, CELL_H - 8);
    lv_obj_set_pos(cell->bar, 0, 4);
    lv_obj_set_style_bg_color(cell->bar, lv_color_hex(CLR_GREEN), 0);
    lv_obj_set_style_bg_opa(cell->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cell->bar, 0, 0);
    lv_obj_set_style_radius(cell->bar, 2, 0);
    lv_obj_set_scrollbar_mode(cell->bar, LV_SCROLLBAR_MODE_OFF);

    // Title
    cell->title_label = lv_label_create(cell->container);
    lv_label_set_text(cell->title_label, title);
    lv_obj_set_style_text_color(cell->title_label, lv_color_hex(CLR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(cell->title_label, &lv_font_montserrat_12, 0);
    lv_obj_set_pos(cell->title_label, 10, 4);

    // Value
    cell->value_label = lv_label_create(cell->container);
    lv_label_set_text(cell->value_label, "--");
    lv_obj_set_style_text_color(cell->value_label, lv_color_hex(CLR_TEXT_BRIGHT), 0);
    lv_obj_set_style_text_font(cell->value_label, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(cell->value_label, 10, 21);

    // Unit (inline, placed to the right of value — repositioned on update)
    cell->unit_label = lv_label_create(cell->container);
    lv_obj_set_style_text_color(cell->unit_label, lv_color_hex(CLR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(cell->unit_label, &lv_font_montserrat_14, 0);
}

// ── Update helpers ──────────────────────────────────────────────────────────
static void _set_cell_value(metric_cell_t *cell, const char *value,
                            const char *unit, uint32_t bar_color)
{
    if (!lvgl_port_lock(50)) return;
    lv_label_set_text(cell->value_label, value);
    lv_label_set_text(cell->unit_label, unit);
    lv_obj_set_style_bg_color(cell->bar, lv_color_hex(bar_color), 0);

    // Reposition unit label to the right of the value
    lv_obj_update_layout(cell->value_label);
    int vw = lv_obj_get_width(cell->value_label);
    lv_obj_set_pos(cell->unit_label, 10 + vw + 4, 25);
    lvgl_port_unlock();
}

// ── LVGL initialisation ─────────────────────────────────────────────────────
static void ui_create(void)
{
    lv_obj_t *scr = lv_display_get_screen_active(g_disp);
    lv_obj_set_style_bg_color(scr, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    // ── Status bar ──────────────────────────────────────────────────────
    g_status_left = lv_label_create(scr);
    lv_label_set_text(g_status_left, "WHOOP");
    lv_obj_set_style_text_color(g_status_left, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(g_status_left, &lv_font_montserrat_12, 0);
    lv_obj_align(g_status_left, LV_ALIGN_TOP_LEFT, 6, 4);

    g_status_wifi = lv_label_create(scr);
    lv_label_set_text(g_status_wifi, "");
    lv_obj_set_style_text_color(g_status_wifi, lv_color_hex(CLR_WIFI_OFF), 0);
    lv_obj_set_style_text_font(g_status_wifi, &lv_font_montserrat_12, 0);
    lv_obj_align(g_status_wifi, LV_ALIGN_TOP_RIGHT, -6, 4);

    // ── HR section ───────────────────────────────────────────────────────
    g_hr_label = lv_label_create(scr);
    lv_label_set_text(g_hr_label, "--");
    lv_obj_set_style_text_color(g_hr_label, lv_color_hex(CLR_HR), 0);
    lv_obj_set_style_text_font(g_hr_label, &lv_font_montserrat_48, 0);
    lv_obj_align(g_hr_label, LV_ALIGN_TOP_MID, 0, HR_TOP + 2);

    g_bpm_label = lv_label_create(scr);
    lv_label_set_text(g_bpm_label, "bpm");
    lv_obj_set_style_text_color(g_bpm_label, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(g_bpm_label, &lv_font_montserrat_16, 0);
    lv_obj_align(g_bpm_label, LV_ALIGN_TOP_MID, 0, HR_TOP + 54);

    // ── Divider line ─────────────────────────────────────────────────────
    lv_obj_t *div = lv_obj_create(scr);
    lv_obj_set_size(div, DISPLAY_H_RES - 8, 1);
    lv_obj_set_pos(div, 4, DIVIDER_Y);
    lv_obj_set_style_bg_color(div, lv_color_hex(CLR_DIVIDER), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_scrollbar_mode(div, LV_SCROLLBAR_MODE_OFF);

    // ── Metric grid (3 rows × 2 columns) ─────────────────────────────────
    const int x0 = GRID_PAD_X;
    const int x1 = GRID_PAD_X + CELL_W + CELL_GAP;
    const int row_h = CELL_H + CELL_GAP;

    const char *titles[] = {"RECOVERY", "STRAIN", "HRV", "RHR",
                            "SLEEP", "SpO2"};
    const int xs[] = {x0, x1, x0, x1, x0, x1};
    const int ys[] = {GRID_Y, GRID_Y,
                      GRID_Y + row_h, GRID_Y + row_h,
                      GRID_Y + row_h * 2, GRID_Y + row_h * 2};

    for (int i = 0; i < 6; i++) {
        _create_cell(&g_cells[i], scr, xs[i], ys[i], titles[i]);
    }

    // Stale HR indicator (hidden until needed)
    g_stale_label = lv_label_create(scr);
    lv_label_set_text(g_stale_label, "");
    lv_obj_set_style_text_color(g_stale_label, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(g_stale_label, &lv_font_montserrat_12, 0);
    lv_obj_align(g_stale_label, LV_ALIGN_BOTTOM_MID, 0, -4);
}

// ── Display init ────────────────────────────────────────────────────────────
esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Init ST7789V2 240×280 SPI + LVGL");

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = PIN_SCLK,
        .mosi_io_num     = PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISPLAY_H_RES * DISPLAY_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI init");

    // Panel IO
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = PIN_CS,
        .dc_gpio_num       = PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
    };
    esp_lcd_panel_io_handle_t io_handle;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                        &io_cfg, &io_handle), TAG, "panel IO");

    // Panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel_handle;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle),
                        TAG, "panel new");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel_handle, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(panel_handle, true, false), TAG, "mirror");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel_handle, LCD_X_GAP, LCD_Y_GAP),
                        TAG, "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel_handle, true), TAG, "disp on");

    // LVGL — match official esp_lvgl_port ST7789 example
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_LOGI(TAG, "Calling lvgl_port_init...");
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init");
    ESP_LOGI(TAG, "lvgl_port_init OK");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = io_handle,
        .panel_handle = panel_handle,
        .buffer_size  = DISPLAY_H_RES * 50,
        .double_buffer = true,
        .hres         = DISPLAY_H_RES,
        .vres         = DISPLAY_V_RES,
        .monochrome   = false,
        .rotation = {
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,
        },
    };
    ESP_LOGI(TAG, "Calling lvgl_port_add_disp...");
    g_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_LOGI(TAG, "lvgl_port_add_disp returned %p", (void*)g_disp);
    if (!g_disp) { ESP_LOGE(TAG, "LVGL port add failed"); return ESP_FAIL; }

    ESP_LOGI(TAG, "Turning on backlight...");
    ESP_RETURN_ON_ERROR(backlight_on(), TAG, "backlight");
    ESP_LOGI(TAG, "Creating UI...");
    ui_create();
    ESP_LOGI(TAG, "Display ready");
    return ESP_OK;
}

// ── Public API ──────────────────────────────────────────────────────────────

void display_set_hr(uint16_t hr_bpm, bool contact)
{
    if (!g_disp || !g_hr_label) return;
    if (!lvgl_port_lock(50)) return;

    if (contact && hr_bpm > 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", (unsigned)hr_bpm);
        lv_label_set_text(g_hr_label, buf);
        lv_obj_set_style_text_color(g_hr_label, lv_color_hex(CLR_HR), 0);
    } else {
        lv_label_set_text(g_hr_label, "--");
        lv_obj_set_style_text_color(g_hr_label, lv_color_hex(0x444444), 0);
    }
    lvgl_port_unlock();
}

void display_set_snapshot(const whoop_snapshot_t *snap)
{
    if (!g_disp) return;

    char val_buf[24];

    // Recovery
    if (snap->recovery_score <= 100) {
        snprintf(val_buf, sizeof(val_buf), "%u%%", (unsigned)snap->recovery_score);
        _set_cell_value(&g_cells[0], val_buf, "",
                        _recovery_color(snap->recovery_score));
    }

    // Strain
    snprintf(val_buf, sizeof(val_buf), "%.1f", (double)snap->strain);
    _set_cell_value(&g_cells[1], val_buf, "",
                    _strain_color(snap->strain));

    // HRV
    snprintf(val_buf, sizeof(val_buf), "%.0f", (double)snap->hrv_ms);
    _set_cell_value(&g_cells[2], val_buf, "ms",
                    _hrv_color(snap->hrv_ms));

    // RHR
    snprintf(val_buf, sizeof(val_buf), "%u", (unsigned)snap->resting_hr);
    _set_cell_value(&g_cells[3], val_buf, "bpm",
                    _rhr_color(snap->resting_hr));

    // Sleep (perf + debt on one line)
    if (snap->sleep_perf <= 100) {
        if (snap->sleep_debt_min != 0) {
            snprintf(val_buf, sizeof(val_buf), "%u%% %+d",
                     (unsigned)snap->sleep_perf,
                     (int)(-snap->sleep_debt_min));
        } else {
            snprintf(val_buf, sizeof(val_buf), "%u%%", (unsigned)snap->sleep_perf);
        }
        _set_cell_value(&g_cells[4], val_buf, "",
                        _sleep_color(snap->sleep_perf));
    }

    // SpO₂
    if (snap->spo2 > 0) {
        snprintf(val_buf, sizeof(val_buf), "%u%%", (unsigned)snap->spo2);
        _set_cell_value(&g_cells[5], val_buf, "",
                        _spo2_color(snap->spo2));
    }

    ESP_LOGI(TAG, "Snapshot: rec=%d%% strain=%.1f hrv=%.0f rhr=%d sleep=%d%% spo2=%d%%",
             snap->recovery_score, (double)snap->strain, (double)snap->hrv_ms,
             snap->resting_hr, snap->sleep_perf, snap->spo2);
}

void display_set_wifi(bool connected)
{
    if (!g_disp || !g_status_wifi) return;
    if (!lvgl_port_lock(50)) return;

    if (connected) {
        lv_label_set_text(g_status_wifi, "Wi‑Fi");
        lv_obj_set_style_text_color(g_status_wifi, lv_color_hex(CLR_WIFI_ON), 0);
    } else {
        lv_label_set_text(g_status_wifi, "");
        lv_obj_set_style_text_color(g_status_wifi, lv_color_hex(CLR_WIFI_OFF), 0);
    }
    lvgl_port_unlock();
}


void display_set_hr_stale(uint32_t seconds_ago)
{
    if (!g_disp || !g_stale_label) return;
    if (!lvgl_port_lock(50)) return;

    if (seconds_ago > 0) {
        char buf[32];
        if (seconds_ago < 60) {
            snprintf(buf, sizeof(buf), "HR stale · %us", (unsigned)seconds_ago);
        } else {
            snprintf(buf, sizeof(buf), "HR stale · %um", (unsigned)(seconds_ago / 60));
        }
        lv_label_set_text(g_stale_label, buf);
    } else {
        lv_label_set_text(g_stale_label, "");
    }
    lvgl_port_unlock();
}