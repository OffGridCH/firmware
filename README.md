# Offgrid Firmware

Firmware for the Heltec WiFi LoRa 32 V3. This branch currently builds a
**serial development mock**, not a secure modem. Use synthetic data only.

## Current Stage

Serial mock Stage 5 implements bounded JSON Lines communication, system commands,
hello/session envelopes, mock provisioning/unlock/lock, heartbeat, policy, simulated
reboot, and physically confirmed mock reset. Status and boot events include
`mock: true` and `mock_stage: 5`. Firmware version is `0.1.0-config-store.1`.

The mock validates wrapper shapes but **bypasses authentication**. Public identity,
QR pairing, 32-slot contact CRUD, typed messages, pending/cancel, and simulated
incoming/delivery/failure events are implemented. Radio/mesh/LED configuration,
synthetic statistics/logs/traces, and real NVS configuration persistence are available.
Default mock builds do not initialize radio, LEDs, strips, or OLED. Only configuration and timeout
policy are stored in flash; identity, tokens, contacts and messages remain mocks.
See the [host README](src/host/README.md) for API behavior and the
[storage README](src/storage/README.md) for the record format and device checker.

Storage is implemented in separate module, integration and checker/documentation
commits. The UART configuration image compiles; physical persistence checks
must be run on your board with the single Python checker below.

The [host specification](docs/offgrid-link.md)
describes the complete target API, not the subset already implemented.

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
The optional `--storage-phase reset` checks physically confirmed configuration
erasure; see the storage README before running it.

## Build and Upload

Use PlatformIO Core or the PlatformIO terminal in VS Code. Versions and C++17
mode are configured in `platformio.ini`. Find the actual port with `pio device list`.

For VS Code C++ IntelliSense, generate the local ESP32 compilation database with
`pio run -e heltec_wifi_lora_32_V3 -t compiledb`. The checked-in editor configuration
uses that database rather than generic Arduino/AVR include paths.

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

Replace the example ports as needed. Both profiles set `OFFGRID_MOCK_DEVICE=1`;
the mock entry point deliberately refuses to compile without it. Neither is a
production build. Do not assume a connector exposes both USB interfaces.

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
