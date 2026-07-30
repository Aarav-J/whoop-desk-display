#include "whoop_ble.h"
#include "whoop_api.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <stdio.h>

#define TAG "main"

// ── Wi-Fi credentials (set in sdkconfig or environment) ──────────────────
#define WIFI_SSID     CONFIG_WHOOP_WIFI_SSID
#define WIFI_PASS     CONFIG_WHOOP_WIFI_PASSWORD
#define BACKEND_URL   CONFIG_WHOOP_BACKEND_URL

// ── FreeRTOS queues ───────────────────────────────────────────────────────
// BLE HR events  →  main logic
typedef struct {
    uint16_t hr;
    bool     contact;
} ble_hr_msg_t;

static QueueHandle_t g_hr_queue;

// ── Callbacks ─────────────────────────────────────────────────────────────

// Called from NimBLE host task when a new HR measurement arrives.
// Just enqueue so main task handles it (no blocking in BLE callback).
static void on_ble_hr(uint16_t hr_bpm, bool contact) {
    ble_hr_msg_t msg = { .hr = hr_bpm, .contact = contact };
    xQueueSend(g_hr_queue, &msg, 0);
}

// Called from API poller task when a snapshot arrives.
static void on_api_snapshot(const whoop_snapshot_t *snap) {
    if (!snap->valid) {
        ESP_LOGW(TAG, "API poll failed — using cached data");
        display_set_wifi(false);
        return;
    }

    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "══ WHOOP snapshot ══");
    ESP_LOGI(TAG, "  Recovery:  %3d%%   HRV: %5.1f ms   RHR: %d bpm   SpO₂: %d%%",
             snap->recovery_score, snap->hrv_ms, snap->resting_hr, snap->spo2);
    ESP_LOGI(TAG, "  Strain:     %4.1f   Avg HR: %d bpm",
             snap->strain, snap->avg_hr);
    ESP_LOGI(TAG, "  Sleep:      %3d%%   Debt: %d min",
             snap->sleep_perf, (int)snap->sleep_debt_min);
    ESP_LOGI(TAG, "  Max HR:     %d bpm  (alert at %d)",
             snap->max_hr, (int)(snap->max_hr * 0.9f));
    ESP_LOGI(TAG, "");

    display_set_snapshot(snap);
    display_set_wifi(true);
}

// ── Main task — consumes BLE HR + evaluates triggers ─────────────────────
static void main_task(void *pv) {
    ble_hr_msg_t msg;
    uint16_t last_hr = 0;
    uint8_t  max_hr = 188;  // updated from API snapshot
    TickType_t last_hr_tick = 0;

    ESP_LOGI(TAG, "Main task running — waiting for BLE HR...");

    for (;;) {
        // Block on HR queue (1s timeout so we can do periodic work)
        if (xQueueReceive(g_hr_queue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            TickType_t now = xTaskGetTickCount();
            float delta_s = (float)(now - last_hr_tick) / configTICK_RATE_HZ;

            if (msg.contact && msg.hr > 0) {
                ESP_LOGI(TAG, "❤️  %3d bpm  (Δ=%.1fs)", msg.hr, delta_s);
                last_hr = msg.hr;
                display_set_hr(msg.hr, true);
                display_set_hr_stale(0);  // fresh — clear stale indicator

                // ── Alert: HR above 90% of max ─────────────────────────
                if (max_hr > 0 && msg.hr >= (uint16_t)(max_hr * 0.9f)) {
                    ESP_LOGW(TAG, "⚠️  HR %d >= %d (90%% of max %d) — SLOW DOWN!",
                             msg.hr, (int)(max_hr * 0.9f), max_hr);
                }
            } else {
                ESP_LOGI(TAG, "—  (off body / no contact)");
                display_set_hr(0, false);
                display_set_hr_stale(0);
            }
        }

        // No HR for >30s? Show staleness; keep last value visible.
        if (last_hr > 0) {
            TickType_t elapsed = xTaskGetTickCount() - last_hr_tick;
            float elapsed_s = elapsed * portTICK_PERIOD_MS / 1000.0f;
            if (elapsed > pdMS_TO_TICKS(30000)) {
                if (elapsed < pdMS_TO_TICKS(31000)) {  // log once per gap
                    ESP_LOGW(TAG, "No HR data for %.0fs", (double)elapsed_s);
                }
                display_set_hr_stale((uint32_t)elapsed_s);
            }
        }
}
}

// ── Entry ─────────────────────────────────────────────────────────────────
void app_main(void) {
    ESP_LOGI(TAG, "════ WHOOP desk display ════");
    ESP_LOGI(TAG, "ESP32-S3 + NimBLE + ST7789");

    // Bring up LCD first so boot status is visible
    esp_err_t disp_err = display_init();
    if (disp_err != ESP_OK) {
        ESP_LOGE(TAG, "Display init failed: %s", esp_err_to_name(disp_err));
    }

    // Create HR queue
    g_hr_queue = xQueueCreate(16, sizeof(ble_hr_msg_t));
    if (!g_hr_queue) {
        ESP_LOGE(TAG, "Queue create failed");
        return;
    }

    // Start API poller (Wi-Fi + HTTP) — pinned to Core 1
    whoop_api_start(WIFI_SSID, WIFI_PASS, BACKEND_URL, on_api_snapshot);

    // Start main UI/logic task on Core 1 too (shares with poller)
    xTaskCreatePinnedToCore(main_task, "main", 4096, NULL, 2, NULL, 1);

    // Start BLE HR — NimBLE host runs on Core 0 by default
    whoop_ble_start(on_ble_hr);
}
