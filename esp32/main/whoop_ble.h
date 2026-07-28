#pragma once
#include <stdint.h>
#include <stdbool.h>

// Called on main event loop when a new HR value arrives.
// hr_bpm = 0 means contact lost / off body.
// FreeRTOS ISR-safe — post to a queue, don't block.
typedef void (*whoop_ble_hr_cb_t)(uint16_t hr_bpm, bool contact);

// Start scanning + connecting.  Non-blocking; events arrive on callback.
// Call once after Wi-Fi + NimBLE init.
void whoop_ble_start(whoop_ble_hr_cb_t cb);

// Disconnect and stop scanning.
void whoop_ble_stop(void);

// True while connected + subscribed.
bool whoop_ble_is_connected(void);
