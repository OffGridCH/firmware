# Host API and Serial Mock

## Real secure profile

`secure_dev` and `secure_dev_usb` do not include this mock backend. Their real
command groups are implemented in `Commands` and call App-owned domain modules.
Lifecycle, identity, settings, authenticated recovery and real contacts/pairing
are wired. Real `msg.*` commands now use MeshEngine, durable per-contact counters
and actual radio completion/error events. Physical two-board delivery acceptance
and relay scheduling are pending; these profiles never fabricate delivery.
See the project README and host specification section 6.11 for secure acceptance
and app/master reset. The owner requested mock removal after domain replacement.

## Current Implementation: Serial Mock With Real Configuration Storage

This folder implements the real JSON Lines transport and command-envelope
boundary. The simulated device backend lives in `MockDevice.h`, owned by `App`, behind
`OFFGRID_MOCK_DEVICE=1`. It is not secure firmware. Use synthetic data only.

| Component | Responsibility |
|---|---|
| `SerialLink` | LF/CRLF framing, 2048-byte wire limit, bounded RX/TX work, complete output lines, queued JSON serialization |
| `Commands` | JSON envelope validation, exact integer IDs, argument-validation helpers, dispatch, response/error formatting |
| `MockDevice` | Command-specific behavior, public identity, mock lifecycle, deferred events, buttonless reset rejection and typed UI adapters |
| `App` | Instance ownership, initialization, cooperative loop and LED/OLED snapshot/event routing |

Request views are valid only during dispatch. Copy any data retained for later
work into owned fixed buffers. Domain implementations can replace the mock
callback without changing the transport. `ConfigStore` now provides real NVS
persistence; security and mesh remain mocked. The separate radio_diag profiles
connect real RadioDriver hardware without changing the default Android mock.

OLED has cooperative startup/rendering; LED drives onboard PWM and two confirmed
four-pixel GRB NeoPixel outputs (GPIO45 and GPIO46).
`sys.status.result.ui` adds `led_ready`, `oled_ready` and numeric `page` (Home=0,
Radio=1, Pairing=2). Home is the default, with automatic timed overlays.
See the [App/UI handoff](../../docs/app-core-handoff.md) for driver integration.

## Radio Diagnostic Images

Only radio_diag/radio_diag_usb expose diag.radio.status/rx/tx/cad/stop/recover.
They advertise radio_diagnostics true in boot/hello and use actual radio state,
packet statistics and raw events. They reject msg.* and mock.* with E_BUSY so
simulated delivery cannot be confused with encrypted radio communication.
The diagnostic commands intentionally work without provisioning; use them only
in controlled lab setups with synthetic bytes. Normal radio.set and radio.stats
retain their specified unlocked access check (authentication is still mocked).

Radio updates are queued after persistent configuration commits. An accepted
set response adds apply_pending; inspect diag.radio.status.applied/config_pending
or wait for diag.radio.configured / diag.radio.error for the hardware outcome.
No new mesh or encryption behavior is supplied. See the
[radio README](../drivers/README.md) for the two-board check and raw event shapes.

## Connection

Build/upload instructions are in the [project README](../../README.md).
Default profile `heltec_wifi_lora_32_V3` uses UART0 through the USB-UART bridge:
115200 baud, 8N1, no flow control. `serial_mock_usb` selects native USB Serial/JTAG
CDC if your connector exposes it and Android supports its USB driver.

On connection, discard input through the first LF, ignore ROM lines not beginning
with `{`, and retry `sys.hello` every 500 ms until answered. UART cannot detect
port-open, so an already-running device may not repeat its boot event. Native
USB attempts boot replay when its connection indicator becomes active.

## Requests and Output

The existing Python tool can send a single command without running its test flow:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --command radio.get
python3 tools/serial-check.py --port /dev/ttyUSB0 --command radio.set --args '{"preset":"lab_default","persist":true}' --allow-mock-changes
```

Read-only public commands need no modification opt-in. Other commands require
allow-mock-changes, which verifies the mock marker and automatically provisions
or unlocks for commands needing unlocked state. It never attempts mock unlock on
production firmware. Raw diag.radio.tx additionally requires allow-radio-transmit.
Command mode cannot be combined with storage or two-board checker modes.
For radio.set, queued hardware application is awaited before exiting.

```json
{"id":1,"cmd":"sys.hello","args":{"app":"offgrid-android","app_proto":"1.0"}}
{"id":2,"cmd":"sys.status"}
{"id":3,"cmd":"sys.ping"}
```

Each line ends with LF and must fit 2048 bytes including LF (and CR if used).
Keep at most four requests in flight. Responses echo a signed 64-bit integer ID.
`args` must be an object when present. Errors use `{code,message,field?}`.
Malformed JSON produces `E_PARSE` without ID; malformed envelopes use
`E_INVALID_ARG`. Unknown commands return `E_UNKNOWN_CMD`. Async events carry
`evt`, monotonic uptime `t`, and `data`; never answer them.

There are six fixed TX slots. The backend defers lifecycle events until after
the command response and pauses dispatch while notifications are pending.
Event producers must check queue capacity and retry failed sends. Never emit
raw prints or retain borrowed JSON views between ticks.

## Supported Commands

| Commands | Behavior |
|---|---|
| `sys.hello` | Validates optional app/version fields, rejects incompatible major versions, replaces the one-use session, reports limits and mock markers |
| `sys.status`, `sys.ping` | Public runtime snapshot and heartbeat; private counts appear only while unlocked |
| `sys.set_log_level` | Validates/stores off/error/warn/info/debug; non-off selection emits a synthetic log event after the response |
| `sys.reboot` | Response first, then simulated boot in locked state if provisioned |
| `sys.factory_reset` | Missing confirmation returns E_CONFIRM_REQUIRED; ERASE returns E_BUSY on buttonless hardware; no state/storage changes |
| `sec.provision`, `sec.unlock` | State/session checks, canonical Base64 of 32-byte public key and 60-byte wrapper; accepts the structurally valid envelope without authentication |
| `sec.lock` | Locks and emits `sec.state` after the response |
| `sec.rotate_token` | Requires unlocked state and fresh session; validates the envelope and returns simulated success |
| `sec.get_policy`, `sec.set_policy` | Unlocked only; timeout 0..3600 seconds, default 15; validates optional persist boolean |
| `id.get`, `id.set_name` | Public identity fixture and UTF-8 byte-limited display name |
| `pair.begin`, `pair.cancel`, `pair.complete` | Fresh salt, OG1 QR, 300-second expiry, self/duplicate rejection, replacement and fingerprint |
| `contact.list/get/rename/delete` | 32 stable public-data slots; pagination bounded by count and wire size; deletion requires true confirmation |
| `msg.send/cancel/pending` | Typed validation, eight RAM slots, monotonically allocated refs/counters, asynchronous status changes |
| `mock.set_scenario` | Unlocked mock-only delivery outcome: delivered, timeout, or radio_error; applies to subsequently queued messages |
| `mock.crypto.selftest` | Read-only mock diagnostic that runs CryptoEngine known-answer checks on the board; not present in production firmware |
| `mock.receive` | Unlocked mock-only incoming message fixture, using from/type/payload and optional increasing ctr, hops and retry |
| `radio.get/set` | Defaults, presets, strict ranges, warnings and calculated airtime; no radio is initialized |
| `radio.stats` | Synthetic packet/ACK/retry counters and signal values; optional reset reads then clears counters |
| `mesh.get/set` | Validated settings, nested ACK-default merging; hop defaults, ACK defaults, retries and locked relay state affect the mock |
| `led.get/set/test` | Pattern validation, nested merging and real settings persistence; copied settings/preview drive onboard PWM and two NeoPixel strips |
| `dbg.rf_trace` | Enables deferred rf.tx/rf.rx fixtures while unlocked; bytes are marked mock, not actual encrypted RF captures |

All command namespaces listed by the target specification now have mock handlers.
Arguments, result fields, and state rules follow the [host specification](../../docs/offgrid-link.md).

### Pairing and Contact Checks

Provision the mock, call `pair.begin`, then complete with a different device's
QR or the Bob QR test vector from the specification. The mock parses public
payloads but derives no shared/contact keys. Its fingerprint uses the specified
ordered-public-key SHA-256 calculation. Successful completion consumes the
pending pairing; errors leave it available until expiry/cancel/lock.

List pages can contain fewer than `limit` contacts to fit 2048 bytes. Advance
`offset` by the returned array length, not the requested limit. Contacts remain
public mock data in RAM across simulated reboot/lock, but are inaccessible while
locked and disappear on physical reset. Replacing a
contact resets its mock counters and pending/active state.

Check self-pair, duplicate/replace, invalid QR, expiry, empty/full slots, pagination,
rename, and confirmed delete. `contact.state` activation follows simulated
incoming traffic or a simulated verified delivery receipt.

### Message Scenarios

After pairing a contact, these requests exercise real response/event shapes:

```json
{"id":20,"cmd":"msg.send","args":{"to":1,"type":"text","text":"Hello!","ack":true}}
{"id":21,"cmd":"mock.set_scenario","args":{"outcome":"timeout"}}
{"id":22,"cmd":"mock.receive","args":{"from":1,"type":"geo","lat":47.0517,"lon":8.3075}}
```

Delivery normally emits sent, relayed (when hops > 0), then delivered. No-ACK
messages emit only sent and leave the queue. Timeout scenarios retry up to three
times and fail with timeout; radio_error fails without claiming transmission.
Timers use artificial short intervals (100/250/400 ms), not real airtime/retry
budgets. Airtime estimates use the current radio configuration and are calculated,
not measured. CAD, duty cycle, SNR, radio failures and relay operations are not
physically exercised. Retry-base configuration is stored but does not replace
the intentionally accelerated scenario timers.

Cancel returns its response before a cancelled status event. Lock and contact
deletion/replacement fail queued messages with locked/contact_deleted. A simulated
boot drops pending messages, as a real reboot would. A message ref is never reused
during this MCU run; re-pair resets that contact's counters.

`mock.receive` accepts the same typed payload fields as send, but uses `from`.
It emits msg.received after its response, updates the mock RX counter and activates
the contact. Its target.author is already from the receiving app's perspective.
Counters must increase; this fixture is not an AEAD or replay-window test.

Test all payloads, reaction removal, byte-overlong UTF-8 text, invalid coordinates,
eight-slot exhaustion, cancel, timeout/retry, radio error, and lock/delete cleanup.
Mock control commands are not part of the production host specification.

## Mock Security and Persistence

- Hello generates a fresh valid X25519 public key and nonce. Its private key is
  immediately discarded. Android can construct its normal wrapper, but the
  mock never decrypts it, checks a token, derives contact keys, or opens a vault.
- A session is consumed on an allowed provision/unlock/rotation attempt,
  including structurally invalid arguments. Retry requires a fresh hello.
- Wrong-token rejection and authentication rate limiting cannot be tested with
  this bypass. `mock: true` and `authentication: "bypassed-envelope-shape-only"`
  in hello explicitly advertise the limitation.
- Identity is a generated public-key fixture with a SHA-256-derived NodeID; no
  identity private key is retained. Provision/unlock are not security tests.
- Radio/mesh/LED settings and timeout policy now use real `ConfigStore` NVS.
  `persist: true` (default) commits before live application; `persist: false`
  changes only the live copy. Storage failure returns `E_INTERNAL` without
  changing live values. Simulated reboot reloads NVS and retains mock contacts
  and provisioning. A physical reset starts unprovisioned but loads saved settings.
  Identity, tokens, contacts and messages are never stored by ConfigStore.
- Heartbeat timeout locks without a response and emits `sec.state`. Poll/ping
  at least every five seconds. Timeout zero disables auto-lock in this mock.
- Factory reset is unavailable on buttonless boards. A confirmed request returns
  `E_BUSY`, without arming, erasing NVS or changing mock state. Recovery requires
  an owner-approved maintenance policy.

Hello reports `config_storage: "nvs"` and `config_schema: 1`; status adds
`storage: {backend, config_status, writable, last_error}`. Invalid/unknown-version
records are preserved and persistent writes disabled until explicit recovery.
See the [storage README](../storage/README.md) for schema and failure behavior.

## Configuration and Events

All set requests validate a candidate before changing live configuration. Unknown
fields are ignored; supported nested fields merge. A preset applies before
explicit radio fields. Results include the complete effective configuration.
LED custom patterns require 1..16 steps of 10..10000 ms and repeat 1..255;
`led.test.repeat` uses the same range. The repeat cap is a mock implementation
limit where the draft specification does not define one.

`radio.stats` contains simulated activity counts, not measured PHY statistics.
Counters without a simulated source remain zero. Trace payloads have valid
length/hex shapes and recognizable routing headers, but their body/tag bytes are
fixtures, not ciphertext or valid authentication tags. Never use them as crypto
vectors. `log` output is a synthetic level-change notification, not a complete
production logging subsystem. No events expose token/private-key material.

## Run the Real-Device Exerciser

Flash Stage 5, close other serial applications, and use a Python environment with
`pyserial` installed. The script opens a real port; it does not emulate firmware.

```sh
python -m pip install pyserial
python tools/serial-check.py --port /dev/ttyUSB0 --allow-mock-changes
```

Run from the repository root and substitute the actual port. The opt-in flag is
required because checks modify mock provisioning, contacts and now real saved settings.
The script checks mock markers before changes, uses synthetic wrapper bytes,
pairs/replaces the Bob fixture, sends/receives messages, exercises radio-error
events, validates atomic configuration, checks persistence across simulated
reboot, then deletes its fixture contact and leaves the device locked. Existing
mock Bob contacts/settings may change. It prints request/response transcripts.

The script does not automate Android USB permissions, all
framing/overflow cases, queue exhaustion, reconnects, or long soak tests.
The same script's `--storage-phase` modes provide a separate focused persistence
check across physical reset, with a snapshot and restore phase. Use the
[storage device-check instructions](../storage/README.md) for that workflow.
Physical reset clears mock provisioning, but no longer clears saved settings.

## Checks and Limits

The UART configuration image compiles. Physical NVS/Android checks still require
the flashed board. Check hello keys/nonce lengths, version rejection, consumed
sessions, wrong-state errors, response-before-state-event ordering, heartbeat,
reboot, and reset rejection on-device. `--ui-check` measures ping responsiveness
during OLED startup and Home rendering; it is not visual-display validation.

Parser nesting/capacity, full UTF-8 validation,
and strict RFC 8259 compliance still need verification. Mock incoming counters
only support increasing values; they do not implement the production replay
window. Authentication failures/rate limiting and encrypted storage remain out
of scope. Compilation is not USB interoperability proof.