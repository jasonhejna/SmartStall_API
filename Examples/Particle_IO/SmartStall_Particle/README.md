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
   - Format: 3 × `uint32_t` (limit_switch, hand_activation, hall_sensor)
4. Failed-Lock Count (`9a3f5c21-8e4d-4b7a-b3c5-1f2e3d4c5b6a`)
   - Format: `uint32_t`, little-endian
   - Cumulative, NVS-persisted lifetime count of hands-free auto-locks where the bolt never reached
     LOCKED. A rising value indicates that unit has a mechanical problem.
   - READ-only, polled (no notify/subscribe), same cadence as the other 3 characteristics.
   - Optional: if a peripheral doesn't expose this characteristic (older firmware), the hub skips it
     without affecting the other 3 reads (`failed_locks` is reported as `0` in that case).

Each poll cycle resets characteristic handles before discovery to prevent accidental reuse across peers.

## Cloud Event Stream

### `smartstall/data`
Single consolidated JSON payload published when the stall status, sensor counts, or failed-lock count changes (no publish when all are unchanged). Includes derived occupancy field:

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
      "hand_activation": 89,
      "hall_sensor": 145
   },
   "failed_locks": 0
}
```

Removed events (legacy, no longer emitted): `smartstall/status`, `smartstall/sensors`, `smartstall/battery`.

## Cloud Functions

### `restart`
Reboots the hub's M-SoM on demand. The argument is ignored (pass an empty string). The reset is
deferred ~2 seconds internally so the function call's return value reaches the cloud before the
device reboots.

```bash
particle call <device-name-or-id> restart
```

or via the API:

```bash
curl https://api.particle.io/v1/devices/<device-id>/restart \
     -H "Authorization: Bearer <access-token>" \
     -d "arg="
```

A return value of `1` means the restart was accepted and is queued; the device will drop offline
briefly and reconnect after booting.

## Reliability / Long-Term Uptime

This hub is meant to run unattended for years. A real-world incident showed the device could stop
polling SmartStall peripherals in the field while remaining reachable on the Particle console
(cellular/cloud kept working, ping succeeded) — a manual power cycle was required to recover. That
signature (system thread alive, application thread silently stuck) is the classic symptom of the
`loop()` thread getting wedged inside a blocking BLE stack call (e.g. `discoverAllServices()`,
`discoverCharacteristicsOfService()`, or a characteristic `getValue()` that never returns), since
Device OS runs cellular/cloud on a separate system thread by default (Device OS ≥ 6.2).

To make the hub self-heal instead of requiring physical access, this firmware now includes:

- **Hardware watchdog** (`Watchdog.init/start`, `HUB_WATCHDOG_TIMEOUT_MS` = 90 s): `Watchdog.refresh()`
  is called once per `loop()` iteration *and* at safe boundaries between the individual blocking BLE
  calls inside a single poll cycle (after service discovery, after characteristics discovery, and
  between each of the 4 characteristic reads) — but deliberately *never* from inside a single
  blocking BLE call itself. This means a truly wedged call (one that never returns) still fires the
  watchdog and fully resets the MCU, automatically doing what a manual power cycle did — but the
  *cumulative* time of several sequential calls that each complete slowly (marginal RF link, extra
  retries, 4 characteristics instead of 3) no longer falsely trips it, since the timeout only needs
  to cover the slowest single span between two refresh points rather than the whole cycle. See
  [Watchdog - Hardware](https://docs.particle.io/reference/device-os/api/watchdog-hardware/watchdog-hardware/).
- **Boot reset-reason logging** (`System.resetReason()` / `FEATURE_RESET_INFO`): logged at boot and
  published automatically by Device OS as a `last_reset` cloud event, so a fleet of these hubs can be
  monitored remotely for unexpected/watchdog resets. Note: on RTL872x (P2/Photon 2/M-SoM) this often
  reads back as `NONE` since it isn't always safe to persist the reason to flash before an abrupt
  reset — treat it as best-effort, not authoritative.
- **Heap health trend** (`free_memory` / `min_free_memory` in the hub ledger, sampled every ledger
  write): cheap early-warning for heap fragmentation, which is a common cause of gradual firmware
  instability on devices running for weeks/months (this app allocates `String`/`Variant` objects on
  essentially every poll and ledger write). Watch `min_free_memory` over time in the ledger — a
  steady downward trend across days/weeks (rather than settling at a floor) indicates fragmentation
  that a watchdog reset won't fix long-term and needs a code-level allocation reduction.

Further hardening to consider if incidents recur even with the watchdog in place:
- Reduce dynamic allocation on the hot path (pool/reuse buffers instead of `String::format` /
  `Variant` trees on every poll) to reduce fragmentation pressure over multi-month uptime.
- If a specific BLE call is identified (via reset-reason/heap trends or a future repro) as the one
  that wedges, consider bounding it with a coarser hub-level "no successful poll in N minutes despite
  devices in range" self-check that proactively calls `BLE.off()`/`BLE.on()` before resorting to the
  watchdog's full reset.

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

Firmware / README version: 1.3.0 (hardware watchdog, reset-reason + heap trend reporting, cloud `restart` function)

Previously:
- 1.2.0 (adds optional failed-lock count characteristic read/publish)
- 1.1.0 (single-shot multi-device polling, consolidated publish)