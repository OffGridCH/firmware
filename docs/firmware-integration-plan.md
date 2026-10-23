# Firmware Implementation and Integration Plan

## 1. Purpose and Starting Point

This is the execution handoff for the next firmware agent. Implement the missing
parts in small, independently committed stages, keeping each completed branch
uploadable and checking behavior on real boards. Do not implement everything in
one branch or replace missing behavior with simulated success.

Baseline inspected: `feature/firmware-core`, commit `56075f1`, 2026-10-24.
The OLED driver has been merged, but its App integration is not complete.
The earlier App startup check and two-board raw radio checks predate this merge;
they do not prove the current combined branch builds or implements secure chat.

**Hardware correction from the owner: there is no usable button.** Button support
is cancelled. Older button, PRG-hold, page-cycling and physical-reset instructions
in the architecture, host specification and UI handoff are not requirements for
this hardware. Update those documents during the buttonless stage, not by adding
another button abstraction. All other wire/security requirements still apply.

Read [architecture](offgrid-firmware-architecture_v1.md),
[host API](offgrid-link.md), [radio protocol](offgrid-protocol.md) and
[UI contracts](app-core-handoff.md) for detail. This document determines work
order and supersedes their old button assumptions. Resolve any other substantive
specification conflict explicitly; do not silently weaken a security requirement.

## 2. Component Inventory

"Implemented" means code exists, not that every failure condition was tested.
"Placeholder" means the named class exists without its planned implementation.

| Component | What exists now | Remaining work |
|---|---|---|
| `main.cpp` / `App` | Minimal entry point, ownership, cooperative loop, UI routing, mock backend wiring | Repair merged OLED boundary; remove button paths; own and connect real domain modules as they land |
| `BoardConfig` / `Types` | Serial/radio constants, merged OLED pins, public UI values and LED patterns | Verify physical wiring; agree strip pins/counts; extend shared values only for demonstrated needs; retire button-only behavior |
| `SerialLink` | Bounded JSON-line transport, queued output and backpressure | Reuse; verify responsiveness under real radio/crypto/UI load and disconnected/slow hosts |
| `Commands` | Real envelope parsing and argument helpers | Route command groups to real domains; keep JSON outside drivers/security/storage; preserve response-before-completion ordering |
| `MockDevice` | Simulated lifecycle, public contacts, pairing, message scenarios; real config/radio adapters and UI snapshots | Keep explicitly development-only; remove button sampling now; replace each domain path incrementally, without moving its large implementation into App |
| `ConfigStore` | Real versioned NVS settings: radio, mesh, LED, timeout; corruption/write-failure handling | Add vault salt/generation metadata with deliberate compatibility and crash-recovery rules; retain commit-before-live behavior |
| `CryptoEngine` | Randomness, hashes, HMAC/HKDF, X25519, AES-GCM/CCM and zeroization; previous device checks passed | Reuse vetted primitives; add a vetted SipHash implementation when relay dedupe needs it; preserve known-answer diagnostics |
| `RadioDriver` | Actual SX1262 RX/TX, CAD, backoff, completion events, timeouts, recovery, stats | Connect to MeshEngine; preserve hardware completion semantics; finish fault/load acceptance; airtime regulation remains missing |
| `VaultStore` | Placeholder | Encrypted identity/contact records, integrity checks, loading, safe creation/rotation/recovery and erasure |
| `Security` | Placeholder; current state changes are mock behavior | Real identity, provisioning, protected token unwrap, unlock verification, failure backoff, key cleanup, timeout and token rotation |
| `Contacts` | Placeholder; mock has 32 public RAM slots | Decrypted contact keys, persistence, counters/reservations, replay windows, deletion/replacement cleanup |
| `Pairing` | Placeholder; mock parses QR and calculates fingerprints | Real shared-key derivation, persistent contact insertion, expiry/cancel cleanup, collision handling |
| `PacketCodec` | Placeholder | Encode/decode TEXT, REACTION, GEO and ACK; construct authenticated headers/nonces; encrypt and verify packets |
| `MeshEngine` | Placeholder; delivery/retry/relay events are simulated | Real outbound queue, direct receive/delivery, ACKs, retries, replay decisions, relaying and congestion controls |
| `OledDriver` | U8g2 startup animation and home renderer using `HomeView`, `begin`, `drawHome` | Adapt to App's `init/show/onEvent/tick`; bounded animation/transfers; state-driven overlays; independent failure handling |
| `LedDriver` | App-compatible no-op interface, config/preview wiring | Implement actual confirmed outputs, animations, quiet/enabled/brightness behavior and nonblocking transfers |
| `Button` | Empty module, but mock still samples GPIO0 and App consumes page advances | Cancel: remove runtime sampling, debounce/hold/reset logic and button-only UI behavior; do not implement the placeholder |
| Existing device exerciser | Serial API checks, settings persistence phases, raw two-board checks, crypto diagnostic command | Extend the existing tool incrementally for real unlock, pairing, encrypted delivery and relay acceptance; remove button-required reset expectations |

Important distinctions: ConfigStore is not the vault. CryptoEngine is not a
complete secure lifecycle. RadioDriver sending bytes is not encrypted messaging.
Persisted mesh configuration is not a working mesh. Mock contacts/provisioning
do not survive physical power loss as a real identity/contact vault must.

## 3. Integration Boundaries

App owns long-lived instances and wires dependencies explicitly. Keep one
cooperative loop, fixed-capacity application buffers, direct calls and small
typed callbacks. Do not add a generic event bus or application tasks.

| Caller / producer | Destination | Contract / information crossing the boundary |
|---|---|---|
| Phone | SerialLink -> Commands | JSON requests, one response per request, bounded async events; cleartext chat on the cable is part of the design |
| Commands | Security | `sec.*` and `id.*`; validated bounded input, public results, pending/completion state where needed |
| Commands | Pairing / Contacts | `pair.*` / `contact.*`; public QR/contact views, never private keys |
| Commands | MeshEngine | `msg.*` / mesh policy; copied payloads, refs, cancellation and typed status events |
| Commands / App | ConfigStore + affected domain | Validate full candidate, save if requested, then apply live; distinguish desired and applied radio configuration |
| Security | CryptoEngine + VaultStore + ConfigStore | Unwrap phone token, derive vault key, verify/load encrypted identity; publish unlocked only after complete required load |
| Security / App | Pairing + Contacts + MeshEngine + VaultStore | Ordered lock notification: stop own sends, cancel pairing, clear plaintext/keys and close vault; fail pending work |
| Contacts | VaultStore | Versioned encrypted slots and counter/replay state; durable reservation before counter use; deletion/replacement results |
| Pairing | Security + CryptoEngine + Contacts | Own identity, peer QR/salt, shared-secret/key derivation, durable contact insertion; public fingerprint result |
| MeshEngine | Contacts | Candidate lookup, authenticated-use keys internally, TX allocation, replay decision/commit, pending-contact activation |
| MeshEngine | PacketCodec | Bounded payload/header values and internal keys; encoded frame or verified decoded result; codec does not mutate queues/replay state |
| PacketCodec / VaultStore | CryptoEngine | Radio AES-CCM / storage AES-GCM operations, explicit failure, no JSON or scheduling |
| MeshEngine | RadioDriver | Copied bounded frame, RX/TX/CAD request and completion/error events; never claim TX success on queue acceptance |
| Domains | App -> Commands/SerialLink | Typed lifecycle/contact/message events converted to host JSON; critical RF progress cannot wait for serial space |
| Domains / App | LED + OLED | Secret-free copied UiSnapshot, one-shot UiEvent, LED config/preview; UI never opens flash or calls radio/crypto |

Keep the existing UI methods: `init()`, `show(snapshot)`, `onEvent(event, nowMs)`,
`tick(nowMs)`, plus LED `configure(config)` and `preview(pattern, nowMs)`.
Copy retained input; do not hold caller references. `show` does not restart an
animation each loop. Failed UI initialization must not halt the other modules.
Boot, RF activity, errors and pairing remain useful events; no button is needed.

In plain terms, the completed send path is:

```text
Phone -> Commands -> MeshEngine -> Contacts + PacketCodec -> RadioDriver
                                      |           |
                                  VaultStore   CryptoEngine
```

Reception reverses the radio/codec path, checks contact/replay state, and reports
accepted messages to the phone. Relays forward protected frames without needing
the recipient's keys. The UI observes public results; it is never in the security
or message-delivery decision path.

## 4. Buttonless Behavior

- Never configure or poll GPIO0 for application input. The board's programming
  facilities are not an application button.
- Home is the default display; pairing/errors appear from state/events and expire
  automatically. Radio status can remain on Home until a non-button navigation
  contract is explicitly needed. Do not invent a host command just for paging.
- Remove PRG instructions, reset-hold progress and page-advance consumption.
  Retain legacy fields only if required for compatibility, always inactive; do
  not let them suggest a reset is armed or physical input exists.
- `sys.factory_reset` must not report armed/success when it cannot be confirmed.
  Until an owner-approved replacement exists, return existing `E_BUSY` with an
  explicit buttonless-reset-unavailable explanation and perform no erase.
- Do not turn locked-device factory reset into an unauthenticated serial wipe.
  Normal authenticated unlock/token rotation can proceed independently.
- Lost-phone/corrupt-vault recovery needs an explicit maintenance or approved
  replacement reset policy. This is a release decision gate, not permission for
  the agent to choose a weaker policy. Do not make recovery depend on the missing
  button. Keep erase operations explicit and operator-authorized.

## 5. Branches and Commit Stages

Use the branch names below as defaults; reuse an already assigned coworker branch
for the same task. Integrate prerequisites before starting dependent work. The
integration base is currently `feature/firmware-core`, not an assumed up-to-date
`main`. If the owner moves the integration base, follow the actual merged tip.

### Stage 0: Restore the Merged OLED/Core Boundary

Branch: `feature/oled-core-integration`; owned by the OLED coworker if already
assigned. Do not duplicate their implementation or overwrite their work.

1. `fix(oled): adapt display driver to App contracts` - preserve the actual
   renderer; map existing UiSnapshot values to it and restore a buildable API.
2. `feat(oled): advance startup and rendering cooperatively` - replace the
   full-animation blocking loop, bound display transfers, tolerate init failure.
3. `docs(oled): record hardware integration checks` - verify Home uses live core
   data, serial remains responsive during animation, and host disconnect is safe.

Gate: default UART and one native-USB build pass; upload one board and check
status/boot plus actual display. Do not call the current interface mismatch an
accepted baseline. Coordinate/wait for the coworker's compatibility commit if
they own it; independently prepare later interfaces without changing their files.

### Stage 1: Remove Button Dependencies

Branch: `feature/buttonless-core`; depends on Stage 0 for a buildable baseline.

1. `refactor(core): remove application button dependencies` - remove sampling,
   debounce, page advance and hold-confirmation paths; leave no hardware polling.
2. `fix(host): reject unavailable physical reset on buttonless boards` - implement
   the fail-closed behavior in section 4; keep ordinary settings persistence.
3. `docs(core): align workflows with buttonless hardware` - update old specs/UI
   handoff and adapt the existing checker so no active check requires a hold.

Gate: status/ping, mock workflows and saved settings still work; reset is rejected
without changing flash; OLED state does not require a press. No destructive reset
check until a replacement policy is approved.

### Stage 2: Finish LED Output

Branch: `feature/led-core-integration`; depends on Stage 1. Can run alongside
Stages 3 and 6. Its absence must not block secure communication development.

1. `feat(led): initialize verified board and strip outputs`.
2. `feat(led): implement state event and preview patterns` - copy config/preview,
   separate state/event timing, enforce quiet/enabled/brightness, remove hold UI.
3. `docs(led): record output and responsiveness checks`.

Gate: physically verify confirmed outputs, live/persistent settings and preview
without disrupting serial/radio. Do not guess external strip wiring or counts;
leave unknown outputs disabled and report the concrete hardware blocker.

### Stage 3: Implement Encrypted Vault Storage

Branch: `feature/vault-store`; depends on Stage 1; independent of LED rendering.

1. `feat(storage): add versioned encrypted vault records` - agree the record
   formats and metadata layout first; implement authenticated read/write and
   distinct missing/corrupt/unsupported/unavailable outcomes using CryptoEngine.
2. `feat(storage): add crash-safe vault generations` - creation, recovery and
   generation activation; new records must be durable before publishing them.
   Extend ConfigStore metadata deliberately; do not silently overwrite schema-1
   settings or a damaged/unknown vault. Fresh IV per encryption; bind records to
   namespace/key/generation as specified. Never serialize raw C++ structs.
3. `docs(storage): document vault layout and device checks` - use fixed-key
   fixtures only in a diagnostic profile; extend the one existing exerciser.

Gate: encrypted round-trip survives physical power cycle; wrong key, changed
record and wrong associated data fail without usable plaintext. Power interruption
cannot activate an incomplete generation. Never log keys/tokens or store the
vault key unencrypted. Production erasure remains blocked by the reset policy.

### Stage 4: Replace Mock Provisioning and Unlock

Branch: `feature/secure-lifecycle`; depends on Stage 3.

1. `feat(security): implement persistent provisioning and unlock` - real identity,
   one-use X25519 sessions, protected token unwrap, key derivation and vault check;
   connect `sec.*` / `id.*` through the existing command boundary.
2. `feat(security): enforce lock cleanup and authentication backoff` - heartbeat,
   failure handling, partial-load cleanup and public-only state snapshots.
3. `feat(security): rotate tokens with durable generation switching` - old token
   remains recoverable until new generation activation; no half-rotated vault.

Introduce an explicit secure-development build instead of silently changing the
Android mock. Each environment must define the mock flag exactly once. Adapt
App's mock-only guard and radio ownership deliberately; never fall back to mock
authentication on real-vault failure. Keep raw radio/fixed-key diagnostics lab-only.

Gate: physical reboot preserves identity and boots locked; correct token unlocks,
wrong token fails, consumed sessions cannot be reused, timeouts wipe keys, and
power-cut rotation has a defined recoverable outcome. Factory reset stays disabled
unless the replacement policy has been approved and implemented.

### Stage 5: Implement Contacts and Real Pairing

Branch: `feature/contacts-pairing`; depends on Stage 4.

1. `feat(contacts): persist keys counters and replay state` - fixed 32-slot table,
   public views, durable TX reservations, conservative replay recovery and wiping.
2. `feat(pairing): derive and persist contact relationships` - QR validation,
   both-party salt ordering, directional keys, fingerprint, expiry and cancellation.
3. `feat(host): route contact and pairing commands to real domains` - save before
   success; coordinate replacement/deletion with queued-message cleanup.

Gate: two boards pair using synthetic identities/data, agree matching directional
keys/fingerprints without printing keys, retain contacts after power cycle, reject
self/duplicate invalid pairing, and expose no contact secrets through host/UI.

### Stage 6: Implement the Radio Packet Codec

Branch: `feature/packet-codec`; depends on Stage 1. Can run alongside vault work
once input/output value types and ownership have been agreed; no vault is needed
for diagnostic fixed-key checks. Coordinate any shared-header changes.

1. `feat(protocol): encode headers and typed message payloads` - all lengths,
   reserved fields, coordinate endpoints, TEXT/REACTION/GEO/ACK formats.
2. `feat(protocol): seal and verify data and receipt packets` - specified nonce,
   masked associated data and AES-CCM; verification has no scheduling side effects.
3. `docs(protocol): record on-device packet vector checks`.

Gate: device output matches protocol vectors exactly; round-trips all payloads;
corrupt ciphertext/tag/header and malformed lengths fail safely. A codec success
does not yet prove counters, delivery or replay handling are correct.

### Stage 7: Encrypted Direct Messaging and Reliability

Branch: `feature/direct-messaging`; depends on Stages 4, 5 and 6.

1. `feat(mesh): send and receive encrypted direct messages` - connect Contacts,
   PacketCodec and RadioDriver; bound queues; emit accepted receive events once.
2. `feat(mesh): add authenticated receipts retries and replay checks` - actual TX
   completion starts timers; duplicate retries can get a fresh receipt without
   delivering a second message. Add cancel/full/error/lock/deletion handling.
3. `feat(core): route real message lifecycle to host and UI` - preserve public
   response/event shapes, independent RF progress and bounded host notifications.

Gate: two-board TEXT/REACTION/GEO delivery and receipt, lost-receipt retry,
duplicate/replay rejection, cancel, queue exhaustion, lock/unlock and physical
reboot. No private payload/key logging. Raw radio success alone does not pass.

### Stage 8: Multi-Hop Relaying

Branch: `feature/mesh-relaying`; depends on Stage 7.

1. `feat(mesh): add bounded relay scheduling and deduplication` - vetted keyed
   hashing, hop limits, SNR-weighted randomized delay and overhear cancellation.
2. `feat(mesh): enforce forwarding priorities and rate limits` - own/ACK work
   takes priority, collision-aware routing, bounded cache/queues, keyless relay
   policy when locked; unprovisioned secure firmware keeps RF off.
3. `docs(mesh): record multi-board forwarding checks`.

Gate: three boards relay when direct delivery is unavailable; four boards verify
suppression and bounded traffic; relay nodes cannot decrypt payloads. Check slow
host, cache pressure and retries without duplicate delivery or relay storms.
Do not claim multi-hop acceptance with only two available boards.

### Stage 9: Full-System Acceptance and Release Preparation

Branch: `feature/firmware-hardening`; depends on direct/relay stages and completed
available UI hardware. Missing physical hardware is an explicit acceptance gap.

1. `fix(core): close mixed-load recovery and lifecycle gaps` - measure loop stalls,
   serial overruns, radio faults, backpressure, flash failures and power-cut cases.
2. `chore(release): separate secure firmware from lab diagnostics` - remove mock
   controls, fixed-key fixtures and secret-bearing debug paths from normal builds.
3. `docs(release): record acceptance evidence and remaining hardening gates`.

Release remains blocked on an approved buttonless recovery/reset policy, lawful
airtime/duty-cycle controls, signing/update/recovery strategy, and deliberate
Secure Boot/flash-encryption/debug-access configuration. Never burn irreversible
eFuses or erase a user's vault autonomously. Those require owner authorization.

## 6. Non-Negotiable Protocol and Lifecycle Details

- Use counters 1 through `2^24 - 1`; zero is reserved and counters never wrap.
  Per contact/direction, new data and fresh ACKs share counter allocation. Reserve
  an exclusive durable range before use; reboot/unlock resumes at its saved end.
- Retrying data keeps its original counter and advances retry metadata; a newly
  generated ACK gets a fresh counter. Do not invent a different wire format.
- Verify authentication before mutating replay state, activating a contact or
  marking delivery. The 64-entry replay window must not redeliver a duplicate;
  an old delayed retry can legitimately expire outside that window.
- Relays normalize only the specified mutable fields for dedupe, retaining the
  authentication tag. Do not classify packets as self echoes just because a
  short source ID matches; node IDs can collide. Follow addressed verification
  and transit-fallback rules from the protocol before relay-cache decisions.
- Lock cancels pairing and own-message work, wipes plaintext/contact/vault keys,
  and changes radio policy. No failure path may leave the device partly unlocked.
- Flash commits and library operations can stall. Measure them; do not claim
  strict nonblocking behavior merely because a method is named `tick`.

## 7. Autonomous Agent Working Rules

1. Read this document and current code; inspect `git status`, active branch,
   upstream and coworker ownership. This inventory is a dated baseline, not a
   reason to overwrite newer merged implementations. Preserve unrelated changes.
2. Select the first unclaimed stage whose prerequisites are merged. Create its
   feature branch from the current integration tip. Reuse the coworker's branch
   when assigned; use separate worktrees only for truly independent work. Never
   work on or commit someone else's unfinished changes without agreement.
3. Explain the local hypothesis/check, implement the smallest useful stage, then
   immediately run one focused check. Avoid broad exploratory test loops or a
   new test framework. Reuse `tools/serial-check.py`; extend it only as needed.
4. Commit each complete, validated stage using the conventional subjects above
   (`feat`, `fix`, `refactor`, `docs`, `chore`). Exact wording can reflect the
   actual slice. Do not commit incomplete integration just to match a checklist.
5. Build affected conditional paths: default UART plus native USB when host/core
   changes; diagnostic radio when RF wiring changes; secure profile when added.
   Check the touched C++ diagnostics and whitespace. Do not build every profile
   after a documentation-only edit or repeatedly rerun unrelated checks.
6. Use real-device gates for each functional milestone. Detect actual ports;
   close monitors; keep antennas attached. The only board seen at the latest
   inspection was `/dev/ttyUSB0`; enumerate again rather than assume a peer.
   Raw RF requires explicit transmit opt-in and lawful settings. New destructive
   checks require permission and a restore/recovery procedure.
7. If required boards/hardware are unavailable, commit only a coherent buildable
   slice and label hardware acceptance pending. Never claim a simulated check
   proves physical persistence, authenticated delivery, or multi-hop behavior.
8. Push completed branch commits to `origin` with upstream tracking. Do not
   force-push, merge into `main`/the integration branch, burn eFuses, or erase
   devices without separate authorization. Hand back branch, commit IDs, check
   results and remaining blockers so the owner can review/merge.
9. After each accepted milestone, update this document's inventory/progress with
   actual evidence and the next stage. Update affected specs in the same branch
   when behavior/contracts change; do not leave mock claims attached to real code.

## 8. Build and Check Reference

Use PlatformIO Core 6.x. On the inspected machine the working executable was
`/tmp/offgrid-platformio/bin/platformio`; the system `pio` was obsolete. Confirm
the executable still exists; otherwise use a compatible isolated installation.

```sh
git switch -c feature/<stage-name> <current-integration-tip>
/tmp/offgrid-platformio/bin/platformio run -e heltec_wifi_lora_32_V3
/tmp/offgrid-platformio/bin/platformio run -e serial_mock_usb
/tmp/offgrid-platformio/bin/platformio device list
/tmp/offgrid-platformio/bin/platformio run -e radio_diag -t upload --upload-port <actual-port>
python3 tools/serial-check.py --port <actual-port> --command sys.status
git diff --check
git add <only-stage-files>
git commit -m "feat(module): describe the completed stage"
git push -u origin <stage-branch>
```

Commands are templates, not instructions to upload a diagnostic image over an
unrelated coworker test or a provisioned secure device. Select the correct image
and preserve saved state. Secure-token checks must use real wrapped credentials,
not the mock tool's synthetic-envelope unlock helper. Do not send secrets through
chat or print them in transcripts. Keep one existing device exerciser rather
than one new script per component.

## 9. Completion Definition

The project is not complete just because every class has methods. It is complete
when real provisioning and contacts survive power loss, unlocking genuinely
authenticates, private state is cleared on lock, two boards exchange protected
messages with correct receipts/retries/replay handling, and multi-board relaying
works without decrypting transit traffic. Confirm available LED/OLED behavior,
host responsiveness, recovery, and the release gates separately with evidence.

Current next action: coordinate the OLED compatibility stage, then remove all
runtime button assumptions. Vault/security and packet-codec work can follow the
dependencies above; neither vault nor meshing is implemented at this baseline.