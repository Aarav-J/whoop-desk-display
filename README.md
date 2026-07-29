# whoop-desk-display

ESP32-S3 desk display for WHOOP metrics — receives real-time HR via BLE and polls the WHOOP API for recovery, strain, and sleep data.

## Hardware

| Part | Detail |
|---|---|
| **MCU** | ESP32-S3 (8 MB flash, PSRAM) |
| **Display** | Waveshare 1.69" LCD, 240×280, ST7789V2, SPI |
| **Connectivity** | Wi-Fi (API), BLE (Whoop HR broadcast) |

## Firmware stack

| Layer | Choice |
|---|---|
| **RTOS** | FreeRTOS (ESP-IDF built-in) |
| **BLE** | NimBLE (central role, HR service 0x180D) |
| **Display driver** | `esp_lcd_st7789` (ESP-IDF component) |
| **UI** | LVGL via `esp_lvgl_port` |
| **HTTP** | `esp_http_client` + mbedTLS |
| **JSON** | cJSON |

## Architecture

```
┌──────────────┐    ┌──────────────┐
│  whoop_ble.c │    │ whoop_api.c  │
│  (NimBLE HR) │    │ (HTTPS poll) │
└──────┬───────┘    └──────┬───────┘
       │ Queue              │ Callback
       ▼                    ▼
┌──────────────────────────────────┐
│         main_task()              │
│   (merge BLE + API → display)   │
└──────────────┬───────────────────┘
               │ LVGL widgets
               ▼
┌──────────────────────────────────┐
│    ST7789V2 (240×280 SPI LCD)    │
└──────────────────────────────────┘
```

## Building

### Prerequisites

- ESP-IDF v5.x: https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html
- ESP32-S3 connected via USB

### Configure

```bash
cd esp32
idf.py set-target esp32s3
idf.py menuconfig
```

Set Wi-Fi SSID, password, and backend URL under **Whoop Desk Display**.

Or create `sdkconfig.defaults` overrides — the repo includes defaults for NimBLE, flash size, and PSRAM.

### Build & flash

```bash
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

## Verification tools

| Tool | Purpose |
|---|---|
| `whoop_ble.swift` | macOS CoreBluetooth — verifies Whoop HR broadcast reception |
| `verify_whoop.py` | Whoop API OAuth flow + full data dump |
| `verify_whoop_ble.py` | BLE HR validation script |

### macOS BLE tool

```bash
swiftc whoop_ble.swift -o whoop_ble
./whoop_ble
```

Requires Bluetooth permission. Make sure HR Broadcast is ON in the WHOOP app.

### API verification

```bash
python3 verify_whoop.py
```

Walks through OAuth login, fetches recovery / strain / sleep cycles, and dumps to stdout.

## Display wiring (Waveshare 1.69″ ST7789V2)

The module is a 4-wire SPI IPS panel: **240×280**, controller **ST7789V2**, GH1.25 8-pin cable. Power and logic must both be **3.3V** (do not mix 5V logic with 3.3V VCC).

### Default pin map (ESP32-S3)

| LCD pin | Function | ESP32-S3 GPIO | Notes |
|---------|----------|---------------|-------|
| **VCC** | Power | **3V3** | 3.3V only |
| **GND** | Ground | **GND** | Common ground |
| **DIN** | SPI MOSI / SDA | **GPIO11** | Data to display |
| **CLK** | SPI SCLK / SCL | **GPIO12** | SPI clock |
| **CS** | Chip select | **GPIO10** | Active low |
| **DC** | Data / command | **GPIO9** | Low = cmd, high = data |
| **RST** | Reset | **GPIO14** | Active low |
| **BL** | Backlight | **GPIO13** | Active high on this module |

```
ESP32-S3                 Waveshare 1.69" LCD
──────────               ──────────────────
3V3  ─────────────────→  VCC
GND  ─────────────────→  GND
GPIO11 (MOSI) ────────→  DIN
GPIO12 (SCLK) ────────→  CLK
GPIO10 ───────────────→  CS
GPIO9  ───────────────→  DC
GPIO14 ───────────────→  RST
GPIO13 ───────────────→  BL
```

Change pins in `idf.py menuconfig` → **WHOOP Desk Display** → **Waveshare 1.69" LCD pins**, or override `CONFIG_WHOOP_LCD_PIN_*` in `sdkconfig.defaults`.

Avoid ESP32-S3 USB pins (19/20), boot/strapping pins (0, 45, 46), and the flash/PSRAM SPI pins on your module (often 26–37 on octal parts).
