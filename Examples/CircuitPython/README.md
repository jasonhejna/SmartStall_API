# SmartStall Bluetooth Hub

This repository contains a working CircuitPython implementation of a Bluetooth Low Energy (BLE) central hub designed to connect with SmartStall devices. The hub scans for nearby SmartStall peripherals, connects, and reads GATT characteristics that describe the lock state and battery status of the stall.

## 📃 About SmartStall
SmartStall is a touchless locking system used in restrooms. Each SmartStall device exposes a BLE GATT profile containing:

- A unique 6-byte Device ID
- Stall Status (INIT, LOCKED, UNLOCK, SLEEP)
- Battery Voltage in millivolts

## 🚀 Features
- Scans for SmartStall advertisements using 128-bit custom service UUID
- Connects and discovers SmartStall GATT service
- Reads:
  - Device ID (hardware ID)
  - Stall Status (state of the lock)
  - Battery Voltage (mV)
- Designed for ESP32-S3 boards running CircuitPython

## 🛏️ SmartStall GATT Profile
- **Service UUID**: `c56a1b98-6c1e-413a-b138-0e9f320c7e8b`

### Characteristics

| Name            | UUID                                   | Type            | Format     | Description |
|-----------------|----------------------------------------|------------------|------------|-------------|
| Device ID       | `34e6784c-bf53-41d5-a090-7c123d5c1b78` | `READ`           | `byte[6]`  | 6-byte NRF52 hardware ID |
| Stall Status    | `47d80a44-c552-422b-aa3b-d250ed04be37` | `READ`, `NOTIFY` | `uint16_t` | Lock state |
| Battery Voltage | `7d108dc9-4aaf-4a38-93e3-d9f8ff139f11` | `READ`, `NOTIFY` | `uint16_t` | Battery in millivolts |

### Stall Status Values

| Value | State         | Meaning |
|-------|---------------|---------|
| `1`   | INIT          | Device has booted |
| `2`   | LOCKED        | Stall is locked |
| `3`   | UNLOCK        | Stall has been unlocked |
| `4`   | SLEEP         | 20-minute timeout |

> The 20-minute timeout may indicate a medical emergency.

## 🔌 Hub Behavior
1. Continuously scans for BLE advertisements containing the SmartStall service UUID
2. Upon discovery, connects and reads characteristics
3. Displays data over USB serial console

Example Output:
```
🔍 Scanning for SmartStall...
️ Found SmartStall!
  Address: <Address ee:1c:8f:46:50:43>
  RSSI: -66
🔗 Connecting...
📡 Connected!
🔹 Device ID: aabbccddeeff
🔹 Stall Status: 2
🔹 Battery Voltage: 4812 mV
🔌 Disconnected.
```

## 🚀 Getting Started
1. Flash a compatible CircuitPython firmware on your ESP32-S3 board
2. Install the latest `adafruit_ble` library bundle into the `lib/` folder
3. Copy the `code.py` file into the root directory
4. Open a serial monitor to view logs

## 📈 Use Cases
- Hub device for managing multiple stalls
- Data logger for stall usage
- Trigger alerts if no unlock occurs within 20 minutes

## 👁️ Resources
- [SmartStall GATT API](../SmartStall_API.md) (defines peripheral behavior)
- [Adafruit BLE Docs](https://docs.circuitpython.org/projects/ble/en/latest/)

## 🎓 License
MIT License.

---

Built by [Sum Hydration](https://www.sumhydration.com) for hygienic and accessible public restrooms.
