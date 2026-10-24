# Offgrid Firmware

Firmware for the Heltec WiFi LoRa 32 V3. Explicit development profiles provide a
**real persistent encrypted vault and direct-message core** or the older serial
mock. Two-board delivery and relaying acceptance are pending; these are not
production-ready modem images.

## Real Vault and Recovery

```sh
pio run -e secure_dev -t upload --upload-port /dev/ttyUSB0
python3 tools/serial-check.py --port /dev/ttyUSB0 --secure-check --allow-secure-changes
```

`secure_dev_usb` is the native-USB counterpart. These profiles compile without
MockDevice and never bypass authentication. They implement real X25519-wrapped
token provisioning/unlock, encrypted identity persistence, key cleanup, timeout,
failure backoff and two-generation token rotation. Contacts/pairing derive and
persist real relationships. Real `msg.*` routes use bounded queues, authenticated
CCM packets/ACKs and retries driven by hardware TX completion. Relay scheduling
is not implemented yet; no synthetic success is used in secure firmware.

The isolated vault diagnostic also tests the direct-message domain without RF:
eight-slot bounds, wrong ACK rejection, collision candidate verification,
authenticated replay, fresh re-ACK counters, all three retries/no fourth retry,
timeouts, cancellation, contact deletion, lock cleanup and bounded event loss.
These checks are not proof of physical delivery; a second board is needed next.

The checker retains a synthetic device token privately under `~/.config/offgrid/`
(mode 0600). Back it up; never commit or share it. This host test credential is
not an Android Keystore replacement. Normal firmware never stores the token.
The original saved settings are backed up there and restored after acceptance.

Factory reset in secure profiles requires fresh app-token authentication using
the action-specific `og-reset-wrap-v1` label, or an owner-signed 30-second
single-use device challenge. The firmware contains only the fleet public trust
key. The authorized owner private key was generated outside the repository at
`~/.config/offgrid/master-reset.pem`, mode 0600. **Back up this private key
securely**; anyone holding it can erase devices trusting its public key.

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --master-reset --confirm-erase
```

That maintenance command erases the device's vault and configuration and does
not restore them. It works without the app token, including when vault loading
fails, provided NVS and the trusted firmware still function. It does not burn
eFuses or erase unrelated namespaces. `--secure-check --allow-secure-changes
--confirm-erase` instead tests both reset authorities with synthetic vault data,
restores saved settings, then leaves a fresh real vault locked with its retained
local credential. Do not use that acceptance mode on personal identities.

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --contact-check --allow-secure-changes
```

This uses the retained real credential, creates one random synthetic peer,
checks self/duplicate/replacement handling, fingerprint and encrypted contact
CRUD across MCU reboot, deletes only its fixture, and locks. It refuses a full
table rather than deleting existing contacts. Two-board key agreement is pending.

UART/native-USB builds and connected-board lifecycle/reset checks pass. Tested
invalid signature, consumed challenge, wrong action/token, session reuse, rate
limit, rotation, heartbeat lock, MCU reboot identity persistence and settings
restoration. Actual power-cut fault injection and multi-board acceptance remain.

## Current Stage

Serial mock Stage 5 implements bounded JSON Lines communication, system commands,
hello/session envelopes, mock provisioning/unlock/lock, heartbeat, policy, simulated
reboot, and fail-closed reset rejection on buttonless hardware. Status and boot events include
`mock: true` and `mock_stage: 5`. Firmware version is `0.1.0-config-store.1`.

The mock validates wrapper shapes but **bypasses authentication**. Public identity,
QR pairing, 32-slot contact CRUD, typed messages, pending/cancel, and simulated
incoming/delivery/failure events are implemented. Radio/mesh/LED configuration,
synthetic statistics/logs/traces, and real NVS configuration persistence are available.
Default mock builds initialize OLED, onboard LED and two four-pixel strips but not radio. Only configuration and timeout
policy are stored in flash; identity, tokens, contacts and messages remain mocks.
See the [host README](src/host/README.md) for API behavior and the
[storage README](src/storage/README.md) for the record format and device checker.

Storage is implemented in separate module, integration and checker/documentation
commits. The UART configuration image compiles; physical persistence checks
must be run on your board with the single Python checker below.

The [host specification](docs/offgrid-link.md)
describes the complete target API, not the subset already implemented.

## App Core and UI Work

`App` now owns initialization and the cooperative loop; `main.cpp` only delegates.
The temporary mock backend is in `src/host/MockDevice.h`. LED/OLED receive copied
public snapshots and typed events, with LED settings/preview already connected.
OLED uses the merged U8g2 renderer with cooperative startup and dirty-row
transfers; LED output uses PWM and two GRB 800 kHz NeoPixel strips. Readiness is reported through
`sys.status.result.ui`. A failed peripheral never blocks the other modules.
See the short [LED/OLED handoff](docs/app-core-handoff.md) for independent work
boundaries, driver contracts, automatic overlays and device checks. GPIO0 is not
an application input. The legacy mock rejects reset (`E_BUSY`); secure profiles
use the authenticated recovery policy above.

## Radio Bring-Up

`RadioDriver` now wraps pinned RadioLib 7.8.1 for real SX1262 initialization,
asynchronous RX/TX, CAD/backoff, deadlines, configuration and recovery. Select
the separate diagnostic image to use it; the Android mock remains available.

```sh
pio run -e radio_diag -t upload --upload-port /dev/ttyUSB0
```

Flash two boards, attach correct antennas and verify lawful RF settings. Then:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --radio-peer /dev/ttyUSB1 --allow-radio-transmit
```

The diagnostic image rejects mock chat sends and exposes raw diag.radio.*
commands. It is not encrypted messaging and does not enforce duty cycle.
See [radio setup and device checks](src/drivers/README.md) for commands,
events, pins, timing limits, native-USB alternative and acceptance gates.
Firmware builds pass; physical two-board verification remains pending.

## Storage Device Check

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase write --allow-config-changes
# Physically reset or power-cycle the board before the next command.
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase verify --allow-config-changes
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase restore --allow-config-changes
```

Requires `pyserial`. Keep the generated snapshot until restore succeeds.
The optional `--storage-phase reset` checks reset rejection and unchanged
configuration; it performs no erase. `--ui-check` measures serial responsiveness
across startup and Home rendering without changing settings. `--led-check
--allow-mock-changes` verifies output levels and preview/quiet/enabled behavior
using temporary settings, then restores them.

## Build and Upload

Use PlatformIO Core or the PlatformIO terminal in VS Code. Versions and C++17
mode are configured in `platformio.ini`. Find the actual port with `pio device list`.

For VS Code C++ IntelliSense, generate the local ESP32 compilation database with
`pio run -e heltec_wifi_lora_32_V3 -t compiledb`. The checked-in editor configuration
uses that database rather than generic Arduino/AVR include paths. U8g2 2.36.15,
ArduinoJson and RadioLib are inherited by all four profiles.

Default USB-UART bridge image, 115200 baud, 8N1, explicitly using `Serial0`:

```sh
pio run -e heltec_wifi_lora_32_V3
pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/ttyUSB0
pio device monitor -e heltec_wifi_lora_32_V3 --port /dev/ttyUSB0
```

Alternative native ESP32-S3 USB Serial/JTAG CDC image, only if that interface is
physically routed to your connector and supported by the Android USB driver:

```sh
pio run -e serial_mock_usb
pio run -e serial_mock_usb -t upload --upload-port /dev/ttyACM0
pio device monitor -e serial_mock_usb --port /dev/ttyACM0
```

Replace the example ports as needed. These two mock profiles set
`OFFGRID_MOCK_DEVICE=1`; secure profiles explicitly set it to 0 exactly once.
No profile is release-hardened. Do not assume a connector exposes both interfaces.

Close the monitor before another host application connects. DTR/RTS are disabled
in monitor configuration to reduce unintended resets. ROM boot text can still
appear; clients ignore lines not starting with `{`.

## Quick Check

Send each request on its own line terminated by LF:

```json
{"id":1,"cmd":"sys.ping"}
{"id":2,"cmd":"sys.status"}
```

Expect matching response IDs, increasing `uptime_ms`, `state: "unprovisioned"`,
and `radio.running: false` before mock provisioning. Android developers can now
exercise hello and mock lifecycle flows described in the host README.
