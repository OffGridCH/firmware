# Host API and Serial Mock

## Current Implementation: Stage 4

This folder implements the real JSON Lines transport and command-envelope
boundary. The simulated device backend lives in `../main.cpp`, behind
`OFFGRID_MOCK_DEVICE=1`. It is not secure firmware. Use synthetic data only.

| Component | Responsibility |
|---|---|
| `SerialLink` | LF/CRLF framing, 2048-byte wire limit, bounded RX/TX work, complete output lines, queued JSON serialization |
| `Commands` | JSON envelope validation, exact integer IDs, argument-validation helpers, dispatch, response/error formatting |
| `MockDevice` in `main.cpp` | Command-specific behavior, public identity, mock lifecycle, deferred events and physical reset confirmation |

Request views are valid only during dispatch. Copy any data retained for later
work into owned fixed buffers. Domain implementations can replace the mock
callback without changing the transport. No domain modules are initialized.

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
| `sys.set_log_level` | Validates/stores off/error/warn/info/debug; log producers arrive with later stages |
| `sys.reboot` | Response first, then simulated boot in locked state if provisioned |
| `sys.factory_reset` | Requires `confirm: "ERASE"`; arms a ten-second window requiring a fresh debounced PRG hold of three seconds |
| `sec.provision`, `sec.unlock` | State/session checks, canonical Base64 of 32-byte public key and 60-byte wrapper; accepts the structurally valid envelope without authentication |
| `sec.lock` | Locks and emits `sec.state` after the response |
| `sec.rotate_token` | Requires unlocked state and fresh session; validates the envelope and returns simulated success |
| `sec.get_policy`, `sec.set_policy` | Unlocked only; timeout 0..3600 seconds, default 15; validates optional persist boolean |

| `id.get`, `id.set_name` | Public identity fixture and UTF-8 byte-limited display name |
| `pair.begin`, `pair.cancel`, `pair.complete` | Fresh salt, OG1 QR, 300-second expiry, self/duplicate rejection, replacement and fingerprint |
| `contact.list/get/rename/delete` | 32 stable public-data slots; pagination bounded by count and wire size; deletion requires true confirmation |

| `msg.send/cancel/pending` | Typed validation, eight RAM slots, monotonically allocated refs/counters, asynchronous status changes |
| `mock.set_scenario` | Unlocked mock-only delivery outcome: delivered, timeout, or radio_error; applies to subsequently queued messages |
| `mock.receive` | Unlocked mock-only incoming message fixture, using from/type/payload and optional increasing ctr, hops and retry |

Configuration namespaces are added in the next separately committed stage.
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
locked and disappear on physical reset or confirmed factory reset. Replacing a
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
budgets. The default SF9/125-kHz airtime estimate is calculated, not measured.

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
- All settings and provisioning are RAM-only, even with `persist: true`.
  Simulated reboot keeps that data but clears session/unlocked/reset-arm state;
  it does not reset MCU uptime. A physical reset/power cycle starts unprovisioned.
- Heartbeat timeout locks without a response and emits `sec.state`. Poll/ping
  at least every five seconds. Timeout zero disables auto-lock in this mock.
- Factory reset clears mock state only. It requires a new press after arming;
  holding PRG before arming is insufficient. It does not erase other firmware's NVS.

## Checks and Limits

Both firmware profiles compile. Device/Android checks are pending until hardware
is available. Check hello keys/nonce lengths, version rejection, consumed
sessions, wrong-state errors, response-before-state-event ordering, heartbeat,
reboot, and reset expiry/hold on-device.

The [milestone checklist](../../docs/serial-mock-milestones.md) includes transport
boundary/reconnect/load checks. Parser nesting/capacity and strict RFC 8259
compliance still need verification. Compilation is not USB interoperability proof.