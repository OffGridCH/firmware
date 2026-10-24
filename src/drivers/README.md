# Radio Driver

## OLED lock screen

Locked devices show a large centered closed padlock and "Locked", never the
empty Home placeholders or private identity/contact information. A real
locked-to-unlocked transition opens the shackle over 550 ms, holds briefly, then
shrinks/moves the open lock into the Home corner, completing at 1400 ms.
Relocking cancels the animation and invalidates pending old frame rows. Rendering
and I2C continue cooperatively; authentication/radio work never waits for it.
Locked state takes precedence over boot/error overlays. `sys.status.ui.oled_scene`
reports the most recently rendered scene for acceptance instrumentation.

Two-board secure acceptance checks locked/opening/Home transitions and status
RTT below 250 ms, alongside actual encrypted traffic. Scene checks are not an
optical measurement; final visual appearance needs owner observation.

## LED outputs

Owner-confirmed wiring: onboard GPIO35 PWM, four GRB 800 kHz NeoPixels on GPIO45
and four on GPIO46. Both external pins are strapping pins: external circuitry must
not force incorrect levels at boot. Power/level integrity still needs physical
inspection. Adafruit NeoPixel 1.12.5 owns stable allocated pixel buffers; its ESP32
RMT `show()` is synchronous, not a claimed asynchronous transfer. Each strip sends
only four pixels (120 microseconds of data plus driver overhead); output updates
are limited to 100 Hz and skipped when unchanged/not ready. Measure mixed RF load
separately. UI never touches RF, storage or private data.

State timing continues beneath finite event/preview overrides. Configuration and
preview values are copied, outer repeat wins, quiet/disabled forces all outputs
off, and brightness scales PWM and pixels equally. On/off event cycles last
1000 ms; pulse lasts 1500 ms. State cycles always loop.

`tools/serial-check.py --led-check --allow-mock-changes` passed on the connected
board: readiness, output level, master switches, brightness and preview expiry.
It restores temporary settings and locks. `sys.status.ui.led_duty` is software
driver-level evidence, not an optical or electrical measurement. Visual/power
acceptance remains pending. With both strips active, 134 startup/Home pings had
maximum RTT 16.7 ms, below the exerciser's 250 ms threshold.

`RadioDriver` wraps pinned RadioLib 7.8.1 for the Heltec WiFi LoRa 32 V3 SX1262.
Pins and the 1.8 V DIO3 TCXO supply are defined in `BoardConfig.h`; verify against
your board revision. DIO2 controls the RF switch. LoRa uses explicit headers,
PHY CRC and automatic low-data-rate optimization. The sync word is expanded by
RadioLib with control bits 0x44, so 0x4F becomes 0x44F4.

The default serial mock profiles do not initialize radio hardware. Build
`pio run -e radio_diag` for real hardware diagnostics. Initialization uses saved
settings and stays in standby; `sys.status.radio` reports initialized, state and
the last RadioLib error. `radio_diag_usb` selects native USB Serial/JTAG instead.

`startReceive`, `startTransmit`, `tick`, and `popEvent` implement asynchronous
operations. DIO1 only sets an instance-owned flag under an ISR-safe critical
section; the application loop reads SPI and handles packets. TX/RX buffers hold
up to 255 bytes. Four event slots reserve one slot from RX traffic for operation
completion; excess received events are counted as drops. The caller must drain
events promptly. TX success is reported only after TxDone, and RX is restored
when requested. CAD scans and randomized backoff gate sends by default. Access
has a ten-second deadline, each CAD has a symbol-based deadline, and TX has both
a hardware timeout and a loop watchdog. Backoff is standby, not continuous RX;
packets during these half-duplex operation windows may be missed.

The driver uses RadioLib's staged packet setup, then issues the three-byte
SX1262 SetTx command through its bounded Module SPI interface. It deliberately
does not use startTransmit/launchMode: those convenience paths contain an
unbounded BUSY wait, including in 7.8.1. DIO2 performs this board's RF switching.
Runtime SPI/BUSY waits are capped at 20 ms per library transaction; a multi-command
operation can take longer. Initialization/recovery can take substantially longer
and must not be treated as normal real-time ticks.

`queueConfig` retains the latest requested settings until RX/standby is safe;
TX/CAD/backoff finish first. Configured or Failed events report the outcome.
Hardware failures stop automatic progress. `recover` explicitly reinitializes
the pending or last working settings and returns to standby. It never loops
through repeated reinitialization on every tick.

Initialization/configuration calls can wait inside RadioLib. No strict
non-blocking timing guarantee or real-device acceptance is claimed by a build.
Keep antennas connected and use lawful frequencies/power when RF is enabled.

## Wiring and Profiles

The driver owns SPI, RadioLib, buffers, IRQ state, operation deadlines and PHY
statistics. It accepts typed Config values and exposes typed Event values; it
has no JSON, NVS, serial, mesh or crypto dependency. App owns the driver and
injects it into the temporary `host/MockDevice.h` backend, which adapts serial/NVS
settings. Later `MeshEngine` will use the same typed boundary for encrypted frames.

Default profiles keep the Android mock and leave hardware RF off. Diagnostic
profiles enable real RF, advertise `radio_diagnostics: true` in boot/hello, and
reject msg.* / mock.* with E_BUSY. Identity/authentication remain mock-only;
these builds are not secure chat firmware. Initialization/recovery may take
several seconds; allow a longer host timeout for diag.radio.recover.

```sh
pio run -e radio_diag -t upload --upload-port /dev/ttyUSB0
```

Flash both boards for a two-way check. Substitute actual ports. Use radio_diag_usb
only when native USB is routed and supported by your host driver.

## Diagnostic API

Commands below exist only in diagnostic builds and intentionally work without
mock provisioning. Use synthetic raw bytes, never plaintext personal messages.

| Command | Arguments/result |
|---|---|
| `diag.radio.status` | Initialized, state, last_error, config_pending, applied settings and physical stats |
| `diag.radio.rx` | Start continuous receive |
| `diag.radio.tx` | hex for 1..255 bytes, optional cad boolean and backoff_slots 0..32; returns queued and op_ref |
| `diag.radio.cad` | One asynchronous scan; returns queued and op_ref |
| `diag.radio.stop` | Standby; active operation emits a cancellation error |
| `diag.radio.recover` | Explicit reinitialization using desired settings; returns standby or error |

Events include uptime t and data.diagnostic true:

- diag.radio.rx: hex, len, measured rssi and snr.
- diag.radio.tx: op_ref, hex, len and TX completion duration airtime_ms.
- diag.radio.cad: op_ref and busy boolean.
- diag.radio.error: op_ref when applicable, code E_RADIO and numeric radio_error.
- diag.radio.configured: queued settings successfully applied.

Driver error values: -10000 busy, -10001 not ready, -10002 cancelled, -10003
channel-access timeout; remaining codes are RadioLib errors. A queued response
does not mean transmission succeeded: wait for completion or error.

radio.get/set retain normal mock-unlocked access checks. A diagnostic radio.set
result adds apply_pending: persistent settings commit before queued hardware
application. The latest accepted settings apply after active TX/CAD/backoff.
Application failure emits an error, not a fake configuration success event.
The saved/desired configuration remains available for explicit recovery; inspect
applied and config_pending to distinguish desired from actual hardware settings.

radio.stats uses physical counters in diagnostic builds; mesh/crypto counters
remain zero. CAD scans/busy, failures, timeouts and event drops are added. CRC
counts include PHY header errors. dbg.rf_trace optionally adds raw rf.rx/rf.tx
aliases with action diagnostic. Those low-priority aliases can drop under serial
load; the dedicated diagnostic event is retained and retried by the adapter.

Mock lock/heartbeat timeout stops RF. Simulated reboot
reload settings and return to standby. Diagnostics can explicitly restart RF in
any mock state; this is not a secure locked-relay implementation.

## Two-Board Device Check

Match frequency, bandwidth, SF, CR, preamble and sync word. On board B:

If saved settings differ, set the same preset on each board using single-command
mode (this persists settings and automatically provisions/unlocks only a mock):

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --command radio.set --args '{"preset":"lab_default","persist":true}' --allow-mock-changes
python3 tools/serial-check.py --port /dev/ttyUSB1 --command radio.set --args '{"preset":"lab_default","persist":true}' --allow-mock-changes
```

The tool waits for diag.radio.configured and prints applied settings before
exiting. No firmware reflash is required to add this host-tool mode. Settings
survive reset; mock provisioning does not. Query without changing settings:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.radio.status
```

For manual serial communication, start reception on board B:

```json
{"id":1,"cmd":"diag.radio.status"}
{"id":2,"cmd":"diag.radio.rx"}
```

On board A:

```json
{"id":1,"cmd":"diag.radio.rx"}
{"id":2,"cmd":"diag.radio.tx","args":{"hex":"4F472D44494147","cad":true}}
{"id":3,"cmd":"sys.ping"}
```

Check A's completion and B's exact received bytes with numeric RSSI/SNR. Reverse
directions and confirm automatic RX restoration and serial responsiveness.
The existing single Python checker can perform this on two real ports:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --radio-peer /dev/ttyUSB1 --allow-radio-transmit
```

It verifies distinct initialized boards, matching applied settings, CAD completion,
one synthetic frame per direction, exact bytes, TX completions, signal values,
counter increments and ping while operations run. It finally stops both radios.
It changes no NVS settings. Optional radio-count 1..10 repeats the exchanges;
default is one. Requires pyserial and exclusive access to both serial ports.

Neither firmware nor checker enforces duty cycle. CAD is not regulatory airtime
enforcement. Use lawful power/frequency/airtime, attached matching antennas and
controlled low-power tests with sufficient separation or attenuation.

Manual gates remain: busy-channel backoff, stopped/missing IRQs, cancellation,
settings queued during TX, serial backpressure and recovery. The diagnostic and
default UART images compile and the Python CLI loads; no two-board RF success,
fault injection or hardware timing result is claimed without running on-device.