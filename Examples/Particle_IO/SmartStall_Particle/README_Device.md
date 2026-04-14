# SmartStall Firmware

Firmware for SmartStall, an automated stall locking system built on the **nRF52840** SoC, using the **Zephyr RTOS**. This project powers a custom PCB (rev. 2026-03-21) integrating servo control, Bluetooth Low Energy (BLE) communication, dual-sensor proximity detection (capacitive touch + IR), power rail management, and ADC-based battery monitoring.

## Table of Contents

- [Overview](#overview)
- [Hardware Features](#hardware-features)
- [GPIO Pin Assignments](#gpio-pin-assignments)
- [Firmware Architecture](#firmware-architecture)
- [State Machine](#state-machine)
- [Peripheral Integration](#peripheral-integration)
  - [Dual-Sensor Proximity Detection](#dual-sensor-proximity-detection)
  - [MTCH101 Capacitive Touch Sensor](#mtch101-capacitive-touch-sensor)
  - [IR Proximity Sensor](#ir-proximity-sensor)
  - [Power Rail Control](#power-rail-control)
  - [Servo Control](#servo-control)
  - [Status LEDs](#status-leds)
  - [Battery Monitoring](#battery-monitoring)
  - [BLE GATT](#ble-gatt)
- [Power Management](#power-management)
- [Bluetooth API Documentation](#bluetooth-api-documentation)
- [Development Notes](#development-notes)
- [Build Instructions](#build-instructions)
- [Watchdog](#watchdog)
- [License](#license)

---

## Overview

SmartStall automates the opening, locking, and monitoring of a stall door using:

- Servo-driven locking mechanism
- Hall effect sensor for door-state detection
- **Dual-sensor proximity detection**: MTCH101 capacitive touch + IRM-H638T infrared receiver
- Three RGB LED banks for rich status indication
- BLE communication for remote monitoring
- Battery voltage monitoring via ADC
- Low-power sleep modes with per-rail sensor power gating

The firmware is built on **Zephyr RTOS** and targets the `smartstallv1refactor_nrf52840` board definition.

---

## Hardware Features

| Component | Part | Purpose |
|---|---|---|
| nRF52840 SoC | — | Main MCU: BLE, GPIO, PWM, ADC |
| Servo motor | — | Physically locks/unlocks the stall |
| Capacitive touch sensor | MTCH101-I/OT | Primary proximity trigger (3″ sensing pad) |
| IR LED | IR26-21C/L110/CT | 940 nm IR emitter (38 kHz NEC carrier) |
| IR receiver module | IRM-H638T/TR2 | 38 kHz demodulating IR receiver |
| Power rail controller | TPS22996DRLR | Enables/disables sensor power rails |
| Hall effect sensor | — | Detects door open/closed state |
| RGB LEDs (×3 banks) | — | Status indication |
| Battery (Li-Ion) | — | Power source (monitored via ADC on AIN3) |

---

## GPIO Pin Assignments

All GPIOs match the schematic `SCH_main board copy_1_2026-03-21`.

| Signal | Pin | Direction | Notes |
|---|---|---|---|
| LED1_R | P1.10 | Output | Active-low (low-side drive) |
| LED1_G | P1.13 | Output | Active-low |
| LED1_B | P1.15 | Output | Active-low |
| LED2_R | P0.13 | Output | Active-low |
| LED2_G | P0.15 | Output | Active-low |
| LED2_B | P0.17 | — | **Re-routed to IR_RECV_SIG on this revision** |
| LED3_R | P1.06 | Output | Active-low |
| LED3_G | P0.09 | Output | Active-low; NFC pin — requires `CONFIG_NFCT_PINS_AS_GPIOS=y` |
| LED3_B | P1.07 | Output | Active-low |
| CAP_TOUCH_SIG (MTCH101 MTO) | P1.09 | Input | Active-low; edge-both interrupt |
| PWM_MTSA | P0.14 | Output/PWM | MTCH101 sensitivity via RC filter (PWM0, 10 kHz) |
| MTPM | P0.08 | Output | MTCH101 power-mode pin; HIGH = normal scan rate |
| IR_RECV_SIG | P1.02 | Input | IRM-H638T output; active-low, edge interrupt |
| IR_LED_PWM | P1.04 | Output/PWM | 38 kHz NEC carrier (PWM2, separate instance) |
| CAP_SW_EN | P0.21 | Output | Enables MTCH101 power rail (TPS22996) |
| IR_SW_EN | P0.25 | Output | Enables IR sensor power rail (TPS22996); on-demand only |
| HAL_SIG | P0.30 | Input | Hall effect wake pin |
| LIMIT_SIG | P0.26 | Input | Reference/limit switch |
| SERVO_SIG | P1.14 | Output/PWM | Servo PWM signal (PWM1, 20 ms period) |
| SERVO_SW | P1.12 | Output | Servo power gate |
| BATTERY_V | P0.05 / AIN3 | Analog input | Battery voltage divider |

---

## Firmware Architecture

- **Finite State Machine** for door logic and transitions
- **Non-blocking servo control** via `k_uptime_get()` timestamps
- **Two-stage proximity detection**: capacitive edge triggers IR confirmation window
- **BLE GATT service** for remote monitoring
- **NVS-backed persistent counters** with write-queue for BLE coordination
- **Hardware watchdog** for automatic recovery from stalls
- **Per-rail power gating** via TPS22996DRLR

---

## State Machine

### States

| State | Description |
|---|---|
| `STATE_INIT` | Boot or wake: re-enables sensor power rails, determines initial door state |
| `STATE_OPEN` | Stall is open; enters sleep on timeout |
| `STATE_CLOSED_ONE` | Stall closed; waiting before locking |
| `STATE_LOCKING` | Executing lock servo sequence; BLE stack starts at end of this state |
| `STATE_CLOSED_TWO` | Post-lock idle; monitors for hand detection or 20-minute timeout |
| `STATE_DISCONNECTING` | Waiting for graceful BLE teardown (up to 2 s) before entering sleep |
| `STATE_SLEEP` | System OFF (SYSTEMOFF ultra-low power); wakes on hall interrupt |

### Transitions

```text
         +------------+
         | STATE_INIT |  ← sensor power re-enabled on every entry
         +-----+------+
               |
         hall? ↓  else ↓
     CLOSED_ONE   STATE_OPEN
               |           \
         timeout             hand detected
               ↓                ↓
         STATE_LOCKING  ←───────┘
        (BLE starts here)
               |
          lock complete
               ↓
        STATE_CLOSED_TWO
         /            \
    hand detected    20-min timeout (status→5)
         ↓                ↓
      unlock        STATE_DISCONNECTING
         ↓           (wait BLE teardown)
    STATE_INIT            ↓
                     STATE_SLEEP
```

### Manual Lock (Reference Switch)

- **Activation**: Reference switch 0→1 rising edge while in `STATE_INIT` or `STATE_CLOSED_ONE`
- **Behavior**: Skips servo movement and transitions directly to `STATE_CLOSED_TWO` (locked). The reference switch crossing during the servo's normal lock travel is suppressed to avoid a spurious re-trigger.
- **BLE status value**: `2` (LOCKING) — same as an auto-lock sequence

> `STATE_MANUAL_LOCK` has been removed from the firmware; manual-lock behaviour is now handled inline within the main state-machine loop.

---

## Peripheral Integration

### Dual-Sensor Proximity Detection

Hand presence uses a **two-stage confirmation** strategy to minimise false triggers and reduce power consumption:

1. **Primary trigger — MTCH101 capacitive edge**: Any state change (HIGH→LOW or LOW→HIGH) on the sensor output arms the IR confirmation window and immediately powers up the IR rail.
2. **Confirmation gate — IR sensor**: The IRM-H638T must detect a reflected IR burst within `IR_CONFIRM_WINDOW_MS` (500 ms). If it does, hand presence is confirmed and the state machine triggers.
3. **Fallback**: If IR does not confirm within the window, the cap-touch event alone triggers (graceful degradation if IR is obstructed).
4. **Post-trigger cooldown**: After any confirmed detection a 5-second cooldown (`DETECTION_COOLDOWN_MS`) suppresses further cap-touch edges and IR arming, preventing the servo movement from re-triggering.

The IR rail (`IR_SW_EN`) is held **OFF** by default and powered only on demand from the MTCH101 interrupt, then shut off after confirmation or timeout. This significantly reduces idle current.

```
MTCH101 edge ISR
    │
    ├─ IR_SW_EN → HIGH (immediate, from ISR)
    └─ cap_any_edge_pending = true
           │
    main loop
           ├─ reset IR lockout + burst state
           ├─ hand_sensor() → cap_fired = true
           ├─ ir_sensor()   → ir_fired?
           │
           ├─ ir_fired within 500 ms → CONFIRMED → state machine
           ├─ timeout (500 ms)       → CAP-ONLY fallback
           └─ IR_SW_EN → LOW
```

### MTCH101 Capacitive Touch Sensor

- **Signal pin**: P1.09 (`CAP_TOUCH_SIG`) — active-low, interrupt on both edges
- **Sensitivity (MTSA)**: Set by a fixed hardware resistor divider on the PCB (R1 = R2 = 100 kΩ: R1 from VDD→MTSA, R2 from MTSA→GND), giving V_MTSA = 1.65 V (50% VDD). P0.14 (`PWM_MTSA`) is **not connected to MTSA** on this board revision and is left as a high-impedance input.
  - To increase sensitivity (more range): reduce R1 or increase R2 (lower V_MTSA, toward ~40% VDD minimum)
  - To decrease sensitivity (fewer false triggers): increase R1 or reduce R2 (raise V_MTSA)
  - Avoid V_MTSA below ~40% VDD (~1.3 V): the AEC becomes over-sensitive on a large 3″ pad and re-baselines against environmental noise
- **MTPM pin**: P0.08 — held HIGH for normal scan rate (~69–105 ms cycle). Pulling LOW switches to low-power mode (~572–845 ms cycle).
- **Power rail**: `CAP_SW_EN` (P0.21) via TPS22996; enabled at startup. With `CAP_ALWAYS_ON=1` (current setting) the rail stays **on through SYSTEMOFF sleep** — GPIO output state is retained in the nRF52840 always-on S0 domain, so the MTCH101 maintains a live AEC baseline during sleep, eliminating calibration warm-up time on every wake.

### IR Proximity Sensor

- **Emitter**: IR26-21C/L110/CT (940 nm) driven via `IR_LED_PWM` (P1.04, PWM2, 38 kHz NEC carrier)
- **Receiver**: IRM-H638T/TR2 (38 kHz, 940 nm) on `IR_RECV_SIG` (P1.02), active-low output
- **Burst mode**: 10 ms on / 40 ms off cycles to avoid IRM-H638T AGC saturation
- **NEC carrier duty**: `NEC_CARRIER_DUTY_PERMILLE = 5` (0.5%) — kept low to minimise switching noise
- **Power rail**: `IR_SW_EN` (P0.25) via TPS22996; off by default, on only during detection window

> **PCB Revision Note** — Electrical crosstalk was confirmed between the IR LED drive trace (P1.04) and the original IR receiver trace (P1.02) when they ran in parallel on the same layer. The receiver trace was re-routed, resolving the coupling. Firmware flag `ENABLE_IR_SENSOR = 1` enables IR confirmation; set to `0` to fall back to cap-touch-only detection. If crosstalk re-appears on future board revisions, the PCB fixes are:
> 1. Route LED drive and receiver signal on separate layers with a GND plane between them.
> 2. Add a ground-guard trace alongside the receiver trace.
> 3. Maximise physical separation; cross traces at 90° if same-layer routing is unavoidable.

### Power Rail Control

The **TPS22996DRLR** load switch controls two independent sensor rails:

| Rail | Enable pin | Default state | Powered by |
|---|---|---|---|
| CAP_SW_EN | P0.21 | ON at boot | `set_sensor_power()` |
| IR_SW_EN | P0.25 | OFF | `set_ir_power()` — on demand only |

Both rails are turned off in `STATE_SLEEP`. Both are restored when `STATE_INIT` is entered (boot, hall wake, or state-machine reset).

### Servo Control

- **PWM1**, 20 ms period, pulse width mapped to angle (0°–180°)
- `SERVO_MIN_PULSE_US = 500 µs` (0°) → `SERVO_MAX_PULSE_US = 2400 µs` (180°)
- Power-gated via `servo_power_pin` (P1.12)
- Non-blocking: move start time stored, completion checked each main-loop iteration

```c
servo_move_angle(SERVO_LOCK_ANGLE);    // 180° — locked
servo_move_angle(SERVO_UNLOCK_ANGLE);  //   0° — unlocked
servo_move_angle(SERVO_IDLE_ANGLE);    //  94° — mid/idle
servo_move_angle(SERVO_HOLD_ANGLE);    //  43° — hold
```

### Status LEDs

Three independent RGB LED banks; all GPIOs are **active-low** (low-side driven).

| Bank | R pin | G pin | B pin |
|---|---|---|---|
| LED1 | P1.10 | P1.13 | P1.15 |
| LED2 | P0.13 | P0.15 | — (re-routed) |
| LED3 | P1.06 | P0.09 | P1.07 |

> P0.09 (LED3_G) and P0.10 are NFC antenna pins on nRF52840. `CONFIG_NFCT_PINS_AS_GPIOS=y` in `prj.conf` releases them for GPIO use.

### Battery Monitoring

- ADC input **AIN3** (P0.05), internal 0.6 V reference, 1/6 gain, 12-bit resolution, 4× oversampling
- Voltage divider factor applied in firmware; result reported in millivolts over BLE
- Low-battery threshold triggers LED warning and sleep entry

```c
float voltage = read_adc_voltage();          // Returns scaled voltage (V)
battery_voltage_mv = (uint16_t)(voltage * 1000.0f);
```

### BLE GATT

Custom primary service UUID: `c56a1b98-6c1e-413a-b138-0e9f320c7e8b`

| Characteristic | UUID | Properties | Description |
|---|---|---|---|
| Stall Status | `47d80a44-c552-422b-aa3b-d250ed04be37` | Read | Operational state (uint16) |
| Battery Voltage | `7d108dc9-4aaf-4a38-93e3-d9f8ff139f11` | Read | Battery level in mV (uint16) |
| Sensor Counts | `3e4a9f12-7b5c-4d8e-a1b2-9c8d7e6f5a4b` | Read | Persistent trigger counts (3× uint32) |

> **No notifications**: All characteristics are READ-only. There are no CCCD descriptors; clients must poll.

**BLE startup is deferred**: The BLE stack does not initialise at boot. It starts only after the first complete lock sequence (`STATE_LOCKING` completion). This keeps the BT stack silent during critical sensor initialisation. Status values `0` and `1` are never visible over BLE in normal operation.

**TX power**: Advertising at 0 dBm; connection TX raised to +4 dBm via Nordic SoftDevice vendor HCI commands (`CONFIG_BT_LL_SOFTDEVICE=y`).

**LE Coded PHY**: Requested via HCI `LE Set PHY` on every new connection for extended range; falls back to 1M PHY if the central does not support Coded PHY.

#### Stall Status Values

| Value | Meaning |
|---|---|
| 0 | UNKNOWN (pre-BLE-startup) |
| 1 | INIT / idle (pre-BLE-startup) |
| 2 | LOCKING (auto-lock or manual reference-switch lock) |
| 3 | UNLOCKING / STATE_INIT entered after wake |
| 4 | SLEEP (SYSTEMOFF entry) |
| 5 | PRE_SLEEP (20-minute idle timeout; BLE disconnect imminent) |

#### Sensor Counts Structure

```c
struct sensor_counts {
    uint32_t limit_switch_triggers;  // Door position switch activations
    uint32_t cap_touch_triggers;     // Capacitive touch sensor detections
    uint32_t hall_sensor_triggers;   // Hall effect sensor changes
};
```

> **Note**: This field was previously named `ir_sensor_triggers`. It now tracks the MTCH101 capacitive touch sensor, which is the primary detection source. IR confirmation events are not separately counted.

**For complete BLE integration details, see [BLUETOOTH_API.md](BLUETOOTH_API.md)**

---

## Power Management

| Mechanism | Detail |
|---|---|
| System OFF | `NRF_POWER->SYSTEMOFF = 1` — deepest sleep; wakes on hall interrupt |
| MTCH101 rail (CAP_SW_EN) | Stays **HIGH** through SYSTEMOFF (`CAP_ALWAYS_ON=1`); GPIO state retained in nRF52840 S0 domain; MTCH101 AEC baseline preserved across sleep cycles |
| IR rail (IR_SW_EN) | OFF during sleep; powered on demand only during detection window |
| Servo gating | `servo_power_pin` driven low when servo is idle |
| PWM halt | Servo and IR LED PWM stopped before SYSTEMOFF |
| SAADC halt | `NRF_SAADC->TASKS_STOP` called before SYSTEMOFF |
| Auto-sleep timer | 20-minute idle timeout in `STATE_CLOSED_TWO`; triggers `STATE_DISCONNECTING` → `STATE_SLEEP` |
| Manual lock timeout | 30-minute timeout (`MANUAL_LOCK_TIMEOUT_MS`) |
| Sensor restore | `STATE_INIT` re-enables IR rail and resets detection state on every entry (boot or wake) |
| BLE before sleep | `STATE_DISCONNECTING` waits up to 2 seconds for clean HCI disconnect before SYSTEMOFF |

---

## Bluetooth API Documentation

For complete integration details including GATT service spec, UUIDs, data structures, connection management, and hub implementation examples, see **[BLUETOOTH_API.md](BLUETOOTH_API.md)**.

---

## Development Notes

### Required Devicetree Overlay Properties (`zephyr,user`)

```
red_led-gpios, green_led-gpios, blue_led-gpios         # LED bank 1
red_led2-gpios, green_led2-gpios                       # LED bank 2 (no blue — re-routed)
red_led3-gpios, green_led3-gpios, blue_led3-gpios      # LED bank 3
hall_wake_pin-gpios                                    # Hall effect
mtch101_signal-gpios                                   # CAP_TOUCH_SIG
mtch101_mtpm-gpios                                     # MTCH101 power-mode pin
pwms (index 0: PWM0/P0.14 hi-Z, index 1: IR_LED/PWM2) # PWM entries (PWM0 allocated but unused; MTSA set by resistor divider)
ir_recv_signal-gpios                                   # IR receiver output
cap_sw_en-gpios                                        # Capacitive sensor power rail
ir_sw_en-gpios                                         # IR sensor power rail
servo_power_pin-gpios                                  # Servo power gate
limit-gpios                                            # Reference/limit switch
io-channels                                            # ADC channel (AIN3)
```

### Key `prj.conf` Settings

```kconfig
CONFIG_NFCT_PINS_AS_GPIOS=y    # Required: releases P0.09 (LED3_G) from NFC use
CONFIG_BT_LL_SOFTDEVICE=y      # Nordic SoftDevice BLE controller (enables TX power + Coded PHY vendor commands)
CONFIG_NVS=y                   # Non-volatile counter storage
CONFIG_WATCHDOG=y              # Hardware watchdog (60 s timeout)
CONFIG_LOG_BACKEND_RTT=y       # Segger RTT logging (no UART)
# Optional:
CONFIG_BT_EXT_ADV=y            # Enable extended advertising (Coded PHY advertising range); falls back to legacy if not set
```

### Tunable Constants in `src/main.c`

| Constant | Default | Description |
|---|---|---|
| `IR_CONFIRM_WINDOW_MS` | 500 | Max ms after cap edge to accept IR confirmation |
| `DETECTION_COOLDOWN_MS` | 5000 | Post-trigger suppression window (ms) |
| `IR_BURST_ON_MS` | 10 | IR LED burst duration |
| `IR_BURST_OFF_MS` | 40 | Gap between IR bursts (AGC reset time) |
| `NEC_CARRIER_DUTY_PERMILLE` | 4 | IR carrier duty in ‰ (0.4%); reduce further if crosstalk persists |
| `WDT_TIMEOUT_MS` | 60000 | Watchdog timeout |
| `ADV_INT_MIN / MAX` | 64 / 80 | Advertising interval (×0.625 ms = 40–50 ms) |
| `ADV_TX_DBM` | 0 | Advertising TX power (dBm); requires `CONFIG_BT_LL_SOFTDEVICE=y` |
| `CONN_TX_DBM` | 4 | Connection TX power (dBm); raised after connection establishment |
| `CAP_ALWAYS_ON` | 1 | Keep MTCH101 rail powered through SYSTEMOFF sleep; set to 0 for max battery savings at cost of sensor warm-up on wake |
| `ENABLE_IR_SENSOR` | 1 | Enable IR confirmation (0 = cap-touch-only detection) |

> **Note**: `MTCH101_PWM_DUTY_PERCENT` has been removed. MTSA sensitivity is now set by a fixed hardware resistor divider (R1 = R2 = 100 kΩ → 50% VDD). P0.14 is not connected to MTSA on this PCB revision.

### PWM Instance Allocation

| Instance | Signal | Pin | Period |
|---|---|---|---|
| PWM0 | (unused — P0.14 hi-Z; MTSA set by resistor divider) | P0.14 | — |
| PWM1 | Servo | P1.14 | 20 ms (50 Hz) |
| PWM2 | IR LED 38 kHz carrier | P1.04 | 26 µs (38 kHz) |

PWM1 and PWM2 are on independent nRF52840 instances so each can have an independent period. PWM0 is allocated in the overlay but the pin is left as a high-impedance input; MTSA voltage is set by the hardware resistor divider (see [MTCH101 Capacitive Touch Sensor](#mtch101-capacitive-touch-sensor)).

### Advertising Restart Behaviour

After a BLE disconnect, advertising restarts automatically in the `disconnected` callback. After the 10-minute periodic NVS backup, advertising is stopped briefly and restarted. Both restart paths use `NULL` for the scan response (scan data is embedded via `BT_LE_ADV_OPT_USE_NAME`); passing `sd` alongside `USE_NAME` causes `-EINVAL`. The backup restart path includes one retry with a 100 ms delay for transient stack states.

---

## Build Instructions

Ensure the nRF Connect SDK (v2.8.0) and West are installed and on the PATH.

1. Clone and initialise the workspace:

   ```sh
   west init -l .
   west update
   ```

2. Build for the SmartStall board:

   ```sh
   west build -b smartstallv1refactor_nrf52840
   ```

3. Flash the firmware:

   ```sh
   west flash
   ```

4. View logs via Segger RTT (J-Link):

   ```sh
   JLinkRTTViewer
   ```

---

## Watchdog

- **Timeout**: 60 seconds (`WDT_TIMEOUT_MS`)
- **Feed**: Once per main-loop iteration via `watchdog_feed()`
- **Action on timeout**: Full SoC reset
- `&wdt { status = "okay"; }` in the board overlay enables the hardware WDT peripheral
- `zephyr,watchdog = &wdt` in the overlay `chosen` node is the primary device lookup; `DEVICE_DT_GET_ANY(nordic_nrf_wdt)` is the fallback
- Enabled via `CONFIG_WATCHDOG=y` in `prj.conf`

---

## License

MIT License. See [LICENSE](LICENSE) file.

## Contact

For hardware schematics, customisations, or support, reach out to the SmartStall engineering team.
