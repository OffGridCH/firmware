# Host API and Serial Mock

## Current Implementation: Stage 2

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

All other namespaces are added in subsequent separately committed stages.
Arguments, result fields, and state rules follow the [host specification](../../docs/offgrid-link.md).

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