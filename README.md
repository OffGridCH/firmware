# OffGrid Firmware

Heltec WiFi LoRa 32 V3 firmware with real encrypted storage, authenticated
provisioning/unlock/recovery, persistent QR pairing, encrypted direct messages,
receipts/retries, replay protection and bounded keyless relaying.

**The mock backend and mock-only profiles/checks have been deleted.** All normal
builds use the secure domains; no authentication or delivery-success bypass
exists. Physical multi-hop and release-hardening gates below remain outstanding.

## Build and upload

PlatformIO Core 6.x; pinned ESP32, RadioLib, ArduinoJson, U8g2 and NeoPixel
dependencies are in [platformio.ini](platformio.ini).

```sh
pio run -e heltec_wifi_lora_32_V3
pio run -e native_usb
pio run -e heltec_wifi_lora_32_V3 -t upload --upload-port /dev/ttyUSB0
```

- Default `heltec_wifi_lora_32_V3`: secure UART through CP2102.
- `native_usb`: secure native USB Serial/JTAG.
- `secure_dev` / `secure_dev_usb`: compatibility aliases of those secure builds.
- `radio_diag` / `radio_diag_usb`: real secure firmware plus isolated fixtures,
  ciphertext trace and receipt-loss injection. Never install lab profiles for
  normal use. Normal firmware compiles these commands and fixtures out.

Opening CP2102 on the tested boards resets the MCU. This is a hardware reset,
not evidence of a power-cut test.

## Security and storage

The registered app proves a token through a single-use X25519/HKDF/AES-GCM
session. The token is never stored in firmware flash. Versioned AES-GCM vault
records persist the private identity and contact keys. Wrong credentials fail
closed, with backoff; lock/timeout clears identity/contact keys, pairing and own
queued plaintext. Token rotation atomically activates a verified generation.

Pairing uses out-of-band QR material, real X25519/directional HKDF keys, a
role-independent fingerprint and stable encrypted contact slots. Fresh data and
ACKs share reserved 24-bit counters; reboot skips the unused reservation rather
than reusing a nonce. Counters never wrap. Authenticated replay acceptance is
committed before delivery; endurance/power-cut acceptance is still a release gate.

The ESP32 SAR-ADC entropy source is enabled before cryptographic operations
because external LoRa does not supply internal Wi-Fi/BLE entropy. Future
ADC/internal-radio/I2S integration must coordinate that entropy source rather
than enabling conflicting peripherals.

See [protocol](docs/offgrid-protocol.md), the complete
[serial API](docs/offgrid-link.md) and [storage](src/storage/README.md).
Static v1 keys do not provide forward secrecy; deleting NVS records is not a
forensic erase guarantee.

## OLED and LEDs

Locked firmware displays a large closed padlock. A genuine unlock opens the
shackle, then shrinks/moves the open lock to the Home corner over 1.4 seconds.
Relocking cancels it and replaces pending frames. Rendering and I2C updates are
cooperative; host/RF work never waits for the animation.

GPIO35 PWM and two four-pixel GRB/800-kHz strips on GPIO45/46 share configurable
monochrome patterns, brightness and quiet/disabled settings. Defaults: locked
slow blink, unlocked off except activity. GPIO45/46 are strapping pins: external
power/boot integrity and optical appearance still require physical inspection.
No GPIO0 button workflow exists. See [drivers](src/drivers/README.md).

## Real-device acceptance tools

The checker requires `pyserial` and `cryptography`. It refuses real credentials
on old simulator images and never silently erases an existing vault.

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --secure-check --allow-secure-changes
python3 tools/serial-check.py --port /dev/ttyUSB0 --contact-check --allow-secure-changes
python3 tools/serial-check.py --port /dev/ttyUSB0 --led-check --allow-secure-changes
python3 tools/serial-check.py --port /dev/ttyUSB0 --ui-check
python3 tools/serial-check.py --port /dev/ttyUSB0 --framing-check
python3 tools/serial-check.py --port /dev/ttyUSB0 --mesh-peer /dev/ttyUSB1 \
    --allow-secure-changes --allow-radio-transmit
```

Secure acceptance retains each synthetic token privately in
`~/.config/offgrid/device-<device_id>.json` (0600). Preserve it; never commit or
share it. This host credential is not a replacement for the Android Keystore.
Two-board checks use existing credentials, remove only their test relationships,
restore temporary settings and lock both boards. They refuse replacing existing
contacts. On lab images they additionally lose a real ACK and verify physical
retry/re-ACK without redelivery, exact eight-slot host queue bounds, cancellation,
lock and deletion cleanup, and actual ciphertext trace.

RF acceptance requires antennas and lawful local settings. The supplied check
uses 869.525 MHz/BW125/SF9/CR4/5, 0 dBm and CAD. Shared data/ACK/relay scheduling
waits 99 times measured TX airtime after transmission, including interrupted/
failed TX with known airtime. This conservative 1% per-boot pacing is not a
durable rolling-hour ledger or regulatory certification across resets.

For lab-only, production-namespace-independent diagnostics:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.codec.selftest --allow-secure-changes
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.vault.selftest --allow-secure-changes
```

## Authorized buttonless factory reset

The registered app needs fresh action-specific token proof with reset label
`og-reset-wrap-v1`; an ordinary unlock proof cannot erase a vault.

Owner maintenance instead signs a fresh, device-bound, expiring single-use
challenge with an operator-held P-256 private key:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --master-reset --confirm-erase
```

This **erases that board's vault/settings**. The firmware embeds only the fleet
public trust key; the private key is outside the repository under
`~/.config/offgrid/master-reset.pem` (0600). Back it up securely: possession grants
reset authority over devices trusting that key. No eFuses/download mode are
changed, and no unrelated namespace/partition is erased. Reset authorization is
journaled before response and resumed on reboot if interrupted.

`--secure-check --allow-secure-changes --confirm-erase` tests app/master reset
using synthetic data, restores backed-up settings and leaves a new locked vault.
Use it only on an explicitly authorized disposable test identity.

## Acceptance and release status

UART, native-USB and secure lab builds pass. Two physical boards passed real
TEXT/REACTION/GEO and bound receipts in both directions, contact persistence,
lost-receipt retry without duplicate delivery, queue/lock/delete cleanup, OLED
scene/latency checks and real lifecycle/reset/LED software checks.

Remaining external release gates: three/four-board forwarding/suppression,
power-cut and flash/RF fault injection, prolonged host/traffic pressure and flash
endurance, physical LED/OLED/power inspection, jurisdiction-specific RF
certification, signing/update/recovery policy, and deliberate Secure Boot/flash
encryption/debug-access choices. No irreversible fuse changes were made.
These are external acceptance/production-hardening gates, not missing two-device
messaging features. Multi-hop forwarding is implemented; its physical
three/four-board acceptance is still outstanding.

## Documentation

- [Serial API](docs/offgrid-link.md): implemented commands, authentication,
  events, errors, reset authorization and app integration.
- [Radio protocol](docs/offgrid-protocol.md): encrypted wire format and vectors.
- [Hardware drivers](src/drivers/README.md),
  [host interface](src/host/README.md) and
  [storage format](src/storage/README.md): maintained implementation references.

Obsolete implementation plans, mock/UI handoffs and target-architecture drafts
have been removed; the references above describe the shipped implementation.
