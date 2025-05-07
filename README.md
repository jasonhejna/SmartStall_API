# SmartStall_API


![image](docs/smartstall_product_img.png)

# SmartStall Bluetooth Peripheral API

This document describes the public-facing **Bluetooth GATT API** for the SmartStall touchless locking system. It is intended for developers building Bluetooth central clients (e.g., ESP32-based hubs) that connect to SmartStall peripherals.

The SmartStall device is a Bluetooth Low Energy (BLE) peripheral. It advertises a custom GATT service that exposes:
- Stall lock state (enum values)
- Battery voltage (in millivolts)

> ⚠️ The SmartStall system is intended for use in bathrooms. After 20 minutes of being locked without activity, the device enters sleep mode and updates the central. This may indicate a medical issue with the user.

---

## GATT Service Overview

- **Service UUID**: `c56a1b98-6c1e-413a-b138-0e9f320c7e8b`
- **Characteristics**:

| Characteristic        | UUID                                    | Type     | Description                                                                 |
|-----------------------|------------------------------------------|----------|-----------------------------------------------------------------------------|
| Device ID             | `34e6784c-bf53-41d5-a090-7c123d5c1b78`   | `READ`   | 6-byte unique hardware ID read from the NRF52’s FICR register.              |
| Stall Status          | `47d80a44-c552-422b-aa3b-d250ed04be37`   | `READ+NOTIFY` | Indicates current lock state (e.g., INIT, LOCKED, SLEEP).             |
| Battery Voltage (mV)  | `7d108dc9-4aaf-4a38-93e3-d9f8ff139f11`   | `READ+NOTIFY` | Battery voltage as an unsigned 16-bit integer in millivolts.         |

---

## Characteristics

## Device ID

The `Device ID` is a 6-byte unique identifier derived from the NRF52840’s factory info configuration registers (`FICR->DEVICEID`).
- Use this ID to differentiate between deployed SmartStall devices.
- Value is fixed for each device.

---

### 🟦 Stall Status

- **UUID**: `47d80a44-c552-422b-aa3b-d250ed04be37`
- **Properties**: `READ`, `NOTIFY`
- **Format**: `uint16_t`
- **Description**: Indicates the current state of the stall lock.

#### Enum Values

| Value | Name             | Description                                         |
|-------|------------------|-----------------------------------------------------|
| `1`   | `STATE_INIT`     | Device booted, reporting initialization             |
| `2`   | `STATE_LOCKED`   | Stall is physically locked                          |
| `3`   | `STATE_UNLOCK`   | Stall has been unlocked                             |
| `4`   | `STATE_SLEEP`    | 20-minute timeout occurred, entering sleep mode     |

> 💡 Central clients can subscribe to receive `NOTIFY` events on status change.

### 🟩 Battery Voltage

- **UUID**: `7d108dc9-4aaf-4a38-93e3-d9f8ff139f11`
- **Properties**: `READ`, `NOTIFY`
- **Format**: `uint16_t`
- **Units**: Millivolts (mV)
- **Description**: Most recent measured battery voltage of the device.

> Battery voltage is measured at boot and sent via notification.

> Example: `4910mV` → `4.910V`
---

## Notifications

To receive real-time updates:

1. Central must **connect** and **enable notifications** for both characteristics.
2. SmartStall will notify under these conditions:
   - After **boot** (`STATE_INIT`)
   - When **locked** or **unlocked**
   - When **battery voltage** is read at boot
   - After **20-minute inactivity timeout** (possible medical emergency)

---

## BLE Advertisement

- The SmartStall peripheral advertises with:
  - Connectable mode
  - Its 128-bit service UUID

## Interaction Workflow

1. Central (hub) scans and connects to a SmartStall peripheral.
2. Reads the `Device ID` once.
3. Subscribes to notifications for `Stall Status` and `Battery Voltage`.
4. Waits for updates:
   - Stall open, lock, unlock events
   - 20-minute timeout alert (possible emergency)
   - Battery level changes (optional monitoring)

---

## Example Usage (ESP32)

```cpp
// Pseudocode to read GATT values
connectToSmartStall();
read(deviceID);
subscribe(stallStatus);
subscribe(batteryVoltage);
```

---

## License

This API specification is provided under the MIT License. See LICENSE file for details.

---

© 2025 SmartStall. All rights reserved.