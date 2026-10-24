# Secure host interface

All firmware profiles use real Security, Contacts, Pairing, MeshEngine and
encrypted VaultStore. The development simulator and its authentication bypass
have been deleted. The retained `mock: false` wire field lets clients/checkers
reject old simulator images; it is not a selectable backend.

`Commands` validates JSON/envelopes and strict typed arguments, adapts domain
results/events to [offgrid-link](../../docs/offgrid-link.md), and owns live
configuration views. `SerialLink` owns bounded JSON-lines input/output and
reserves response space before accepting another command. RF progress never
waits for host output space; bounded event/trace loss is counted and reported.

Normal UART/native-USB builds exclude `diag.*`, `dbg.*` and fixed-key fixtures.
The explicitly selected `radio_diag` / `radio_diag_usb` profiles retain real
authentication and encrypted storage while adding:

- `diag.codec.selftest`: published vectors and malformed/boundary cases.
- `diag.vault.selftest`, optional `verify`: separate `og_vtest` namespace, storage,
  counters, replay, queues, relay and exact airtime-pacing assertions.
- `diag.mesh.drop_next_ack`: unlocked, suppress exactly one genuine receipt to
  test physical retry/re-ACK without duplicate delivery. Cleared on lock.
- `dbg.rf_trace {enabled}`: unlocked, actual PHY ciphertext bytes and outcomes;
  disabled on lock, bounded drops visible in `radio.stats`. Never keys/tokens.

No raw-radio mock control or unauthenticated provisioning path remains.
`sys.set_log_level` is RAM-only; structured error reporting is never disabled.
See the root [README](../../README.md) for real-device acceptance commands,
operator credentials and owner-signed reset.
