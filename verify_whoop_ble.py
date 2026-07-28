#!/usr/bin/env python3
"""Verify WHOOP BLE HR broadcast on macOS.

Prerequisites:
  1. pip3 install bleak
  2. On your WHOOP band: WHOOP app → Settings → Device Settings → HR Broadcast → ON
  3. Keep the band within ~3m of this Mac
  4. macOS will pop up a Bluetooth permission dialog — allow it

Usage:
    python3 verify_whoop_ble.py

Connects to the WHOOP strap's standard BLE Heart Rate Service (0x180D),
subscribes to Heart Rate Measurement (0x2A37) notifications, and prints
live HR values every second.
"""

import asyncio
import struct
import sys
from datetime import datetime

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("Install bleak first:  pip3 install bleak")
    sys.exit(1)

HEART_RATE_SVC = "0000180d-0000-1000-8000-00805f9b34fb"
HR_MEASUREMENT_CHAR = "00002a37-0000-1000-8000-00805f9b34fb"


def parse_hr(data: bytearray) -> int | None:
    """Parse Heart Rate Measurement per BLE GATT spec."""
    if len(data) < 2:
        return None
    flags = data[0]
    hr_fmt_16bit = flags & 0x01
    contact_detected = flags & 0x02
    contact_supported = flags & 0x04

    if contact_supported and not contact_detected:
        return None  # band not on skin

    if hr_fmt_16bit:
        hr = struct.unpack_from("<H", data, 1)[0]
    else:
        hr = data[1]
    return int(hr)


def hr_callback(sender, data: bytearray):
    hr = parse_hr(data)
    ts = datetime.now().strftime("%H:%M:%S")
    if hr is not None:
        print(f"  [{ts}]  ❤️  {hr} bpm")
    else:
        print(f"  [{ts}]  —  (no contact / off body)")


async def main():
    print("=" * 48)
    print("WHOOP BLE HR broadcast verification")
    print("=" * 48)

    # Scan for devices advertising Heart Rate service with "WHOOP" in name
    print("\nScanning for WHOOP band (BLE Heart Rate Service 0x180D)...")
    print("  Make sure HR Broadcast is ON in the WHOOP app.\n")

    device = None
    for attempt in range(3):
        devices = await BleakScanner.discover(timeout=5.0, return_adv=True)
        for d, adv in devices.items():
            svc_uuids = adv.service_uuids or []
            name = adv.local_name or d.name or ""
            if HEART_RATE_SVC in svc_uuids and "whoop" in name.lower():
                device = d
                print(f"  Found: {name} ({d.address})  RSSI={adv.rssi}dBm")
                break
        if device:
            break
        print(f"  Attempt {attempt + 1}/3: no WHOOP found. Retrying...")

    if not device:
        print("\nFAIL: Could not find WHOOP band advertising HR service.")
        print("Check:")
        print("  1. WHOOP app → Settings → Device Settings → HR Broadcast is ON")
        print("  2. Band is charged and within range (~3m)")
        print("  3. macOS Bluetooth is ON")
        print("  4. macOS gave Terminal/Python Bluetooth permission")
        sys.exit(1)

    # Connect and subscribe
    print(f"\nConnecting to {device.name} ({device.address})...")
    async with BleakClient(device) as client:
        print("  Connected ✓")
        svcs = client.services

        # Verify the HR service exists
        if HEART_RATE_SVC not in svcs:
            print(f"FAIL: Heart Rate Service not found on device")
            print(f"  Available services: {[s.uuid for s in svcs]}")
            sys.exit(1)
        print("  Heart Rate Service found ✓")

        await client.start_notify(HR_MEASUREMENT_CHAR, hr_callback)
        print("  Subscribed to notifications ✓")
        print("\n  Live HR (Ctrl+C to stop):\n")

        try:
            while True:
                await asyncio.sleep(1)
        except KeyboardInterrupt:
            pass

    print("\nDisconnected.")


asyncio.run(main())
