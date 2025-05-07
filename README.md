# SmartStall_API
# SmartStall Bluetooth Peripheral API

This document describes the public-facing **Bluetooth GATT API** for the SmartStall touchless locking system. It is intended for developers building Bluetooth central clients (e.g., ESP32-based hubs) that connect to SmartStall peripherals.

The SmartStall device is a Bluetooth Low Energy (BLE) peripheral. It advertises a custom GATT service that exposes:
- Stall lock state (enum values)
- Battery voltage (in millivolts)

> ⚠️ The SmartStall system is intended for use in bathrooms. After 20 minutes of being locked without activity, the device enters sleep mode and updates the central. This may indicate a medical issue with the user.

---

## Service Overview

| Name               | UUID                                   | Type     |
|--------------------|----------------------------------------|----------|
| SmartStall Service | `c56a1b98-6c1e-413a-b138-0e9f320c7e8b` | Primary  |

---

## Characteristics

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

## Advertising Data

SmartStall advertises the 128-bit service UUID:

```c
const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_STALL_SERVICE_VAL),
};
```

---

## Getting Started (Central)

To integrate with SmartStall:

1. Scan for advertising packets containing `c56a1b98-6c1e-413a-b138-0e9f320c7e8b`
2. Connect to the peripheral
3. Discover the service and characteristics
4. Subscribe to notifications on both UUIDs
5. Handle `uint16_t` values accordingly in your app logic

---

## License

This API specification is provided under the MIT License. See LICENSE file for details.

---

© 2025 SmartStall. All rights reserved.