# SmartStall Bluetooth Central Hub

This Particle firmware (designed for B-Series / M SoM and other BLE-capable Particle devices) acts as a **multi‑device BLE central hub**. It discovers SmartStall peripherals, cycles through them in a controlled round‑robin schedule, performs a single-shot poll (connect → discover → read → publish → disconnect), and repeats.

## Current Architecture (v1.1.0)

| Aspect | Strategy |
|--------|----------|
| Discovery | Opportunistic light scan every 15s + full/global scan every 60s when idle |
| Device Tracking | In‑RAM registry with lastSeen, lastRead, failureCount (max 12 devices) |
| Poll Model | Single-shot per device (no long-held connections, no notifications) |
| Connection | Up to 3 immediate attempts (250 ms spacing) per poll cycle |
| Timeout | 10 s connect timeout (was 15 s in earlier versions) |
| Backoff | Added after consecutive failures; interval extended dynamically |
| Stale Skip | Devices not seen in >120 s skipped until seen again |
| Reads | Each characteristic read with up to 3 retries (150 ms spacing) |
| Publish | Single consolidated `smartstall/data` event per successful poll |
| Threading | System thread enabled by default on Device OS ≥ 6.2 (no explicit macro needed) |

## Why Single-Shot Polling & No Notifications?

The earlier revision subscribed to notifications and emitted multiple event streams (`status`, `sensors`, `battery`). To simplify bandwidth, event quota usage, and avoid complexity from long-lived connections (which increased chance of timeouts / stale handles when rotating many devices), we intentionally removed notifications. Every connection now performs explicit GATT reads with retry logic, ensuring deterministic data snapshots.

If future requirements need near-real-time change streaming, notifications can be reintroduced selectively (e.g., stall status only) while keeping the current poll loop intact.

## BLE GATT Profile

Primary Service UUID:

```
c56a1b98-6c1e-413a-b138-0e9f320c7e8b
```

Characteristics:
1. Stall Status (`47d80a44-c552-422b-aa3b-d250ed04be37`)
   - Format: `uint16_t`
   - Values:
     - 0 = UNKNOWN (Initial/undefined state)
     - 1 = INIT (System initializing or idle)
     - 2 = LOCKED (Active locking sequence)
     - 3 = UNLOCKED (Active unlocking sequence)
     - 4 = SLEEP (Entering deep sleep mode)
     - 5 = 20_MINUTE_ALERT (Locked for 20 minutes or more; safety alert)
2. Battery Voltage (`7d108dc9-4aaf-4a38-93e3-d9f8ff139f11`)
   - Format: `uint16_t` millivolts
3. Sensor Counts (`3e4a9f12-7b5c-4d8e-a1b2-9c8d7e6f5a4b`)
   - Format: 3 × `uint32_t` (limit_switch, ir_sensor, hall_sensor)

Each poll cycle resets characteristic handles before discovery to prevent accidental reuse across peers.

## Cloud Event Stream

### `smartstall/data`
Single consolidated JSON payload published when the stall status changes (no publish on unchanged status). Includes derived occupancy field:

- occupied (boolean)

Occupancy mapping: 0/1/3/4 → non-occupied, 2/5 → occupied.

```json
{
   "device": "AA:BB:CC:DD:EE:FF",
   "timestamp": 1696118400,
   "status": 2,
   "status_name": "LOCKED",
   "occupied": true,
   "battery_mv": 3700,
   "battery_v": 3.70,
   "sensor_counts": {
      "limit_switch": 150,
      "ir_sensor": 89,
      "hall_sensor": 145
   }
}
```

Removed events (legacy, no longer emitted): `smartstall/status`, `smartstall/sensors`, `smartstall/battery`.

## Poll & Backoff Logic

Baseline per-device poll interval: 30 s.

If consecutive read/connect failures accrue (tracked via `failureCount`):
- After `MAX_FAILURES_BEFORE_BACKOFF` (3), extra backoff time (45 s * (failureCount - 2)) is added to the interval.
- When a device is seen again in advertisements and has been idle, failure count decays gradually.
- Devices not seen for >120 s are temporarily skipped to avoid wasted connection attempts.

## Connection Flow (Per Device)
1. Selected by scheduler (round‑robin, respecting interval/backoff)
2. Scanning stopped (if active)
3. Up to 3 immediate `BLE.connect()` attempts
4. On success (callback or manual detect) → service discovery (with up to 2 retries if zero services)
5. Characteristic discovery & assignment
6. Each characteristic read with retry (3 attempts)
7. Consolidated publish
8. Disconnect and return to scanning/scheduling loop

## Getting Started

1. Flash to a Particle device with BLE (Boron, Argon, Photon 2, B-Series SoM, M SoM).
2. Power SmartStall peripherals so they advertise the service UUID.
3. Monitor serial logs:
   ```bash
   particle serial monitor --follow
   ```
4. View events:
   ```bash
   particle subscribe smartstall
   ```
5. Expect a `smartstall/data` event only when a device's status changes.

## Adjusting Behavior

| Need | Tweak |
|------|-------|
| Poll less often | Increase `DEVICE_POLL_INTERVAL_MS` |
| Reduce scanning load | Increase `GLOBAL_SCAN_INTERVAL_MS` and opportunistic scan threshold |
| Harsher failure backoff | Increase `DEVICE_FAILURE_BACKOFF_MS` or lower `MAX_FAILURES_BEFORE_BACKOFF` |
| Keep connections longer | (Would require reintroducing a connected state loop + notifications) |
| Re-enable legacy events | Add publishes inside `publishSmartStallData()` for subsets |

## Troubleshooting

| Symptom | Likely Cause | Action |
|---------|--------------|--------|
| Repeated connection timeouts | Device asleep / out of range / interference | Verify RSSI, move closer, ensure advertising interval sane |
| Only battery or partial data (should not happen now) | Characteristic discovery failure | Check logs for validity summary; verify service UUIDs |
| Device never polled again | Marked stale or heavy backoff | Confirm it is still advertising; reduce `DEVICE_STALE_MS` |
| Event quota concerns | Too many devices at 30 s poll | Increase interval or implement multi-device batching |

## Future Enhancements (Not Implemented Yet)
- Optional partial notification reintroduction (status only).
- Dynamic MTU negotiation (if required by large characteristics in future revisions).
- Particle.variable exposure of registry snapshot.
- Persistent caching of device registry across resets (EEPROM / retained RAM).

## Support & Feedback
Questions or feedback? Join the [Particle community](https://community.particle.io) or your internal SmartStall engineering channel.

## Version

Firmware / README version: 1.1.0 (single-shot multi-device polling, consolidated publish)