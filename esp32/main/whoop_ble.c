#include "whoop_ble.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include <stdio.h>
#include <string.h>

#define TAG "whoop-ble"

#define HR_SVC_UUID     0x180D
#define HR_CHAR_UUID    0x2A37

// ── State ─────────────────────────────────────────────────────────────────
static whoop_ble_hr_cb_t g_hr_cb = NULL;
static bool               g_connected = false;
static bool               g_running = false;
static uint16_t           g_conn_handle = 0;
static struct ble_gap_disc_params g_disc_params;

// ── Forward declarations (MUST precede any call site) ────────────────────
static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static int  gatt_svc_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_svc *svc, void *arg);
static int  gatt_chr_cb(uint16_t conn_handle,
                         const struct ble_gatt_error *error,
                         const struct ble_gatt_chr *chr, void *arg);
static int  gatt_write_cb(uint16_t conn_handle,
                           const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg);
static void start_scan(void);
static void on_disconnect(int reason);

// ── Helpers ───────────────────────────────────────────────────────────────
static const char *addr_str(const ble_addr_t *addr) {
    static char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
    return buf;
}

static void parse_hr_bytes(const uint8_t *data, uint16_t len) {
    if (!g_hr_cb || len < 2) return;
    uint8_t flags = data[0];
    bool contact_supported = flags & 0x04;
    bool contact_detected = flags & 0x02;
    bool fmt_16bit = flags & 0x01;
    if (contact_supported && !contact_detected) { g_hr_cb(0, false); return; }
    uint16_t hr = fmt_16bit && len >= 3
        ? data[1] | ((uint16_t)data[2] << 8) : data[1];
    g_hr_cb(hr, true);
}

static int extract_name(const uint8_t *data, uint16_t data_len,
                         char *out, int out_len) {
    uint16_t pos = 0;
    while (pos < data_len - 1) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= data_len) break;
        uint8_t field_type = data[pos + 1];
        if ((field_type == 0x08 || field_type == 0x09) && field_len >= 2) {
            int nlen = field_len - 1;
            if (nlen > out_len - 1) nlen = out_len - 1;
            memcpy(out, &data[pos + 2], nlen);
            out[nlen] = '\0';
            return nlen;
        }
        pos += field_len + 1;
    }
    out[0] = '\0';
    return 0;
}

static bool adv_has_hr_service(const uint8_t *data, uint16_t data_len) {
    uint16_t pos = 0;
    while (pos < data_len - 1) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= data_len) break;
        uint8_t field_type = data[pos + 1];
        if ((field_type == 0x02 || field_type == 0x03) && field_len >= 3) {
            int count = (field_len - 1) / 2;
            for (int i = 0; i < count; i++) {
                uint16_t uuid = data[pos + 2 + i*2]
                              | ((uint16_t)data[pos + 3 + i*2] << 8);
                if (uuid == HR_SVC_UUID) return true;
            }
        }
        pos += field_len + 1;
    }
    return false;
}

// ── NimBLE host callbacks ──────────────────────────────────────────────
static void on_sync(void) {
    int rc;
    ESP_LOGI(TAG, "NimBLE synced with controller");

    // Generate random address (HCI call — only safe after sync)
    ble_addr_t addr;
    rc = ble_hs_id_gen_rnd(1, &addr);
    if (rc == 0) {
        ble_hs_id_set_rnd(addr.val);
    } else {
        ESP_LOGW(TAG, "ble_hs_id_gen_rnd failed: %d", rc);
    }

    start_scan();
}

static void on_reset(int reason) {
    ESP_LOGW(TAG, "NimBLE reset (reason=%d)", reason);
}


// ── Scanning ──────────────────────────────────────────────────────────────
static void start_scan(void) {
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        own_addr_type = BLE_OWN_ADDR_PUBLIC; // fallback
    }
    ESP_LOGI(TAG, "Scanning for WHOOP (0x180D)...");
    ble_gap_disc(own_addr_type, BLE_HS_FOREVER,
                 &g_disc_params, gap_event_cb, NULL);
}

// ── Disconnect + retry ────────────────────────────────────────────────────
static void on_disconnect(int reason) {
    ESP_LOGW(TAG, "Disconnected (reason=%d). Restarting scan in 5s...", reason);
    g_connected = false;
    if (g_running) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        start_scan();
    }
}

// ── GATT callbacks ────────────────────────────────────────────────────────
static int gatt_svc_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_svc *svc, void *arg) {
    if (error->status != 0) {
        ESP_LOGE(TAG, "Service discovery error: %d", error->status);
        return 0;
    }
    if (!svc) return 0;  // end-of-list
    ESP_LOGI(TAG, "Found HR Service: [%04x-%04x]",
             svc->start_handle, svc->end_handle);
    ble_gattc_disc_all_chrs(conn_handle, svc->start_handle,
                             svc->end_handle, gatt_chr_cb, NULL);
    return 0;
}

static int gatt_chr_cb(uint16_t conn_handle,
                        const struct ble_gatt_error *error,
                        const struct ble_gatt_chr *chr, void *arg) {
    if (error->status != 0) {
        ESP_LOGE(TAG, "Char discovery error: %d", error->status);
        return 0;
    }
    if (!chr) return 0;  // end-of-list
    if (chr->uuid.u.type == BLE_UUID_TYPE_16 &&
        chr->uuid.u16.value == HR_CHAR_UUID) {
        ESP_LOGI(TAG, "Found HR Char: handle=%04x val=%04x props=0x%02x",
                 chr->def_handle, chr->val_handle, chr->properties);
        if (!(chr->properties & BLE_GATT_CHR_PROP_NOTIFY)) {
            ESP_LOGE(TAG, "HR char does not support notify!");
            return 0;
        }
        ESP_LOGI(TAG, "Subscribing to CCCD at handle %04x...",
                 chr->val_handle + 1);
        uint8_t cccd[2] = {0x01, 0x00};
        ble_gattc_write_flat(conn_handle, chr->val_handle + 1,
                              cccd, sizeof(cccd), gatt_write_cb, NULL);
    }
    return 0;
}

static int gatt_write_cb(uint16_t conn_handle,
                          const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg) {
    if (error->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed: %d", error->status);
        return 0;
    }
    ESP_LOGI(TAG, "Subscribed to HR notifications ✓");
    return 0;
}

// ── GAP event handler ─────────────────────────────────────────────────────
static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        struct ble_gap_disc_desc *d = &event->disc;
        if (!adv_has_hr_service(d->data, d->length_data)) return 0;
        char name[32] = {0};
        extract_name(d->data, d->length_data, name, sizeof(name));
        if (!strstr(name, "WHOOP")) return 0;

        ESP_LOGI(TAG, "Found: %s (%s)  RSSI=%d",
                 name, addr_str(&d->addr), d->rssi);
        ble_gap_disc_cancel();

        int rc = ble_gap_connect(BLE_OWN_ADDR_RANDOM, &d->addr,
                                  15000, NULL, gap_event_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_connect: %d — rescanning", rc);
            start_scan();
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Connect failed: %d — rescanning",
                     event->connect.status);
            start_scan();
            return 0;
        }
        g_conn_handle = event->connect.conn_handle;
        g_connected = true;

        // Fetch connection descriptor for peer address
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(g_conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGI(TAG, "Connected: conn=%d, peer=%s",
                     g_conn_handle, addr_str(&desc.peer_ota_addr));
        } else {
            ESP_LOGI(TAG, "Connected: conn=%d", g_conn_handle);
        }

        ble_gattc_disc_svc_by_uuid(g_conn_handle,
            BLE_UUID16_DECLARE(HR_SVC_UUID), gatt_svc_cb, NULL);
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        g_conn_handle = 0;
        on_disconnect(event->disconnect.reason);
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint8_t buf[32];
        uint16_t len = OS_MBUF_PKTLEN(om);
        if (len > sizeof(buf)) len = sizeof(buf);
        os_mbuf_copydata(om, 0, len, buf);
        parse_hr_bytes(buf, len);
        return 0;
    }

    default:
        return 0;
    }
}

// ── NimBLE host FreeRTOS task ─────────────────────────────────────────────
// nimble_port_freertos_init() creates a task that calls this function.
// Passing NULL here jumps to 0x0 → InstrFetchProhibited reboot loop.
static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run(); // returns only after nimble_port_stop()
    nimble_port_freertos_deinit();
}

// ── Public API ────────────────────────────────────────────────────────────

void whoop_ble_start(whoop_ble_hr_cb_t cb) {
    g_hr_cb = cb;
    g_running = true;

    // Discovery params set before init
    memset(&g_disc_params, 0, sizeof(g_disc_params));
    g_disc_params.passive = 0;
    g_disc_params.filter_duplicates = 0;
    g_disc_params.limited = 0;
    g_disc_params.filter_policy = 0;
    g_disc_params.itvl = 0;
    g_disc_params.window = 0;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(ret));
        return;
    }

    // Host callbacks after port init (controller is up; sync fires from host task)
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    // Start NimBLE host task — on_sync() fires when controller is ready
    nimble_port_freertos_init(nimble_host_task);
    ESP_LOGI(TAG, "NimBLE host started (waiting for sync...)");
}

void whoop_ble_stop(void) {
    g_running = false;
    if (g_connected)
        ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    ble_gap_disc_cancel();
}

bool whoop_ble_is_connected(void) {
    return g_connected;
}
