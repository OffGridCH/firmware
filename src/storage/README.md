# Configuration Storage

`ConfigStore` is a real plaintext NVS module. The surrounding identity, unlock,
contacts, messages, and peripheral behavior remain development mocks.

## Stored Data

The default ESP32 NVS partition contains namespace `og_cfg`, with one blob key
named `settings`. Schema 1 encodes the complete saved radio, mesh, LED, and
security-policy configuration as JSON. It stores no raw C++ struct memory.

- Radio: frequency, bandwidth, SF, CR, power, preamble and sync word.
- Mesh: hop/relay/CAD/backoff/retry/rate-limit settings and per-type ACK defaults.
- LED: enabled/quiet/brightness and built-in/custom state/event patterns.
- Security policy: `lock_timeout_s` only.

Settings contain no tokens or private material. `ConfigStore` also provides
separate `vault` metadata blob IO without rewriting schema-1 `settings`.
`VaultStore` now implements encrypted generations; it is not yet wired to a real
host lifecycle in the mock profiles. Do not confuse configuration with the vault.

## Encrypted vault format (schema 1)

`og_cfg/vault` is an atomic 26-byte metadata blob:
`"OGVM" | version:u8=1 | bank:u8 (0/1) | generation:u32be | salt:16`.
Generation zero is invalid. Unknown/corrupt metadata never becomes a fresh vault.
Settings schema 1 remains unchanged, including read-only handling of damaged
settings. Metadata is updated independently; it never overwrites saved settings.

`og_vault` contains physical keys `a_check`, `a_identity`, `a_index`, `a_c01`
through `a_c32`, with matching `b_` keys for the other generation. Each blob is:
`"OGVR" | version:u8=1 | plaintext_length:u16be | random_iv:12 | ciphertext | tag:16`.
AES-256-GCM AAD is `namespace ASCII | NUL | physical key ASCII | NUL |
generation:u32be`, binding namespace, logical slot/bank and generation.
Plaintext is bounded at 256 bytes. Every encryption obtains a fresh random IV.
Keys are RAM-only, explicitly zeroized on close; failed reads clear the entire
caller output and return length zero.

The check plaintext is `"offgrid-vault-v1"` including its final NUL. Identity is
`private_key:32 | name_length:u8 | name:1..24`, never raw struct memory.
`index` is a four-byte big-endian contact-occupancy bitmap; contact plaintext
encoding belongs to Contacts. Open verifies check, identity, index and every
occupied contact before making a vault key available to internal operations.
Missing occupied records fail rather than silently dropping contacts.

Creation writes/commits all required records, authenticates readback, then
switches metadata. Abandoned initial records without metadata boot `incomplete`,
not `missing`, and require authorized recovery. Rotation first clears only the
inactive bank, copies/re-encrypts every occupied record, verifies it, and finally
atomically commits metadata. Failure before activation leaves the old bank
selected. An ambiguous metadata commit failure closes the vault; recovery must
reload the durable selector rather than guessing. No automatic older-generation
fallback is allowed after successful rotation.

Contact insertion commits its record before adding it to the authenticated
index. Deletion durably removes its index bit; obsolete ciphertext is excluded
from reads/rotation and later overwritten/cleared. NVS does not promise physical
forensic erasure of old flash pages. Counter/contact callers must fail closed on
ambiguous active-record write failures.

Diagnostic-only `diag.vault.selftest` uses the separate `og_vtest` namespace and
fixed fixture keys compiled only into radio diagnostic profiles. It does not
write `og_cfg`/`og_vault` or transmit RF:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.vault.selftest --allow-mock-changes
# Hardware reset/power cycle, then:
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.vault.selftest --args '{"verify":true}' --allow-mock-changes
```

Both phases passed on the connected board, across hardware reset: authenticated
round trip, wrong key/generation and modified-tag rejection, cleared failed
plaintext, refusal to activate an incomplete bank, contact slot 32/deletion and
two generation switches with old-key rejection. This is not power-cut injection
or physical power-removal evidence. Real secure lifecycle and authorized reset now use these APIs in `secure_dev`;
no mock authentication opens a real vault.

## Interface

| Method | Contract |
|---|---|
| `load()` | Initialize/access NVS, load and validate schema/ranges/patterns; use defaults when no record exists |
| `copyTo(config, timeout)` | Copy saved radio/mesh/LED values and timeout into the caller's live state |
| `saveCategory(name, config)` | Save a complete validated radio, mesh or LED category while retaining other saved categories |
| `savePolicy(timeout)` | Save timeout while retaining the other saved settings |
| `eraseAll()` | Erase only namespace `og_cfg`, commit, and restore in-memory defaults |
| `statusName()`, `writable()`, `lastError()` | Expose load state, persistent-write availability, and ESP-IDF error code |
| `validPattern(value)` | Shared built-in/custom LED pattern validation for storage and host commands |

`ConfigStore` owns the saved snapshot; the caller owns live configuration.
All buffers/documents are fixed-capacity members. The record wire-size cap is
8192 bytes and each store JSON document has 16384 bytes of capacity. Host command
responses retain their independent 2048-byte serial limit.

## Persistence and Failure Behavior

`persist: true` (default) commits before the host applies the live update. Failed
storage returns `E_INTERNAL`; the command does not report success or change live
settings. `persist: false` changes only the live copy and performs no flash write.

Saving a category saves its complete effective candidate. For example, changing
SF temporarily and then persisting a power change also persists that temporary
SF. It does not persist temporary settings from other categories or policy.
The device checker explicitly covers this distinction.

Writes use one NVS blob followed by `nvs_commit`, not independent field writes.
Recovery relies on ESP-IDF NVS single-key update/integrity behavior. No custom
journal is claimed; a power interruption can leave the old or new complete
record. If a commit reports an I/O failure, the durable outcome must be checked
after recovery even though the live update was withheld. Do not write settings
every tick: commits and flash garbage collection can block the cooperative loop.

Load states:

- `defaults`: namespace/record absent; use defaults and allow future saves.
- `loaded`: schema 1 and all required fields/types/ranges validated.
- `invalid`: malformed, oversized or invalid settings; defaults used read-only.
- `unsupported_version`: unfamiliar schema; defaults used read-only.
- `unavailable`: NVS initialization/access failed; defaults used read-only.

Corrupt/unknown records are never silently replaced or erased. Temporary settings
can still be used. Boot emits an error event when loading fails; `sys.status`
includes `storage.backend`, `config_status`, `writable`, and `last_error`.
Hello advertises `config_storage: "nvs"` and `config_schema: 1`.

Legacy mock reset is unavailable (`E_BUSY`). Secure profiles use the approved
fresh app-token proof or owner-signed device challenge described in host section
6.11. `og_cfg/reset` is committed before accepting a reset; App then erases vault
records/metadata first, settings and marker last, and reboots. A hardware reset
mid-erase resumes that previously authorized operation. An unreadable/unknown
marker blocks provisioning rather than implying fresh storage. If NVS cannot
initialize, no partition-wide automatic recovery erase occurs.

`sys.reboot` is still simulated, but reloads configuration from actual NVS. A
physical reset/power cycle also reloads it. Mock provisioning and contacts are
RAM-only, so physical reset returns the mock to unprovisioned while configuration
and policy remain saved. OLED is initialized independently; default RF stays off.

## One Device Checker

Use the existing `tools/serial-check.py`; no additional test suite is required.
Install `pyserial` in your Python environment and flash the config-store image.
Close other applications using the serial port. Run from the repository root:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase write --allow-config-changes
```

This phase captures the original saved configuration, writes fixtures, checks
invalid all-or-nothing updates and applies temporary overrides. It also persists
one category while other categories remain temporary to catch accidental leakage.
It creates `config-check-state.json` locally and refuses to overwrite an existing
snapshot. Keep it if a run fails so the original settings can be restored.

**Physically reset or power-cycle the board**, then verify flash readback:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase verify --allow-config-changes
```

Verification requires the same hardware device ID, a completed write phase,
fresh unprovisioned mock state and `loaded` NVS status. It checks radio, mesh,
custom LED patterns, and timeout policy against the saved snapshot. It must not
be replaced with the simulated `sys.reboot` command for this check.

Restore the original saved settings when finished:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase restore --allow-config-changes
```

Restoration saves the original values and removes the snapshot on success.
Use the same `--state /path/to/snapshot.json` in every phase if overriding its
location. The script refuses a different board ID. A fresh write phase starts by
reloading saved settings, so existing temporary overrides are discarded.

Optional non-destructive reset-rejection check:

```sh
python3 tools/serial-check.py --port /dev/ttyUSB0 --storage-phase reset --allow-config-changes
```

This verifies `E_CONFIRM_REQUIRED`/`E_BUSY`, unchanged state/storage status and
unchanged public configuration. It never erases or alters a snapshot. Physical power-cut
fault injection, corrupt-record/version recovery and unrelated-namespace
preservation are manual acceptance checks, not claimed by this script.

The module and UART integration compile. Checker CLI validation is not proof of
flash persistence: the write/reset/verify phases must be run on a flashed board.