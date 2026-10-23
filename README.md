# Offgrid Firmware

Firmware for the Heltec WiFi LoRa 32 V3. This branch currently builds a
**serial development mock**, not a secure modem. Use synthetic data only.

## Current Stage

Serial mock Stage 2 implements bounded JSON Lines communication, system commands,
hello/session envelopes, mock provisioning/unlock/lock, heartbeat, policy, simulated
reboot, and physically confirmed mock reset. Status and boot events include
`mock: true` and `mock_stage: 2`. Firmware version is `0.1.0-serial-mock.2`.

The mock validates wrapper shapes but **bypasses authentication**. Pairing,
contacts, messages, and configuration commands are not implemented yet.
Radio, LEDs, strips, and OLED are not initialized. No application data is written
to flash. See the [host README](src/host/README.md) for supported commands and
the RAM-only persistence/simulated reboot limitations.

See [serial stages and device checks](docs/serial-mock-milestones.md) for commit
boundaries and acceptance scenarios. The [host specification](docs/offgrid-link.md)
describes the complete target API, not the subset already implemented.

## Build and Upload

Use PlatformIO Core or the PlatformIO terminal in VS Code. Versions and C++17
mode are configured in `platformio.ini`. Find the actual port with `pio device list`.

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
