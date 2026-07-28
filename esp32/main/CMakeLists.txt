idf_component_register(
    SRCS
        "app_main.c"
        "whoop_ble.c"
        "whoop_api.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp-tls
        cjson
        bt
)

# Flash arguments for ESP32-S3 via USB:
#   idf.py -p /dev/cu.usbmodem* flash monitor
