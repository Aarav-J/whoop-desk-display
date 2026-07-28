#include "whoop_api.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "cJSON.h"
#include <string.h>

#define TAG "whoop-api"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define POLL_INTERVAL_MS   (5 * 60 * 1000)  // 5 min
#define MAX_HTTP_RECV      2048

static EventGroupHandle_t g_wifi_evt;
static whoop_api_cb_t g_cb;
static char g_backend_url[256];
static int g_retry_count = 0;

// ── Wi-Fi helpers ─────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *event_data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_retry_count < 10) {
            esp_wifi_connect();
            g_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi retry %d", g_retry_count);
        } else {
            xEventGroupSetBits(g_wifi_evt, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR, IP2STR(&ev->ip_info.ip));
        g_retry_count = 0;
        xEventGroupSetBits(g_wifi_evt, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(const char *ssid, const char *pass) {
    g_wifi_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection (max 30s)
    EventBits_t bits = xEventGroupWaitBits(g_wifi_evt,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi OK");
        return ESP_OK;
    }
    return ESP_FAIL;
}

// ── HTTP poll ─────────────────────────────────────────────────────────────
static void poll_api(whoop_snapshot_t *snap) {
    memset(snap, 0, sizeof(*snap));

    if (g_backend_url[0] == '\0') {
        ESP_LOGW(TAG, "No backend URL configured — using stubbed values");
        // Stub until backend is live
        snap->recovery_score = 84;
        snap->hrv_ms = 97.2f;
        snap->resting_hr = 64;
        snap->spo2 = 95;
        snap->strain = 5.1f;
        snap->avg_hr = 71;
        snap->sleep_perf = 82;
        snap->sleep_debt_min = 0;
        snap->max_hr = 188;
        snap->valid = true;
        return;
    }

    // Real HTTP poll when backend exists
    esp_http_client_config_t http_cfg = {
        .url = g_backend_url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 15000,
        .buffer_size = MAX_HTTP_RECV,
        .user_agent = "whoop-display-esp32/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

    char *response = malloc(MAX_HTTP_RECV);
    if (!response) { esp_http_client_cleanup(client); return; }
    memset(response, 0, MAX_HTTP_RECV);

    int content_len = 0;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        content_len = esp_http_client_fetch_headers(client);
        if (content_len > 0 && content_len < MAX_HTTP_RECV) {
            esp_http_client_read_response(client, response, MAX_HTTP_RECV - 1);
        }
    }
    esp_http_client_cleanup(client);

    if (content_len <= 0) {
        ESP_LOGE(TAG, "HTTP fetch failed: %d", content_len);
        free(response);
        return; // snap->valid stays false
    }

    // Parse JSON (when backend is live)
    cJSON *root = cJSON_Parse(response);
    free(response);
    if (!root) { ESP_LOGE(TAG, "JSON parse failed"); return; }

    cJSON *v;
    #define GET_U8(field, var) do { v = cJSON_GetObjectItem(root, field); \
        if (v && cJSON_IsNumber(v)) snap->var = (uint8_t)v->valueint; } while(0)
    #define GET_FLOAT(field, var) do { v = cJSON_GetObjectItem(root, field); \
        if (v && cJSON_IsNumber(v)) snap->var = (float)v->valuedouble; } while(0)
    #define GET_I32(field, var) do { v = cJSON_GetObjectItem(root, field); \
        if (v && cJSON_IsNumber(v)) snap->var = (int32_t)v->valueint; } while(0)

    GET_U8("recovery", recovery_score);
    GET_FLOAT("hrv", hrv_ms);
    GET_U8("rhr", resting_hr);
    GET_U8("spo2", spo2);
    GET_FLOAT("strain", strain);
    GET_U8("avg_hr", avg_hr);
    GET_U8("sleep_perf", sleep_perf);
    GET_I32("sleep_debt_min", sleep_debt_min);
    GET_U8("max_hr", max_hr);
    #undef GET_U8
    #undef GET_FLOAT
    #undef GET_I32

    snap->valid = true;
    ESP_LOGI(TAG, "Poll OK: recovery=%d strain=%.1f sleep=%d%%",
             snap->recovery_score, snap->strain, snap->sleep_perf);
    cJSON_Delete(root);
}

// ── Poller task ───────────────────────────────────────────────────────────
static void poller_task(void *pv) {
    whoop_snapshot_t snap;

    for (;;) {
        poll_api(&snap);
        if (g_cb) g_cb(&snap);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

// ── Public API ────────────────────────────────────────────────────────────
void whoop_api_start(const char *ssid, const char *pass,
                     const char *backend_url, whoop_api_cb_t cb) {
    g_cb = cb;
    if (backend_url) {
        strncpy(g_backend_url, backend_url, sizeof(g_backend_url) - 1);
    }

    // Init NVS (Wi-Fi credentials storage)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Connect Wi-Fi
    esp_err_t wifi_ok = wifi_init_sta(ssid, pass);
    if (wifi_ok != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi failed — starting poller anyway (stub mode)");
    }

    // Launch poller task on Core 1
    xTaskCreatePinnedToCore(poller_task, "whoop-api", 8192, NULL, 3, NULL, 1);
}

void whoop_api_poll_now(void) {
    // Signal poller task via notification (simpler: just let it run on interval)
    ESP_LOGI(TAG, "poll_now not yet implemented — auto-poll on interval");
}
