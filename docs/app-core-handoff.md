# App Core Handoff: LED and OLED

## Starting Point

Base both UI branches/worktrees on the completed `feature/firmware-core` branch.
No worktrees are created yet. Authentication, contacts and messaging remain mocked;
NVS configuration is real. Only diagnostic profiles drive the actual SX1262.

`main.cpp` only calls `App::init()` and `App::tick()`. App owns serial, commands,
the temporary `host/MockDevice.h` backend, both UI drivers, and diagnostic radio
when enabled. Keep direct calls/typed callbacks, with no new tasks or event bus.

## Independent Work

| Owner | Primary files | Deliverable |
|---|---|---|
| LED agent | `src/drivers/LedDriver.h`, `src/drivers/LedDriver.cpp` | Onboard PWM LED, confirmed strips, patterns and preview |
| OLED agent | `src/drivers/OledDriver.h`, `src/drivers/OledDriver.cpp` | SSD1306 initialization, Home, timed pairing and error overlays |

Keep the public interfaces intact. Coordinate changes to `Types.h`, App,
`BoardConfig.h` and `platformio.ini` rather than editing these independently.
Add verified hardware pins/pixel counts to BoardConfig; UI pins/counts are not
defined there yet. Verify board revision, OLED power/reset wiring and external
strip wiring before selecting pins or libraries. Use the
[architecture](offgrid-firmware-architecture_v1.md) and
[host specification, section 7.8](offgrid-link.md) for presentation requirements.

## Driver Contract

- `init()` runs once. Return true only when usable. The current stubs return
  false; App skips all subsequent calls to that unavailable driver. Failure
  must not halt serial, radio or the other UI driver. No automatic retry exists.
- Each loop samples monotonic 64-bit milliseconds once, ticks the backend, handles
  bounded serial work, updates UI values, then ticks LED and OLED. UI updates
  continue without a connected native-USB host.
- `show(const UiSnapshot&)` runs every loop for a ready driver. Copy only needed
  values. It must not restart animations or repaint the display on every call.
- `onEvent(UiEvent, nowMs)` is a one-shot notification. Record pending effects;
  events may arrive before that loop's fresh `show()`.
- `tick(nowMs)` advances animations or bounded display/strip transfers. No
  `delay()`, waits for an entire animation, serial prints, JSON, NVS or radio calls.
- LED `configure(const LedConfig&)` runs at startup and after accepted LED changes,
  simulated reboot or reset. Copy settings; do not retain references.
- LED `preview(const LedPattern&, nowMs)` runs once after a valid `led.test`.
  Copy it immediately; the caller's value expires after the call. Outer repeat
  wins (default 1); preview never saves settings.

All shared types are in [Types.h](../src/Types.h). Snapshots contain fixed copied
values, never keys, tokens, JSON views or message payloads. Identity, counts and
the last completed pairing fingerprint are hidden while locked. The fingerprint
clears on a new pairing, lock or simulated reboot. `mock` remains true even in
diagnostic images; `radioDiagnostics` distinguishes actual radio telemetry from
synthetic mock counters/signal values. Battery telemetry is not available yet.

State pattern indexes match `DeviceState`: unprovisioned, locked, unlocked.
Event pattern indexes match the first seven `UiEvent` values: tx, rx,
msg_received, delivered, failed, error, pairing. Boot has no
configured LED slot; never index `eventPatterns` with them. State patterns loop;
event patterns honor repeat. Custom steps alternate on/off beginning with on.
Enforce enabled/quiet/brightness according to the host specification.

## Buttonless UI and Events

Do not configure or sample GPIO0 anywhere in application code. Button support
and its placeholder are removed. Home stays the default page. Active pairing
automatically overlays it; completed fingerprints remain for 15 seconds, errors
for 3 seconds and RF/message activity for 5 seconds. No host paging command is
introduced. `sys.factory_reset` returns `E_BUSY` with no arm window or erasure,
pending an owner-approved maintenance/recovery policy.

App fans out Boot, accepted pairing start and dispatched command
errors. The backend reports mock message TX/RX/delivery/failure and drained real
radio TX/RX/error events. Mock message progress still follows serial backpressure;
diagnostic serial backpressure can also delay draining later radio events.
Snapshot counters remain the current source of truth, not event counts.

## Build and Device Check

Use PlatformIO Core 6.x; on the current machine the working executable is
`/tmp/offgrid-platformio/bin/platformio` (the system `pio` is obsolete).

```sh
pio run -e heltec_wifi_lora_32_V3
pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/ttyUSB0
python3 tools/serial-check.py --port /dev/ttyUSB0 --command sys.status
python3 tools/serial-check.py --port /dev/ttyUSB0 --command led.test --args '{"pattern":"flash","repeat":2}' --allow-mock-changes
```

`sys.status.result.ui` reports led_ready, oled_ready and numeric page (0..2).
OLED readiness is true on the connected board; LED remains a stub. Check timed
overlays, idle updates with the host disconnected and independent driver failure
on hardware. Use synthetic pairing data; factory reset must not erase configuration.
Commit each driver in small buildable stages. No new test suite is needed.

Core verification: diagnostic UART/mock native-USB builds passed. On-device status
confirmed loaded NVS, radio standby and unavailable UI stubs, without RF TX.
Physical visual rendering acceptance remains pending; no button acceptance applies.