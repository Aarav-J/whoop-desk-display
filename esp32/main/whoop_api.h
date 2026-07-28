#pragma once
#include <stdint.h>

// Snapshot of WHOOP data the display cares about.
// Populated by the API poller every ~5 min.
typedef struct {
    // Recovery
    uint8_t  recovery_score;        // 0-100, or 255 if n/a
    float    hrv_ms;
    uint8_t  resting_hr;
    uint8_t  spo2;
    // Strain (latest cycle)
    float    strain;
    uint8_t  avg_hr;
    // Sleep (latest)
    uint8_t  sleep_perf;            // 0-100
    int32_t  sleep_debt_min;
    // Identity
    uint8_t  max_hr;                // for alert threshold
    // Status
    bool     valid;                 // true after first successful poll
} whoop_snapshot_t;

// Callback type: fired after each successful poll (or on error).
// snapshot.valid == false means poll failed.
typedef void (*whoop_api_cb_t)(const whoop_snapshot_t *snapshot);

// Start periodic polling.  Pass Wi-Fi SSID + password + backend URL.
// Non-blocking; results arrive on callback.
void whoop_api_start(const char *wifi_ssid, const char *wifi_pass,
                     const char *backend_url, whoop_api_cb_t cb);

// Trigger an immediate poll (e.g. on wake from deep sleep).
void whoop_api_poll_now(void);
