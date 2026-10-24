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

No tokens, identity/private keys, contacts, messages, vault salt, or vault generation
are stored by this implementation. Vault metadata and encrypted storage remain
separate future work. Do not confuse plaintext configuration with the vault.

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

Factory reset is unavailable on buttonless hardware. `confirm: "ERASE"` returns
`E_BUSY`, with no erase or arm window; missing confirmation still returns
`E_CONFIRM_REQUIRED`. Recovery requires an owner-approved maintenance policy.
The internal `eraseAll()` primitive has no serial command path. If NVS cannot
initialize, the module never erases the partition as an automatic recovery shortcut.

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