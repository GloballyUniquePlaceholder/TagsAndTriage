
# THIS IS ALL CLAUDE
"""
BLE Heart Rate Service client for the Pico WH heart rate monitor.

Connects to a BLE peripheral named "PicoW-HRM", subscribes to the standard
Heart Rate Measurement characteristic (0x2A37), and prints/logs each reading.

Run on Windows (10/11) with Bluetooth LE hardware:
    pip install -r requirements.txt
    python hr_monitor.py
"""

import asyncio
import struct
from datetime import datetime

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "PicoW-HRM"
HEART_RATE_MEASUREMENT_UUID = "00002a37-0000-1000-8000-00805f9b34fb"
LOG_FILE = "heart_rate_log.csv"


def parse_heart_rate_measurement(data: bytearray) -> dict:
    """Decode a Heart Rate Measurement characteristic payload per the
    Bluetooth GATT spec (works for both this firmware's simple 2-byte
    payload and fuller payloads from other HRS devices)."""
    flags = data[0]
    hr_is_16bit = bool(flags & 0x01)
    contact_supported = bool(flags & 0x02)
    contact_detected = bool(flags & 0x04)
    energy_present = bool(flags & 0x08)
    rr_present = bool(flags & 0x10)

    offset = 1
    if hr_is_16bit:
        bpm = struct.unpack_from("<H", data, offset)[0]
        offset += 2
    else:
        bpm = data[offset]
        offset += 1

    energy_expended = None
    if energy_present:
        energy_expended = struct.unpack_from("<H", data, offset)[0]
        offset += 2

    rr_intervals_ms = []
    if rr_present:
        while offset + 2 <= len(data):
            rr_raw = struct.unpack_from("<H", data, offset)[0]
            rr_intervals_ms.append(rr_raw / 1024.0 * 1000.0)
            offset += 2

    return {
        "bpm": bpm,
        "contact_supported": contact_supported,
        "contact_detected": contact_detected,
        "energy_expended": energy_expended,
        "rr_intervals_ms": rr_intervals_ms,
    }


def make_notification_handler(log_file):
    def handler(_sender, data: bytearray):
        reading = parse_heart_rate_measurement(data)
        ts = datetime.now().strftime("%H:%M:%S.%f")[:-3]

        if reading["contact_supported"] and not reading["contact_detected"]:
            print(f"[{ts}] no sensor contact -- place a finger on the sensor")
        else:
            print(f"[{ts}] HR: {reading['bpm']:3d} bpm")

        if log_file:
            log_file.write(
                f"{datetime.now().isoformat()},{reading['bpm']},"
                f"{reading['contact_detected']}\n"
            )
            log_file.flush()

    return handler


async def main():
    print(f"Scanning for '{DEVICE_NAME}'...")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)

    if device is None:
        print("Device not found. Check that the Pico WH is powered on, "
              "the firmware is running, and Bluetooth is enabled on this PC.")
        return

    print(f"Found {device.name} [{device.address}]. Connecting...")

    is_new_log = True
    try:
        with open(LOG_FILE, "r"):
            is_new_log = False
    except FileNotFoundError:
        pass

    log_file = open(LOG_FILE, "a", encoding="utf-8")
    if is_new_log:
        log_file.write("timestamp,bpm,contact_detected\n")

    try:
        async with BleakClient(device) as client:
            print("Connected. Subscribing to Heart Rate Measurement notifications...")
            await client.start_notify(
                HEART_RATE_MEASUREMENT_UUID, make_notification_handler(log_file)
            )
            print(f"Streaming heart rate data (logging to {LOG_FILE}). "
                  "Press Ctrl+C to stop.\n")

            while client.is_connected:
                await asyncio.sleep(1)
    finally:
        log_file.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")