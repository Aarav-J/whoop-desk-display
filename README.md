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

## Status

The display stack is functional and rendering live data:

- **BLE HR broadcast** — real-time heart rate from WHOOP band received and displayed
- **API polling** — recovery, strain, sleep, HRV, SpO₂ fetched every ~5 min
- **ST7789V2** — LVGL widgets rendering on the 240×280 IPS panel via `esp_lcd_st7789`

## Roadmap

The display is step one. The vision is a wellness-aware desk companion that actively
interprets WHOOP data instead of just showing numbers.

### Real-time biofeedback

- **HR spike alerts** — if resting HR climbs above baseline during focused work, the
  display nudges you to breathe, stand up, or step away for 2 minutes
- **Stress detection** — combine elevated HR + low HRV to detect stress spikes before
  you consciously register them; prompt a box-breathing exercise
- **HRV trend watch** — track HRV drift across the workday and warn when it's
  trending down (chronic stress signal)

### Recovery-gated work modes

- **Red recovery** (≤33%) — display shows a "take it easy" mode: suggests lighter
  work, reminds you to hydrate, flags that strain today will carry a higher cost
- **Yellow recovery** (34–66%) — normal operation with a gentle reminder to watch
  intensity
- **Green recovery** (≥67%) — full send: "you're recovered, push hard today"

### Sleep-debt awareness

- If sleep debt exceeds 60 min, the display surfaces it during morning routines
- Correlates sleep performance with next-day HRV to close the feedback loop

### Long-term

- **Ambient display modes** — clock-like face that glows red/amber/green based on
  real-time strain vs recovery balance
- **Historical trends** — weekly HRV, RHR, and sleep charts on-device
- **Haptic / audio alerts** — optional buzzer for HR/stress threshold crossing
- **Home Assistant integration** — publish metrics via MQTT for dashboarding

## Enclosure

A custom 3D-printed enclosure is planned:

- **Form factor** — compact desk wedge, angled ~15° toward the user, footprint
  roughly 50×40×25 mm
- **Material** — matte PLA (dark grey or black) to avoid distracting reflections
- **Mount** — friction-fit tray for the ESP32-S3 dev board + snap-in bezel for the
  1.69″ LCD; no screws visible from the front
- **Vents** — passive airflow slots on the underside; ESP32-S3 runs cool so active
  cooling is unnecessary
- **Cable management** — rear exit channel for USB-C power, routed downward to
  hide the cable

Design files (STEP + STL) will live in `enclosure/` once modeled.

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

## Backend proxy

The ESP32 doesn't call the WHOOP API directly — it talks to a local proxy that handles
OAuth, token refresh, and multi-endpoint aggregation, then serves a flat JSON snapshot.

```bash
# 1. Create .env from the example
cp .env.example .env
# Fill in your WHOOP_CLIENT_ID and WHOOP_CLIENT_SECRET

# 2. Run the backend
python3 whoop_backend.py
```

The server caches the snapshot for 60 s and refreshes on expiry. Endpoints:

| Path | Purpose |
|------|---------|
| `GET /api/snapshot` | Flat JSON with all 9 fields the ESP32 needs |
| `GET /health` | Liveness probe |

Point the ESP32 at it via `menuconfig` → `WHOOP Desk Display` → `Backend API URL`:

```
http://<your-mac-ip>:8080/api/snapshot
```

Env vars (`WHOOP_BACKEND_PORT`, `WHOOP_BACKEND_BIND`, `WHOOP_CACHE_SECS`) can be set
in `.env` or exported in the shell. Shell exports override `.env`.

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
