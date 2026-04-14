# SmartStall Bluetooth GATT API Documentation

## Overview

The SmartStall firmware exposes a Bluetooth Low Energy (BLE) GATT service that provides real-time monitoring and historical data collection for an automated stall locking system. This document describes the complete GATT profile, data structures, and integration guidelines for Bluetooth hub implementations.

## Device Information

- **Device Name**: `SmartStall`
- **Protocol**: Bluetooth Low Energy (BLE) 5.0+
- **Connection Type**: Single connection (one client at a time)
- **Advertising**: Connectable, discoverable
- **Security**: No pairing required

## Service Architecture

### Primary Service
- **Service UUID**: `c56a1b98-6c1e-413a-b138-0e9f320c7e8b`
- **Service Type**: Primary Service
- **Characteristics**: 3 characteristics (all READ-only; no notifications)

---

## GATT Characteristics

### 1. Stall Status Characteristic

**Purpose**: Operational status of the stall locking mechanism

- **Characteristic UUID**: `47d80a44-c552-422b-aa3b-d250ed04be37`
- **Properties**: `READ`
- **Permissions**: `READ`
- **Data Type**: `uint16_t` (2 bytes, little-endian)
- **Update Frequency**: Poll on demand

> **Note**: Notifications were removed from this characteristic. Clients must poll (read) the status value periodically rather than subscribing to CCCD notifications.

#### Status Values:
| Value | Status | Description |
|-------|--------|-------------|
| `0` | UNKNOWN | Initial/undefined state (before BLE stack starts) |
| `1` | INIT | System initializing at boot; BLE stack not yet started |
| `2` | LOCKING | Active locking sequence in progress (also set on manual lock via reference switch) |
| `3` | UNLOCKING | Unlocking sequence in progress or STATE_INIT entered after wake |
| `4` | SLEEP | System entering deep sleep (SYSTEMOFF) |
| `5` | PRE_SLEEP | 20-minute idle timeout reached; BLE disconnect initiated before sleep |

> **BLE deferred startup**: The BLE stack does not start at device power-on. It is initialised only after the first complete lock sequence (end of `STATE_LOCKING`). Status values `0` and `1` are therefore never visible over BLE in normal operation; `2` (LOCKING) is typically the first status a connecting client will see.

#### Usage Example:
```c
// Poll current status
uint16_t status;
// status = 2 (LOCKING), 3 (UNLOCKING), 4 (SLEEP), 5 (PRE_SLEEP)
// Recommended polling interval: 5–10 seconds while connected
```

---

### 2. Battery Voltage Characteristic

**Purpose**: Current battery voltage monitoring for power management

- **Characteristic UUID**: `7d108dc9-4aaf-4a38-93e3-d9f8ff139f11`
- **Properties**: `READ`
- **Permissions**: `READ`
- **Data Type**: `uint16_t` (2 bytes, little-endian)
- **Units**: Millivolts (mV)
- **Range**: 0 - 65535 mV
- **Update Frequency**: On-demand (read-only)

#### Usage Example:
```c
// Read battery voltage
uint16_t voltage_mv;
// voltage_mv = 3700 (3.7V), 4200 (4.2V), etc.

// Convert to volts
float voltage_v = voltage_mv / 1000.0f;
```

---

### 3. Sensor Counts Characteristic

**Purpose**: Historical trigger counts for all sensors with persistent storage

- **Characteristic UUID**: `3e4a9f12-7b5c-4d8e-a1b2-9c8d7e6f5a4b`
- **Properties**: `READ`
- **Permissions**: `READ`
- **Data Type**: `struct sensor_counts` (12 bytes total)
- **Storage**: Persistent across power cycles (Zephyr NVS on internal flash)
- **Update Frequency**: Poll on demand

> **Note**: Notifications were removed from this characteristic. Clients must poll (read) the value to observe count changes.

#### Data Structure:
```c
struct sensor_counts {
    uint32_t limit_switch_triggers;  // Bytes 0-3: Door position switch activations
    uint32_t cap_touch_triggers;     // Bytes 4-7: Capacitive touch sensor detections
    uint32_t hall_sensor_triggers;   // Bytes 8-11: Magnetic field changes
};
```

> **Field rename notice**: The second field was previously named `ir_sensor_triggers`. It is now `cap_touch_triggers` and counts MTCH101 capacitive touch edge events (the primary detection source). IR confirmation events are not separately counted.

#### Detailed Field Descriptions:

**`limit_switch_triggers`** (uint32_t, little-endian)
- **Purpose**: Counts every door position change (open ↔ closed)
- **Incremented**: On both rising (0→1) and falling (1→0) edge transitions
- **Use Cases**: Door usage frequency, mechanical wear tracking, maintenance scheduling
- **Range**: 0 to 4,294,967,295

**`cap_touch_triggers`** (uint32_t, little-endian)
- **Purpose**: Counts MTCH101 capacitive touch sensor edge events for user interaction tracking
- **Incremented**: On every valid cap-touch edge (both LOW→HIGH and HIGH→LOW) that passes the lockout filter
- **Use Cases**: User interaction analytics, proximity-based unlock frequency
- **Range**: 0 to 4,294,967,295

**`hall_sensor_triggers`** (uint32_t, little-endian)
- **Purpose**: Counts magnetic field changes for door state monitoring
- **Incremented**: On every hall sensor interrupt (door open/close events)
- **Use Cases**: Door operation validation, magnetic wear tracking
- **Range**: 0 to 4,294,967,295

#### Parsing Example:
```c
// Read 12-byte sensor counts data
uint8_t data[12];
// Parse little-endian values
uint32_t limit_switch  = (data[3] << 24) | (data[2] << 16) | (data[1] << 8) | data[0];
uint32_t cap_touch     = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
uint32_t hall_sensor   = (data[11] << 24) | (data[10] << 16) | (data[9] << 8) | data[8];
```

---

## Connection Management

### Advertising Parameters
- **Advertising Name**: "SmartStall"
- **Advertising Interval**: 40 ms min / 50 ms max (ADV_INT 64–80 × 0.625 ms)
- **Discoverable**: Yes
- **Connectable**: Yes
- **Max Connections**: 1 (single connection only)

### Connection Behavior
- **BLE startup**: Deferred until the first complete lock sequence; device is silent on BLE during boot and early sensor initialisation
- **Auto-disconnect**: After 20 minutes locked with no unlock event, firmware sets status to `5` (PRE_SLEEP), disconnects the BLE client, then enters SYSTEMOFF sleep
- **Graceful disconnect**: A `STATE_DISCONNECTING` internal state waits up to 2 seconds for clean HCI teardown before proceeding to sleep; the firmware force-cleans the connection reference if the timeout is reached
- **Sleep mode**: Device enters nRF52840 SYSTEMOFF (deepest sleep); wakes only on hall sensor interrupt
- **Wake sources**: Hall effect sensor interrupt (door state change)
- **Reconnection**: Advertising restarts automatically after BLE disconnection
- **TX power**: Advertising at 0 dBm; connection TX power raised to +4 dBm via Nordic SoftDevice vendor commands (when `CONFIG_BT_LL_SOFTDEVICE=y`)
- **PHY**: LE Coded PHY (S=2/S=8, no preference) is requested via HCI on every new connection for extended range; falls back to 1M PHY if the central does not support Coded PHY
- **Extended advertising**: Supported when `CONFIG_BT_EXT_ADV=y` is set; falls back to legacy advertising automatically

---

## Data Persistence & Reliability

### NVS (Flash) Backup System
All sensor counts are automatically backed up to non-volatile storage (Zephyr NVS on internal flash):

- **Queued writes**: Each sensor trigger is queued for NVS write; safe periods are used to avoid BLE stack conflicts
- **Periodic backup**: Full backup every 10 minutes during operation
- **Critical backup**: Full backup (bypassing the queue) before entering sleep mode
- **Power-loss protection**: Counts survive complete power loss
- **Reset behavior**: Pin reset clears all sensor counts to zero

### Flash Write Management
The firmware implements intelligent flash write management to prevent conflicts with Bluetooth operations:

- **Queued Writes**: Sensor updates are queued and processed during safe periods
- **Bluetooth Coordination**: Flash writes are deferred during active BLE connections
- **Retry Logic**: Failed writes are automatically retried up to 5 times
- **Critical Path**: Sleep-time backups bypass queue for immediate storage

---

## Integration Guidelines

### Hub Implementation Recommendations

#### 1. **Connection Management**
```pseudocode
1. Scan for devices advertising "SmartStall"
2. Connect to device with service UUID c56a1b98-6c1e-413a-b138-0e9f320c7e8b
3. Discover all characteristics
4. Read values for Status and Sensor Counts characteristics
5. Implement connection timeout and retry logic
```

#### 2. **Data Collection Strategy**
```pseudocode
// Initial data collection
1. Read current stall status
2. Read current battery voltage
3. Read current sensor counts baseline

// Ongoing monitoring (polling — no notifications)
1. Poll stall status every 5–10 seconds to detect state changes
2. Poll sensor counts every 30–60 seconds to track usage deltas
3. Periodically re-read battery voltage (every 5–10 minutes)
4. Store all data with timestamps for historical analysis
```

#### 3. **Error Handling**
- **Connection Loss**: Implement automatic reconnection with exponential backoff
- **Data Drift**: Re-read all characteristics after reconnection to re-establish baseline
- **Data Validation**: Validate that sensor counts only increase (except after pin reset)
- **Battery Monitoring**: Alert when voltage drops below operational thresholds
- **PRE_SLEEP state**: When status reads `5`, the device is about to sleep; no further reads will succeed; reconnect after the hall sensor wakes the device

#### 4. **Data Analytics Applications**
- **Usage Patterns**: Analyze door open/close frequency and timing
- **User Interaction**: Track hand detection patterns for UX optimization
- **Maintenance Scheduling**: Use trigger counts for predictive maintenance
- **System Health**: Monitor battery levels and operational status trends

---

## Protocol Specifications

### Byte Order
- **All multi-byte values**: Little-endian format
- **Compatibility**: Standard BLE byte ordering

### Connection Parameters
- **Connection Interval**: Standard BLE values (7.5ms - 4s)
- **Slave Latency**: 0 (immediate response)
- **Supervision Timeout**: 20-32 seconds recommended

### Read Access
- **MTU**: Default 23 bytes (sufficient for all characteristics)
- **All characteristics**: READ only; no CCCD descriptors present
- **Polling**: Clients should poll at application-appropriate intervals (see Integration Guidelines)

---

## Version History

### Current Version (v1.2) — PCB rev. 2026-03-21
- **Service UUID**: `c56a1b98-6c1e-413a-b138-0e9f320c7e8b` (unchanged)
- **Characteristics**: 3 (Status, Battery, Sensor Counts) — all **READ only**
- **Notifications removed**: NOTIFY property dropped from Status and Sensor Counts; clients must poll
- **Status value 5 re-defined**: Was "MANUAL_LOCK mode" (reference switch); now "PRE_SLEEP" (20-minute idle timeout, BLE disconnect pending). Manual lock via reference switch now reports as `2` (LOCKING)
- **BLE deferred startup**: Bluetooth stack initialisation deferred until first lock sequence completes; device is BLE-silent during boot and early sensor initialisation
- **STATE_DISCONNECTING**: New internal state for graceful BLE teardown (2-second timeout) before SYSTEMOFF sleep entry
- **TX power control**: Advertising TX at 0 dBm; connection TX raised to +4 dBm via Nordic SoftDevice vendor HCI commands
- **LE Coded PHY**: Requested on every new connection via HCI `LE Set PHY` command; falls back to 1M PHY if central does not support Coded PHY
- **Extended advertising**: Optional support via `CONFIG_BT_EXT_ADV`; falls back to legacy advertising

### v1.1 — PCB rev. 2026-03-21
- **Field rename**: `sensor_counts.ir_sensor_triggers` → `cap_touch_triggers` (bytes 4–7 now count MTCH101 capacitive touch edges)
- **Dual-sensor detection**: MTCH101 capacitive touch arms IR confirmation window; confirmed together before state machine triggers
- **Power rail control**: IR sensor rail off by default; powered on demand only during detection window
- **Post-trigger cooldown**: 5-second suppression after confirmed detection prevents double-triggers from servo movement
- **Advertising restart fix**: Periodic NVS backup no longer permanently kills advertising (was `-EINVAL` from conflicting scan response + `USE_NAME`)

### v1.0 — Initial release
- **Added**: `sensor_counts_uuid` (consolidated sensor triggers)
- **Removed**: `ref_switch_char_uuid` (reference switch state)
- **Removed**: `lock_counts_uuid` (auto lock/unlock counts)
- **Added**: Persistent storage with power-loss protection

---

## Testing & Validation

### Test Scenarios
1. **Connection Stability**: Verify reliable connection/disconnection cycles
2. **Data Accuracy**: Validate sensor count increments match physical triggers
3. **Persistence**: Confirm counts survive power cycles and resets
4. **Notifications**: Verify real-time updates during sensor activation
5. **Battery Reporting**: Validate voltage readings across charge levels

### Expected Behaviors
- Sensor counts should only increase (except after pin reset)
- Status notifications should reflect actual operational states
- Battery voltage should correlate with actual measured values
- Device should reconnect reliably after sleep/wake cycles

---

## Support & Troubleshooting

### Common Issues
- **Cannot connect**: Device may be in SYSTEMOFF sleep; trigger wake via hall sensor (door state change)
- **Unexpected disconnect**: Check if status read `5` (PRE_SLEEP) just before disconnect — the device is entering sleep; reconnect after next door event
- **Stale data**: All characteristics are read-only with no notifications; poll regularly and re-read all values after reconnection
- **Reset counters**: Pin reset will clear all sensor counts to zero

### Debug Information
- All sensor activities are logged via RTT for development debugging
- Status changes include state transition logging
- EEPROM operations provide success/failure logging
- Bluetooth events are logged for connection troubleshooting

---

## Contact & Support

For technical support, firmware updates, or hardware integration assistance, contact the SmartStall engineering team.

**Last Updated**: April 2026 (PCB rev. 2026-03-21)
**Firmware Version**: Compatible with SmartStall v1.2+
**API Version**: 1.2