# Serial Mock Implementation Stages

## Scope

Implement real USB serial transport with a replaceable mock device handler in
`src/main.cpp`. `SerialLink` owns framing, bounded TX, and JSON output; `Commands`
owns parsing, envelope validation, and dispatch. The mock owns synthetic device
behavior. The remaining domain modules stay placeholders until needed.

The [host specification](offgrid-link.md) is the target API. A mock is not a vault
or radio: use synthetic data only. Build flag `OFFGRID_MOCK_DEVICE=1`, firmware
versioned `0.1.0-serial-mock.*` images, and additive `mock` / `mock_stage` fields distinguish
it from real firmware. Never treat simulated unlock/delivery as a security result.

## Commit Boundaries

Each stage is a separate buildable commit in the repository's `feat(scope): ...`
style. Compile and check the current stage before starting the next. Hardware
acceptance remains a separate gate when a board is unavailable.

| Stage | Commit message | Scope and device checkpoint |
|---|---|---|
| 1 | `feat(serial): add bounded JSON transport and mock status backend` | Framing, strict envelope types, bounded TX, boot/ping/status, build profiles. Exchange requests without truncated/interleaved output. |
| 2 | `feat(serial): add mock handshake and device lifecycle` | Hello/version negotiation, explicit mock session rules, provision/unlock/lock shapes, heartbeat, policy, reboot ordering and physically confirmed reset. Document crypto bypasses and persistence deviations. |
| 3 | `feat(serial): add mock contacts and pairing commands` | Identity, valid synthetic OG1 QR payloads, pairing expiry/cancel, contact CRUD/pagination and error scenarios. App renders/scans QR and manages synthetic contacts. |
| 4 | `feat(serial): simulate messages and asynchronous device events` | TEXT/REACTION/GEO validation, eight pending slots, timed status events, cancellation, incoming-message scenarios and lock/delete cleanup. App exercises all message states. |
| 5 | `feat(serial): add mock configuration and Android device checks` | Radio/mesh/LED commands, atomic partial updates, explicit persistence rules, logs/traces, real-device host exerciser and reconnect/load checks. |

Keep security shortcuts and mock controls compile-time isolated. Preserve normal
response schemas. Do not make a serial integration test depend on an unfinished
Android UI, or mistake host-side compilation for an actual device acceptance run.

## Stage 1 Behavior

- Default profile `heltec_wifi_lora_32_V3` uses UART0 (`Serial0`) through the
  USB-UART bridge at 115200 8N1. It requests 4096-byte driver RX/TX buffers.
- Alternative `serial_mock_usb` uses native USB Serial/JTAG CDC (`Serial`).
  Confirm connector routing, host driver support, and connection detection on
  actual hardware; it is not the USB-UART bridge.
- Startup queues one empty LF and then `sys.boot`. Native USB requeues them
  when the driver's connection indicator changes to connected. UART has no
  reliable port-open notification, so boot replay on an already-running UART
  device is not yet implemented. A fresh ping discovers a running Stage 1 image.
- `sys.ping` returns `{uptime_ms}`. `sys.status` returns the public unprovisioned
  snapshot, plus mock markers. Other commands, including hello, return
  `E_UNKNOWN_CMD`. There is no simulated successful handshake or state transition.
- Uptime is 64-bit monotonic milliseconds. Device ID is the hardware base MAC.
  Heap and security flags reflect the actual runtime/hardware; radio is off,
  battery is `null`, and private queue/contact fields are absent.
- Both directions enforce 2048 bytes including LF. Trailing CR is tolerated and
  counts toward that limit. Oversized input is discarded through LF, producing
  one `E_LINE_TOO_LONG` event. The next line is processed normally.
- Empty/malformed JSON returns `E_PARSE` without ID. An invalid envelope returns
  `E_INVALID_ARG` with the offending field. Valid signed 64-bit IDs are echoed;
  string, boolean, and floating-point IDs are rejected. Commands must be nonempty
  strings without embedded NUL. Present `args` must be an object. Unknown fields
  are ignored.
- ArduinoJson 6.21.5 uses a fixed 8192-byte request document and nesting limit
  eight. Exhausting these parser bounds produces `E_PARSE`. These are initial
  implementation limits, not changes to the protocol. Full RFC 8259 conformance
  and worst-case unknown-field capacity still need verification before claiming
  complete host-spec compliance.
- Each tick reads/writes at most 256 bytes and dispatches at most one line. Six
  fixed 2048-byte TX slots preserve complete-line order. Request dispatch pauses
  when no response slot is available. Current handlers produce one response and
  no extra event; future event producers must preserve response reservation.
- Respect the four-in-flight host limit. Unlimited flooding or an indefinitely
  stalled reader can overflow hardware RX buffers because there is no flow
  control. Output buffering does not guarantee lossless unbounded input.
- Response-document overflow or oversized output becomes `E_INTERNAL`, never a
  truncated JSON response. Parsed request views do not survive dispatch.
- No peripheral demo, flash persistence, radio traffic, authentication, or other
  production device behavior is implemented by this stage.

## Stage 1 Device Acceptance

Use the [README](../README.md) build/upload commands. Record firmware revision,
profile, board revision, actual port, Android USB driver, and a serial transcript.
Only one host application should own the port at a time.

1. Open at 115200 8N1 for the bridge, with DTR/RTS low. Reset once if needed to
   observe startup. Ignore ROM text. Verify empty LF followed by JSON `sys.boot`,
   mock markers, and a 12-uppercase-hex device ID.
2. Send `{"id":1,"cmd":"sys.ping"}` plus LF. Verify exactly one success with
   the matching ID and numeric uptime. Repeat to check increasing uptime.
3. Send `{"id":2,"cmd":"sys.status"}`. Verify unprovisioned state, radio off,
   null battery, no private queue/contact fields, and mock markers.
4. Send a ping in fragments with host-side pauses: no reply before LF. Repeat
   with CRLF and an unknown extra field.
5. Send four pings in one write, IDs 10 through 13. Verify exactly four ordered
   responses, matching IDs, and one complete JSON object per line.
6. Send an empty line, malformed JSON, and `{"id":1,"cmd":"sys.ping"}junk`.
   Each must return `E_PARSE` without ID. A subsequent valid ping still works.
7. Send `[]`, a string ID, a numeric command, and `args: null`. Verify
   `E_INVALID_ARG`, the offending field, and ID echo only when valid.
8. Send an unknown command and `sys.hello`. Verify `E_UNKNOWN_CMD`; neither may
   report successful execution or change state in Stage 1.
9. Use a host sender to pad valid JSON with whitespace: 2047 bytes before LF
   must fit; 2048 bytes before LF produces one oversized-line event. The next
   ping must succeed. Count CR in the same wire budget for CRLF.
10. Pause host reads temporarily with at most four requests in flight. Resume
    and verify whole ordered responses. Repeat with long lines/partial writes.
11. Reconnect without resetting an independently powered board. UART must answer
    a fresh ping even if startup was missed. Check native-USB detection and boot
    replay separately if that interface is used.
12. Run repeated ping/status exchanges for at least ten minutes. Check no reset,
    no raw application debug prints, and no steadily decreasing heap.

## Validation Record

Stage 2 is implemented and both UART/native-USB builds pass. The current command
subset, deliberate authentication bypass, and RAM-only reboot behavior are
documented in the [host README](../src/host/README.md). Earlier Stage 1 behavior
above describes that historical commit, not the current supported command set.

Stage 1 UART and native-USB images compile with the pinned PlatformIO setup.
No USB serial board was detected during implementation. Upload, driver behavior,
these acceptance scenarios, and Android interoperability remain unverified.
Update this record and README's supported subset in every subsequent stage.