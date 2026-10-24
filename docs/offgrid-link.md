# OffGrid Serial API

- **Firmware:** 0.3.0
- **Host protocol:** 1.1 (adds groups; 1.0 apps keep working)
- **Hardware:** Heltec WiFi LoRa 32 V3 / ESP32-S3
- **Transport:** USB serial, UTF-8 JSON Lines

This is the implemented serial interface, not a future API proposal. Normal
firmware uses real authentication, encrypted persistent identities/contacts and
encrypted LoRa messaging. The simulator has been removed.

The phone/app owns the chat history, QR scanning/rendering and unlock credential.
The device owns private identity/contact keys, packet encryption, counters,
receipts, retries, replay protection and relaying. Message bodies are cleartext
on the trusted USB cable and ciphertext over LoRa. Private identity/contact keys
are never returned by this API.

Multi-hop relaying is implemented and its bounded scheduling, dedupe, collision
fallback and limits have passed isolated device checks. Physical forwarding and
suppression across three/four boards have **not** been verified. Two-board
encrypted messaging, lost-receipt retry and duplicate rejection have been tested.
See [README](../README.md) for build profiles and the remaining hardware limits;
see [radio protocol](offgrid-protocol.md) for the on-air format.

## Contents

1. [Connect and frame messages](#1-connect-and-frame-messages)
2. [States and authentication](#2-states-and-authentication)
3. [System commands](#3-system-commands)
4. [Security and identity commands](#4-security-and-identity-commands)
5. [Pairing and contacts](#5-pairing-and-contacts)
6. [Messages](#6-messages)
7. [Radio configuration and statistics](#7-radio-configuration-and-statistics)
8. [Mesh configuration](#8-mesh-configuration)
9. [LEDs and OLED](#9-leds-and-oled)
10. [Events](#10-events)
11. [Factory reset](#11-factory-reset)
12. [Errors and limits](#12-errors-and-limits)
13. [Lab-only commands](#13-lab-only-commands)
14. [App integration checklist](#14-app-integration-checklist)

## 1. Connect and frame messages

- UART/CP2102: **115200 baud, 8 data bits, no parity, 1 stop bit**, no hardware
  flow control. Native USB uses the same message format.
- Normal profiles: `heltec_wifi_lora_32_V3` and `native_usb`.
  `secure_dev` / `secure_dev_usb` are compatibility aliases, not different APIs.
- Opening CP2102 resets the tested boards even when DTR/RTS are held inactive.
  Expect a fresh boot and locked vault. Do not assume a USB open preserves RAM work.
- Read continuously. Discard startup/non-JSON lines and synchronize on newline.
  One initial empty newline can terminate an incomplete old input line; it can
  produce an uncorrelated `E_PARSE`. Do not repeatedly prefix requests with blanks.
- Each request/response/event occupies one line ending in `\n`; incoming `\r\n`
  is also accepted. Embedded newlines in strings must be JSON-escaped.
- Maximum line: **2048 bytes including newline**. Oversize input is discarded
  through its newline, followed by an `E_LINE_TOO_LONG` error event.
- One in-flight request at a time is recommended. Events may arrive before,
  between or after command responses; always dispatch them separately.

Request:

```json
{"id":1,"cmd":"sys.ping","args":{}}
```

Success:

```json
{"id":1,"ok":true,"result":{"uptime_ms":15320}}
```

Failure:

```json
{"id":2,"ok":false,"error":{"code":"E_INVALID_ARG","message":"expected an integer within the allowed range","field":"to"}}
```

Event:

```json
{"evt":"sec.state","t":16000,"data":{"state":"locked","reason":"heartbeat_timeout"}}
```

`id` is a signed 64-bit JSON integer, echoed in a valid request's response.
Malformed JSON or an invalid/missing ID can produce a response without `id`.
Use small increasing IDs to avoid JavaScript integer precision problems.
`args` is optional; when present it must be an object. Unknown fields are
ignored. Strings are strict UTF-8 without embedded NUL; lengths are **bytes**,
not characters. Booleans, integers and numbers are not coerced from strings.
Use canonical padded standard Base64, not Base64url, for binary fields.
Treat `code` as the stable error discriminator; `message` is explanatory.

## 2. States and authentication

| State | Meaning |
|---|---|
| `unprovisioned` | No identity vault exists. RF is off. Provision once. |
| `locked` | Vault exists, but private keys are not in RAM. Unlock to use it. |
| `unlocked` | Authenticated identity/contacts are loaded and messaging is enabled. |

Corrupt/incomplete/unavailable vaults fail closed; they are not treated as fresh
devices. Inspect `sys.status` and use authorized recovery rather than overwriting
an existing identity.

| Commands | Allowed state |
|---|---|
| All `sys.*` commands | Any; factory reset additionally requires authorization |
| `radio.get`, `mesh.get`, `led.get` | Any |
| `sec.provision` | `unprovisioned` |
| `sec.unlock` | `locked` |
| `sec.lock`, `sec.rotate_token`, `sec.get_policy`, `sec.set_policy` | `unlocked` |
| `id.*`, `pair.*`, `contact.*`, `msg.*` | `unlocked` |
| `radio.set`, `radio.stats`, `mesh.set`, `led.set`, `led.test` | `unlocked` |

### Token wrapping

Keep a stable, secret **32-byte token T per device** available only to the
registered app. Android should protect or derive it using Keystore-backed
material. The firmware does not prescribe the phone's token derivation; it needs
the same 32 bytes at provisioning and each later unlock.

1. Call `sys.hello` and decode `session.device_eph_pub` (32 bytes) and
   `session.nonce` (16 bytes).
2. Generate a fresh app X25519 key pair. Compute the shared secret with the
   device's ephemeral public key.
3. Decode the 12-hex-digit `device_id` as **six bytes in the displayed order**.
4. Derive `K = HKDF-SHA256(shared, salt=nonce,
   info=ASCII("og-unlock-wrap-v1") || device_id_bytes, length=32)`.
5. Generate a fresh 12-byte IV. AES-256-GCM encrypt T with K, that IV, and
   `AAD=device_id_bytes`. Append the 16-byte GCM tag to the 32-byte ciphertext.
6. Send Base64 of the app's raw 32-byte ephemeral public key as `app_eph_pub`;
   send Base64 of `IV || ciphertext || tag` (60 bytes) as `wrapped_token`.

Every `sys.hello` replaces the previous ephemeral session. Every authentication
attempt consumes it, including malformed, failed, rate-limited or wrong-state
attempts. Obtain a **fresh hello and app ephemeral pair before every attempt**.
Do not log the token, derived keys or wrapped credential.

Working Python wrapping example (`cryptography`), independent of serial I/O:

```python
import base64
import os
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

def wrap_token(hello, token, *, reset=False):
    if len(token) != 32 or hello.get("mock") is not False:
        raise ValueError("expected a 32-byte token and real secure firmware")
    private = X25519PrivateKey.generate()
    public = private.public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    peer = X25519PublicKey.from_public_bytes(
        base64.b64decode(hello["session"]["device_eph_pub"], validate=True))
    nonce = base64.b64decode(hello["session"]["nonce"], validate=True)
    device_id = bytes.fromhex(hello["device_id"])
    label = b"og-reset-wrap-v1" if reset else b"og-unlock-wrap-v1"
    key = HKDF(algorithm=hashes.SHA256(), length=32, salt=nonce,
               info=label + device_id).derive(private.exchange(peer))
    iv = os.urandom(12)
    wrapped = iv + AESGCM(key).encrypt(iv, token, device_id)
    return {"app_eph_pub": base64.b64encode(public).decode("ascii"),
            "wrapped_token": base64.b64encode(wrapped).decode("ascii")}
```

This code does not store the token. Keep credentials outside the repository.
The shipped checker uses private 0600 files for synthetic test identities; that
is not an Android Keystore implementation.

### Heartbeat and lock

Send `sys.ping` about once per second while the app is active, including while
waiting for channel access or receipts. The default inactivity
timeout is 15 seconds. Handled commands count as activity; radio traffic alone
does not. Cable removal is detected by inactivity, not an immediate GPIO event.

Lock cancels pairing and own-message work and clears identity/contact keys and
queued plaintext. It disables RF unless opt-in keyless locked relaying is enabled.
Pending messages/history are not persisted. A new boot invalidates old `msg_ref`
handles; the app must reconcile its own history.

Three consecutive authentication failures start a two-second backoff. Further
failures double it up to 60 seconds. Success clears it. `E_RATE_LIMITED` does
not include a remaining-delay field; back off and start with a fresh hello.

## 3. System commands

### `sys.hello`

Optional args: `app` (0..128 UTF-8 bytes), `app_proto` (`major.minor`, 3..24 bytes).
Major 1 is supported, including minor versions such as `1.4`; another major
returns `E_PROTO_MISMATCH`.

```json
{"id":1,"cmd":"sys.hello","args":{"app":"OffGrid Android","app_proto":"1.0"}}
```

Result fields:

| Field | Value/type |
|---|---|
| `device`, `chip` | `heltec_wifi_lora_32_v3`, `ESP32-S3` |
| `device_id` | Stable 12-digit uppercase hex |
| `fw` | `{version:"0.3.0", build:"<compiler build date>"}` |
| `proto`, `state` | `"1.1"`, current state |
| `mock` | Always `false`; reject old images reporting `true` |
| `radio_diagnostics` | `false` normally; `true` only in explicit lab profiles |
| `authentication`, `config_storage` | `x25519-aes256gcm`, `nvs` |
| `session` | `{device_eph_pub:<Base64 32 bytes>, nonce:<Base64 16 bytes>}` |
| `limits` | `{max_line:2048,max_contacts:32,max_text_bytes:83,pending_queue:8,max_groups:8,max_group_members:16,max_group_text_bytes:83}` |
| `features` | `["groups"]`; firmware without this entry has no `group.*` commands |
| `recovery` | `{master:"ECDSA-P256-SHA256",app_wrap_label:"og-reset-wrap-v1"}` |

### `sys.ping`

No args. Result `{uptime_ms}`. Use as heartbeat and a transport-health check.

### `sys.status`

No args. Always returns:

- `state`, `uptime_ms`, `heap_free`, `battery_mv` (currently `null`), `mock:false`.
- `radio`: `running`, `relay_only`, `initialized`, `last_error` (RadioLib code).
  Running means RF is active, not necessarily that a frame is being transmitted.
- `storage`: `config_status`, `writable`, `vault_status`, `generation`.
- `security`: actual `secure_boot` and `flash_encryption` enablement booleans.
- `ui`: `led_ready`, `oled_ready`, `page` (base Home page = 0), `led_duty`
  (0..255), `strip_pixels` (4), `oled_scene`, `led_pattern`, `strip_rgb`
  (four packed `0xRRGGBB` JSON integers shared by both strips), `led_frame_ms`
  (uptime of the last LED output change).

Unlocked results additionally include:

- `contacts: {used,capacity:32}`.
- `groups: {used,capacity:8}`.
- `queue: {pending,capacity:8,dropped_events,dropped_acks}`.

`config_status` is `defaults`, `loaded`, `invalid`, `unsupported_version` or
`unavailable`. `vault_status` is `missing`, `ready`, `corrupt`,
`unsupported_version`, `incomplete` or `unavailable`. `ready` describes the
stored format, not an unlocked vault; check `state` separately. `writable`
reports configuration-store writability, not successful authentication.

`oled_scene` is the last rendered RAM scene, not an optical measurement:
`starting`, `boot`, `locked`, `unlocking`, `home`, `pairing`, `error`, `reset`, or `radio`.
Tile transmission can still be in progress.

### `sys.reboot`

No args. Returns `{}`, drains the response, then actually reboots.
Wait for `sys.boot` and perform a new hello/unlock. Never resend pending
application messages automatically based only on an interrupted request.

### `sys.set_log_level`

Required `level`: `off`, `error`, `warn`, `info`, `debug`. Returns `{level}`.
RAM-only, defaults to `warn`. Non-off changes emit a `log` notification.
Error responses/events are never disabled by this setting. Keys and token
material are not logged.

For `sys.reset_challenge` / `sys.factory_reset`, see [factory reset](#11-factory-reset).

## 4. Security and identity commands

### `sec.provision`

Only unprovisioned. Required wrapped-token fields; optional `name` (1..24 UTF-8
bytes). The token becomes the vault credential and the device generates its
private identity. Save the credential before sending the request: losing the
response must not make the app lose the only credential.

### `sec.unlock`

Only locked. Required wrapped-token fields containing the existing token.
The vault and all occupied contacts must authenticate/load before unlocked is
published. Wrong token/envelope returns `E_UNLOCK_FAILED`, not simulated success.

Both provisioning and unlocking return:

```json
{"state":"unlocked","node_id":"E3F0","name":"Offgrid-E3F0","public_key":"<Base64 32 bytes>","contacts":0,"radio":{"running":true,"relay_only":false,"last_error":0}}
```

They also emit `sec.state`. Radio initialization may fail independently of
authentication: inspect `radio.running` / `last_error`; do not infer RF readiness
only from `"state":"unlocked"`.

### `sec.lock`

No args. Unlocked only. Result `{state:"locked"}` and `sec.state`.
Pending own work emits `msg.status` with `state:"failed",reason:"locked"`.

### `sec.rotate_token`

Unlocked only. Obtain a fresh hello and wrap the **new** 32-byte token with the
normal unlock label. Result `{state,node_id,name,public_key}`; identity and
contacts stay the same. No reboot or unlock event is required.

Keep both old and new credentials until success is confirmed. If the connection
fails during activation, preserve both and recover explicitly rather than
guessing which token is active. After confirmed rotation, the old token fails.

### `sec.get_policy` / `sec.set_policy`

Unlocked only. Result `{lock_timeout_s}`.
Set accepts optional integer `lock_timeout_s` (0..3600), optional boolean
`persist` (default `true`). Default timeout is 15; zero disables automatic timeout.
Prefer a nonzero timeout. `persist:false` lasts only until reboot.

### `id.get` / `id.set_name`

Unlocked only. Result `{node_id,name,public_key}`.
`id.set_name` requires `name` (1..24 UTF-8 bytes), committed to the encrypted vault.
NodeID is four uppercase hex digits: the first two bytes of SHA-256(public key).
It is a routing hint, not a globally unique identity; use the full public key
for identity comparisons.

## 5. Pairing and contacts

All commands here require unlocked state.

### `pair.begin`

No args. Starts/replaces the device's five-minute pairing session.
Result `{qr,qr_bytes,expires_s:300}`. `qr` is Base64 of:

```text
"OG1" (3 bytes) || identity_public_key (32) || fresh_pairing_salt (16) || optional_UTF8_name (0..24)
```

The QR encoded by the device includes its current name. The parser also accepts
an absent name. QR data is public, not a contact key.

**Both devices must begin pairing and exchange the QR from that same session.**
Show/scan these through the app; no pairing material is exchanged over LoRa.
Do not restart one side's session between generating and scanning its QR.

### `pair.complete`

Required `qr` (canonical Base64, decoded 51..75 bytes).
Optional `name` (local contact name, 1..24 bytes), `replace` (boolean, default false).

Result `{contact:<contact object>,warnings:[]}`; warnings can contain
`node_id_collision`. Both sides should complete, compare the displayed
fingerprints, and keep the contact pending until authenticated traffic arrives.

Self-pairing, invalid QR, duplicate public key without replacement, expired/missing
session and full storage are rejected. Success ends that pairing session.
`replace:true` replaces only a matching existing public-key relationship and
preserves its local slot while resetting keys/counters/replay state. It cancels
queued work for that contact. Re-pair **both** devices, not just one.

### `pair.cancel`

No args. Idempotent. Result `{cancelled:true}`. Wipes the temporary salt.
Lock or five-minute expiry also cancels the session.

### Contact object

```json
{"contact_id":1,"name":"Bob","node_id":"E841","public_key":"<Base64 32 bytes>","state":"pending","fingerprint":"1234 5678 9012","tx_ctr":0,"rx_highest_ctr":null,"last_rssi":null,"last_snr":null}
```

- `contact_id`: stable local slot 1..32, independent on each device.
- `state`: `pending` or `active`. A first authenticated accepted packet activates it.
- `fingerprint`: 12 decimal digits grouped 4/4/4, identical on both devices.
- `tx_ctr`: highest consumed/reserved TX position; reboot/unlock can skip forward
  because unused durable reservations are never reused.
- `rx_highest_ctr`: highest authenticated accepted peer counter, or `null`.
- Signal fields: last accepted packet RSSI/SNR, or `null`; RAM-only, cleared on load.

No private key or symmetric key is included.

### `contact.list`

Optional `offset` (0..32, default 0), `limit` (1..10, default 10).
Result `{contacts:[...],total,offset}`.

Offset counts **occupied contacts**, not slot IDs. The actual page can contain
fewer than `limit` contacts to keep the whole response under 2048 bytes.
Advance by the returned array length until accumulated offset reaches `total`.
There is no `next_offset` or `more` field.

### `contact.get`

Required `contact_id` (1..32). Result `{contact}`.

### `contact.rename`

Required `contact_id`, `name` (1..24 bytes). Result `{contact}`.
Persists only the local name; it does not change keys or notify the peer.

### `contact.delete`

Required `contact_id`, `confirm:true`. Result `{deleted:true}`.
Durably removes that relationship and cancels its own queued messages with
`reason:"contact_deleted"`. It does not delete the peer's copy or chat history.
Groups keep the member. On the owner its `contact_id` reads `null` and
`group.sync` to it fails until the peer is paired again in the same slot; to
replace it, `group.remove` the old index and `group.add` the new contact.

## 6. Messages

All `msg.*` commands require unlocked state. Counters are per contact/direction,
shared by new data and fresh ACKs, start at 1 and never wrap past 16777215.
Re-pair before exhaustion. A retry reuses its original counter with a new RETRY
value/nonce; a manual resend is a new message with a new counter.

### `msg.send`

Common args:

| Arg | Required | Meaning |
|---|---|---|
| `to` | Yes | Local contact slot 1..32 |
| `type` | Yes | `text`, `reaction`, `geo` |
| `ack` | No | Override the configured default for this type |
| `hops` | No | 0..7, default `mesh.hop_default`; 0 disables forwarding |

Type-specific args:

| Type | Args |
|---|---|
| `text` | Required `text`, 0..83 UTF-8 bytes. No compression or fragmentation. |
| `reaction` | Required `target:{author:"me" or "peer",ref:0..65535}` and `emoji` |
| `geo` | Required finite `lat` (-90..90), `lon` (-180..180); optional nullable integer `alt_m` (-32768..32767), `age_s` (0..2550) |

Reaction `ref` is the low 16 bits of the target message counter. `author` is
relative to the app sending this command; incoming events invert it appropriately.
One of these exact emoji strings is accepted: `👍`, `❤️`, `😂`, `😮`, `😢`, `🙏`.
Use `"emoji":null` to remove a reaction. The heart includes U+FE0F.
History lookup and orphan reaction handling belong to the app.

GEO is quantized to 24 bits per coordinate (about 1.2 m latitude / 2.4 m longitude
at the equator). Upper endpoints are clamped, not wrapped. Age is rounded to
nearest 10 seconds and capped at 2550. Absent/null altitude and age stay `null`
in received events.

```json
{"id":20,"cmd":"msg.send","args":{"to":1,"type":"text","text":"Hello!","ack":true}}
{"id":20,"ok":true,"result":{"msg_ref":1,"ctr":1,"ack_requested":true,"hops":3,"state":"queued","bytes_on_air":23,"est_airtime_ms":206}}
```

`msg_ref` is a local uint32 handle valid for the current boot only.
`est_airtime_ms` is an estimate at the currently applied PHY configuration;
it is not a channel-access or delivery deadline. `queued` means accepted into
one of eight RAM slots, **not sent or delivered**.

ACKs are authenticated, bound to the pending original counter and matched only
after real transmission. Actual TxDone starts the retry timeout. Default automatic
base timeout is `(2*hops+2)*(measured_airtime_ms+750)`, multiplied by `2^retry`.
`mesh.retry_base_ms` can override the base. Maximum is three retries after the
initial transmission; retries and channel contention can make delivery take much
longer than a command response.

### `msg.pending`

No args. Result:

```json
{"pending":[{"msg_ref":1,"to":1,"type":"text","ctr":1,"state":"sent","retry":0}]}
```

Group messages carry `group_id` instead of `to`; a `group_sync` carries both.

Lists own RAM work only, not ACK/relay queues or message history. Terminal work
is removed. Pending contents are lost on reboot/lock; they are not a persistent inbox.

### `msg.cancel`

Required `msg_ref` (1..4294967295). Result `{state:"cancelled"}`, followed by a
`msg.status` event. Unknown/already-terminal handles return `E_INVALID_ARG`.
In-progress radio work is stopped where possible; already transmitted data
cannot be unsent.

### Message status

| State | Meaning |
|---|---|
| `queued` | Accepted; appears in the send response/pending view |
| `sent` | Physical TxDone; terminal when no ACK was requested |
| `relayed` | Matching neighbour repeat overheard; not end-to-end delivery |
| `retrying` | A retry is queued for channel/airtime access |
| `delivered` | A fresh authenticated bound ACK was accepted |
| `failed` | Terminal; `reason` is `timeout`, `locked`, `contact_deleted`, `radio_error` |
| `cancelled` | Terminal app cancellation |

Only `delivered` proves an authenticated peer receipt. `rtt_ms` in that event is
time from local enqueue to ACK acceptance, including waits/retries, not pure RF RTT.
Duplicate authenticated data is not delivered twice; a retry can get a fresh ACK.
Stale data outside the 64-counter replay window is dropped.

### Groups

All `group.*` commands require unlocked state (`E_NOT_PROVISIONED` or
`E_LOCKED` otherwise). The device that creates a group is its owner; only the
owner changes members. Every member must be paired with the owner, not with
the other members. On-air format and limits:
[radio protocol section 8.5](offgrid-protocol.md#85-groups).

Group object:

| Field | Value |
|---|---|
| `group_id` | Local slot 1..8 |
| `gid` | 8 uppercase hex digits, the same on every member |
| `name` | 1..24 UTF-8 bytes, set by the owner |
| `epoch` | 1..255; every member change increments it |
| `owner` | `true` on the owner device |
| `owner_contact_id` | The owner's contact slot on a member, `null` on the owner |
| `member`, `member_count` | Own index (owner 0) and member count including the owner |
| `members` | `[{member, node_id, contact_id or null}]`, not in `group.list` |

A member sees the other members by NodeID; `contact_id` is filled in when a
contact with that NodeID exists on this device.

| Command | Args | Result |
|---|---|---|
| `group.create` | `name`, `members` (1..15 distinct contact IDs) | `{group}`; local only, sync each member next |
| `group.list` | none | `{groups:[group without members], total}` |
| `group.get` | `group_id` | `{group}` |
| `group.sync` | `group_id`, `contact_id`, optional `hops` | Like `msg.send`; owner only |
| `group.add` | `group_id`, `members` (contact IDs) | `{group}` with a new epoch; owner only |
| `group.remove` | `group_id`, `members` (member indexes, not 0) | `{group}` with a new epoch, at least 2 members stay; owner only |
| `group.send` | `group_id`, `type` (`text` or `geo`) with the `msg.send` fields, optional `hops` | Like `msg.send`, `ack_requested:false` |
| `group.delete` | `group_id`, `confirm:true` | `{deleted:true}`; this device only |

`group.sync` sends the current key and member list to one member as an
acknowledged DM; follow it with `msg.status` (`type` `group_sync`). After
`group.create`, `group.add` and `group.remove`, sync every member that is still
in the group. A change or delete cancels queued syncs of the old epoch. Removed
members are not told; they keep the old key but cannot read the new epoch.

Group messages have no receipts and no retries: `ack:true` returns
`E_INVALID_ARG`, and `sent` is the final state. Reactions are not supported in
groups.

```json
{"id":30,"cmd":"group.create","args":{"name":"Team","members":[1,2]}}
{"id":30,"ok":true,"result":{"group":{"group_id":1,"gid":"1A2B3C4D","name":"Team","epoch":1,"owner":true,"owner_contact_id":null,"member":0,"member_count":3,"members":[{"member":0,"node_id":"E3F0","contact_id":null},{"member":1,"node_id":"E841","contact_id":1},{"member":2,"node_id":"5C07","contact_id":2}]}}}
{"id":31,"cmd":"group.sync","args":{"group_id":1,"contact_id":1}}
{"id":31,"ok":true,"result":{"msg_ref":7,"ctr":3,"ack_requested":true,"hops":3,"state":"queued","bytes_on_air":50,"est_airtime_ms":329}}
{"id":32,"cmd":"group.send","args":{"group_id":1,"type":"text","text":"Hallo Gruppe!"}}
{"id":32,"ok":true,"result":{"msg_ref":8,"ctr":1,"ack_requested":false,"hops":3,"state":"queued","bytes_on_air":30,"est_airtime_ms":227}}
```

## 7. Radio configuration and statistics

### `radio.get` / `radio.set`

Get works in every state; set requires unlocked.
Set accepts any subset of the following plus `persist` (boolean, default true).
It validates the whole candidate before changing live settings. A failed flash
write leaves the live candidate unchanged. A hardware application failure can
occur **after** settings were committed; inspect `radio.get` and `sys.status`
instead of assuming `E_RADIO` rolled back the saved configuration.

| Field | Range | Default |
|---|---|---|
| `freq_mhz` | Finite number, 150..960 | 869.525 |
| `bw_khz` | 62.5, 125, 250, 500 | 125 |
| `sf` | Integer 5..12 | 9 |
| `cr` | Integer 5..8 (4/5..4/8) | 5 |
| `tx_power_dbm` | Integer -9..22 | 14 |
| `preamble` | Integer 6..64 symbols | 8 |
| `sync_word` | Integer 0..255 | 79 (0x4F) |

Set-only `preset` resets the full radio candidate before explicit fields:
`lab_default` (SF9/BW125), `fast` (SF7/BW250), `long_range` (SF11/BW125).
Each preset uses 869.525 MHz, CR5, 14 dBm, preamble8 and sync79.

Result is the complete configuration plus `est_airtime_ms_23b`,
`max_text_bytes:83`. Set also includes:

- `warnings`: possible `outside_eu868`, `above_eu868_power_limit`,
  `reserved_sync_word`, and `mesh_incompatible`.
- `apply_pending`: true if waiting for current RF work to finish.

Warnings do not authorize unlawful operation or guarantee antenna compatibility.
Match radio settings on every participating node. Avoid reserved sync words
0x12 and 0x34. SX1262 expands the configured byte using control bits 0x44.

All `.set` settings default to persistence. `persist:false` changes RAM only.
A later persisted change saves the **whole current category**, including earlier
temporary changes in that category; unrelated categories are not persisted.

### `radio.stats`

Unlocked only. Optional `reset` boolean (default false) returns then clears the
counters. It does not reset keys, replay windows or rate budgets.

| Fields | Meaning |
|---|---|
| `tx_packets`, `rx_packets` | Completed PHY transmissions / received PHY packets |
| `rx_crc_errors`, `rx_tag_failures`, `rx_malformed` | PHY/authentication/format failures |
| `rx_dedupe_drops` | Endpoint replay duplicate/stale drops |
| `relayed`, `relay_suppressed`, `relay_dropped`, `rate_limit_drops` | Relay completion, cancellation, bounded loss and rate-limit drops |
| `acks_sent`, `acks_received`, `retries` | ACK completions/acceptances and retries scheduled |
| `tx_airtime_ms_total` | Measured airtime, including known interrupted/failed TX |
| `cad_scans`, `cad_busy` | Channel scans and busy results |
| `radio_failures`, `radio_timeouts` | Physical driver failures/timeouts |
| `dropped_events`, `dropped_acks`, `radio_dropped_events`, `trace_dropped_events` | Bounded queue/trace losses |
| `last_rssi`, `last_snr` | Latest PHY signal values; initially zero |
| `tx_duty_cycle_max_percent` | `null`: no enforced duty-cycle limit |

Lab builds additionally return `injected_ack_drops`.

## 8. Mesh configuration

`mesh.get`: any state. `mesh.set`: unlocked, partial update plus `persist`.
Both return the complete object:

| Field | Range/type | Default |
|---|---|---|
| `hop_default` | Integer 0..7 | 3 |
| `relay_enabled` | Boolean | true |
| `relay_when_locked` | Boolean | false |
| `cad_enabled` | Boolean | true |
| `backoff_slots` | Integer 0..32 | 8 |
| `retries` | Integer 0..3 | 3 |
| `retry_base_ms` | Integer 0..60000; 0 = automatic | 0 |
| `rate_limit_per_src` | Integer 0..255/minute; 0 disables relay count limit | 20 |
| `ack_default` | Partial object with boolean `text`, `reaction`, `geo` | `{text:true,reaction:false,geo:false}` |

ACKs/own messages have priority over transit traffic. Relays cannot decrypt
unknown contacts. Relaying uses eight transit slots, a 256-entry normalized keyed
cache, randomized/SNR-weighted delay and overhear cancellation. Authentication
and replay handling precede relay dedupe; short-ID collisions do not swallow
transit packets. A separate fixed tag-failure budget remains even when relay
rate limiting is disabled.

Locked relaying needs **both** relay booleans true and a provisioned device.
It is keyless: it does not unlock contacts or deliver private messages.
Unprovisioned firmware never enables RF, regardless of saved policy.

No duty-cycle cooldown or airtime budget is enforced in any profile. CAD,
collision backoff, higher-priority work and retry timing still gate transmission;
keep heartbeat active. Measured airtime is retained for statistics and retry
timeouts. The operator must ensure the lab authorization permits the actual
frequency, power and airtime used.

## 9. LEDs and OLED

### `led.get` / `led.set`

Get: any state. Set: unlocked, partial update plus `persist`.

| Field | Type/default |
|---|---|
| `enabled` | Boolean, true |
| `quiet` | Boolean, false |
| `brightness` | Integer 0..255, 128 |
| `state_patterns` | Partial object: `unprovisioned`, `locked`, `unlocked` |
| `event_patterns` | Partial object: `tx`, `rx`, `msg_received`, `delivered`, `failed`, `error`, `pairing` |

Result includes full configuration and `builtin_patterns`.

Patterns are a name or custom object:

```json
{"steps":[100,100,100,700],"repeat":2}
```

Smooth named patterns:

| Name | RGB animation / one cycle |
|---|---|
| `glow` | Blue breathing flow, 5000 ms |
| `send` | White travelling wave from pixel 3 to 0, 1600 ms |
| `receive` | Broader white wave from pixel 0 to 3, 1600 ms |
| `pairing` | Purple breathing flow, 3600 ms |
| `startup` | Blue growing/ripple/fill intro, 6600 ms |
| `reset` | Red bloom/fade, 1800 ms |
| `alert` | Gentle red bloom/fade, 2000 ms |

Legacy names remain available: `off`, `on`, `flash`, `blink_slow`, `blink_fast`,
`double`, `heartbeat`, `pulse`. Custom steps: 1..16 integer durations, each
10..10000 ms, alternating
on/off from on. `repeat` is optional, 1..255, default 1. State patterns always
loop; event/preview patterns finish. Finite on/off cycles last 1000 ms each;
pulse lasts 1500 ms.

Default states: unprovisioned/locked `glow`, unlocked `off`. Events: TX `send`,
raw RX `off`, authenticated new message `receive`, delivered `off`,
failure/error `alert`, pairing `pairing`. Pairing loops while its session is active.
An `off` event means no animation override; it does not cancel an ongoing wave.
Same-kind activity coalesces instead of repeatedly restarting a wave.

Smooth selections crossfade over 250 ms. Both four-pixel GRB strips share the
same RGB frames; the monochrome onboard LED follows the brightest channel.
`enabled:false` or `quiet:true` forces all outputs off, including startup/reset.
Neither setting disables the OLED. Factory startup/reset use fixed named
animations; previews of those names affect LEDs only.

Settings schema 1 upgrades once to schema 2. Entries matching old factory
patterns become the defaults above; nonmatching custom patterns and all
other settings are retained. A failed migration is reported, not silently saved.

### `led.test`

Unlocked. Required `pattern`, optional outer `repeat` (1..255, default 1).
Outer repeat wins over a custom pattern's repeat.
Result `{preview:true,repeat}`. Does not persist configuration.

### OLED behavior

No display-control command is required. Power-on plays the synchronized
6.6-second OLED/blue LED intro once, then locked state shows a large closed
padlock. A genuine unlock opens it and shrinks/moves it into the Home corner
over 1400 ms. Unlock/relock interrupts startup immediately. Relock interrupts
the unlock transition. Authorized reset shows "Factory reset".
Pairing/fingerprint/error/
activity overlays are automatic. There is no button or page-advance workflow.
Use `sys.status.ui` for readiness and last rendered scene.

## 10. Events

Every event has `{evt,t,data}`; `t` is device uptime in milliseconds, not wall
clock time. Do not reply to events.

| Event | Data |
|---|---|
| `sys.boot` | `device_id`, `fw` (version string), `proto`, `state`, `mock:false`, `radio_diagnostics`, `reset_reason` (`software` or `power_on`) |
| `sec.state` | `state`, `reason`: `provision`, `unlock`, `lock`, `heartbeat_timeout`, `factory_reset`, or `error` |
| `contact.state` | `contact_id`, `state:"active"` |
| `msg.status` | `msg_ref`, `to` and/or `group_id`, `state`, `retry`; optional `reason`; delivered also has `hops`,`rtt_ms` |
| `msg.received` | `from`, `ctr`, `retry`, `hops`, `rssi`, `snr`, plus typed payload |
| `group.state` | `group_id`, `state` (`joined` or `updated`), `epoch`; a sync from the owner was applied |
| `group.received` | `group_id`, `member`, `node_id`, `from` (contact ID or `null`), `ctr`, `retry`, `hops`, `rssi`, `snr`, plus typed payload (`text` or `geo`) |
| `log` | `level`, `msg` (no keys/tokens) |
| `error` | `code`, `message`; unsolicited transport/storage/RF/loss notice |

Received payload examples:

```json
{"evt":"msg.received","t":7000,"data":{"from":1,"type":"text","ctr":1,"text":"Hello!","hops":0,"retry":0,"rssi":-72,"snr":9.0}}
{"evt":"msg.received","t":9000,"data":{"from":1,"type":"reaction","ctr":2,"target":{"author":"me","ref":1},"emoji":"👍","hops":0,"retry":0,"rssi":-72,"snr":9.0}}
{"evt":"msg.received","t":11000,"data":{"from":1,"type":"geo","ctr":3,"lat":47.0517,"lon":8.30751,"alt_m":null,"age_s":null,"hops":0,"retry":0,"rssi":-72,"snr":9.0}}
{"evt":"group.state","t":12000,"data":{"group_id":1,"state":"joined","epoch":1}}
{"evt":"group.received","t":13000,"data":{"type":"text","text":"Hallo Gruppe!","group_id":1,"member":0,"node_id":"E3F0","from":1,"ctr":1,"retry":0,"hops":0,"rssi":-72,"snr":9.0}}
```

Group texts arrive as `group.received`, never as `msg.received`, so an app
that does not know groups cannot file them into a direct chat. On a `joined`
event, read the members with `group.get`.

There is no incoming-message ACK command: the device handles authenticated
radio receipts automatically. Store received content/history in the app.

### Backpressure and reconnect

The transport has six output slots, and the domain has 32 event slots. RF,
replay commits and UI progress never wait for the app to read. Extended host
backpressure can therefore lose notifications/content; loss counters and a
subsequent error report it. ACK receipt does not guarantee the app has saved
the message. There is no persistent inbox or event replay API.

Continuously drain serial even while requests are pending. On boot/reconnect,
perform hello, unlock, reload contacts and inspect pending work/loss counters.
Terminal events lost under backpressure cannot be reconstructed from
`msg.pending`; keep uncertain app states explicit instead of fabricating delivery.

## 11. Factory reset

Reset erases the identity, all contact keys and device settings, then reboots
unprovisioned. It is not a normal way to recover a lost message. Authorization is
durably journaled before the successful response; an interrupted authorized reset
is resumed on reboot. There is no button/hold reset and no unauthenticated erase.

Both methods require `"confirm":"ERASE"` exactly. Result `{pending:true}` is
returned before the red 1800-ms animation completes and erase/reboot begins;
wait for `sys.boot` with `state:"unprovisioned"`. Command reception pauses during
this pending-reset interval; it is not an opportunity to cancel authorization.

### Registered-app authorization

Works locked or unlocked if the existing vault can be authenticated.
Call a fresh `sys.hello`, wrap the **current** token using
`info=ASCII("og-reset-wrap-v1") || device_id_bytes`, then send:

```json
{"id":40,"cmd":"sys.factory_reset","args":{"confirm":"ERASE","method":"app","app_eph_pub":"<Base64 32 bytes>","wrapped_token":"<Base64 60 bytes>"}}
```

`method` defaults to `app`. An ordinary unlock/rotation proof cannot authorize
reset because its HKDF label differs.

### Owner-signed maintenance authorization

1. Call `sys.reset_challenge` (no args).
2. Decode `message` and verify that it is exactly 49 bytes:
   `ASCII("og-reset-v1") || device_id_bytes || random_nonce32`.
3. Verify that `key_id` identifies the owner's expected trust key:
   Base64 of SHA-256(the raw 65-byte uncompressed P-256 public point).
4. Sign the message using the owner-held P-256 private key and ECDSA/SHA-256.
   Encode the ASN.1 DER signature (at most 72 decoded bytes) as canonical Base64.
5. Submit within `expires_s:30`:

```json
{"id":41,"cmd":"sys.factory_reset","args":{"confirm":"ERASE","method":"master","signature":"<Base64 DER signature>"}}
```

Challenge result is `{message,key_id,expires_s:30,algorithm:"ECDSA-P256-SHA256"}`.
A new challenge replaces the old one; every verification attempt consumes it,
including invalid signatures. It is device-bound and cannot be replayed.

Only the public trust key is embedded in firmware. Never place the owner's
private signing key on a phone/client that should not have fleet reset authority.
Maintenance reset grants erasure, not disclosure of another user's vault.
No irreversible eFuses/download-mode settings are modified by this API.

## 12. Errors and limits

| Code | Meaning/action |
|---|---|
| `E_PARSE` | Malformed/trailing JSON; recover at the next newline |
| `E_LINE_TOO_LONG` | Oversize line dropped; event, not a normal correlated response |
| `E_INVALID_ARG` | Missing/wrong-typed/out-of-range value or invalid pending handle |
| `E_UNKNOWN_CMD` | Unsupported command; includes lab-only commands on normal firmware |
| `E_PROTO_MISMATCH` | Unsupported host protocol major |
| `E_NOT_PROVISIONED` | A protected operation needs an existing identity |
| `E_LOCKED` | Unlock required; some protected groups also use this when unprovisioned |
| `E_ALREADY_PROVISIONED` | Provisioning cannot overwrite an existing vault |
| `E_ALREADY_UNLOCKED` | Unlock called while already unlocked |
| `E_NO_SESSION` | Obtain a fresh hello before authentication |
| `E_UNLOCK_FAILED` | Credential/envelope/reset authorization rejected |
| `E_RATE_LIMITED` | Authentication backoff; retry later with a fresh session |
| `E_CONFIRM_REQUIRED` | Missing exact erase/delete confirmation |
| `E_NO_PAIRING` | Pairing session missing, completed, cancelled or expired |
| `E_INVALID_QR` | Unsupported/invalid QR or unusable peer key |
| `E_SELF_PAIR` | Own identity scanned |
| `E_CONTACT_EXISTS` | Existing public-key relationship; explicit replacement required |
| `E_NO_SUCH_CONTACT` | Empty contact slot |
| `E_NO_SUCH_GROUP` | Empty group slot |
| `E_STORAGE_FULL` | All 32 contact slots or all 8 group slots occupied |
| `E_TOO_LONG` | Text exceeds 83 bytes or response cannot fit |
| `E_QUEUE_FULL` | Eight own message slots already occupied |
| `E_BUSY` | Counter exhausted; re-pair rather than wrapping. For groups also: epoch 255 reached, create a new group |
| `E_RADIO` | Radio unavailable/configuration failed; inspect status |
| `E_INTERNAL` | Crypto/storage/capacity failure; inspect status/error events, do not assume success |

Wrong authorization can deliberately be reported uniformly. Do not use error
text to probe token validity. Storage failures in contact/counter/replay work
can lock the device; handle the subsequent `sec.state`.

| Limit | Value |
|---|---|
| Request/response line | 2048 bytes including newline |
| JSON nesting | 8 |
| Names | 1..24 UTF-8 bytes |
| Contacts | 32 |
| Groups | 8, each with 2..16 members including the owner |
| Contact page requested size | 1..10; actual result can be smaller |
| Own message slots | 8 |
| Text body | 0..83 UTF-8 bytes |
| Complete mesh packet | 100 bytes |
| Counter | 1..16777215; zero reserved, no wrapping |
| Replay window | 64 counters; accepted highest committed before delivery |
| Pairing expiry | 300 seconds |
| Maintenance challenge expiry | 30 seconds |

## 13. Lab-only commands

Only `radio_diag` / `radio_diag_usb` include these. They use the same real secure
backend, not an authentication bypass. Normal profiles return `E_UNKNOWN_CMD`.

| Command | State / result |
|---|---|
| `diag.led.selftest` | Any state; `{passed:true}` or `E_INTERNAL`; RGB continuity, colors, direction and expiry |
| `diag.codec.selftest` | Any state; `{passed:true}` or `E_INTERNAL`; published vectors/bounds |
| `diag.vault.selftest` | Any state; optional `verify` boolean; `{passed:true}` or `E_INTERNAL` |
| `diag.mesh.drop_next_ack` | Unlocked; `{armed:true}`; deliberately lose exactly one requested receipt |
| `dbg.rf_trace` | Unlocked; required `enabled` boolean; result `{enabled}` |

Vault diagnostics modify only the separate `og_vtest` namespace and do not
transmit. They cover storage, reservations/replay, message queues, relay rules,
source-budget isolation and airtime bounds. They do not erase the real vault.
Receipt-loss injection and trace are cleared on lock.

Trace events contain actual PHY ciphertext, never private keys:

```json
{"evt":"rf.tx","t":10000,"data":{"len":23,"hex":"6CE841E3F00000015D1D3FD92A3E04DA794024DB76ADBC","airtime_ms":206}}
{"evt":"rf.rx","t":10200,"data":{"len":16,"hex":"48E3F0E8410000017F15646B08384B1D","rssi":-90,"snr":6.5,"action":"ack_matched"}}
```

RX `action`: `delivered`, `ack_matched`, `relay_queued`, `dropped_duplicate`,
`dropped_tag`, `dropped_hop_limit`, `dropped_rate_limit`, `dropped_malformed`,
`dropped_queue_full`, `ignored`. Scheduling a relay is not a completed forward;
a later `rf.tx` records actual transmission. Trace loss is counted, never
allowed to stall RF progress.

## 14. App integration checklist

1. Open serial, synchronize on newline, continuously dispatch events/responses.
2. Call hello; require `proto` major 1 and `mock:false`. Identify the board by
   `device_id`, not a USB port name.
3. Obtain the right per-device token. Provision only when explicitly
   unprovisioned; otherwise unlock. Never auto-reset an unknown/corrupt vault.
4. Start one-second heartbeats and keep them running during all asynchronous work.
5. Load identity and paginated contacts. Render/scan both pairing-session QRs and
   compare fingerprints when adding a peer.
6. Store chat history in the app. Track queued messages by boot-local `msg_ref`;
   distinguish physical `sent` from authenticated `delivered`.
7. Keep reading during channel waits/retries. Surface RF/storage/loss errors and
   uncertain outcomes instead of reporting fabricated success.
8. On boot/lock, discard stale RAM handles and sensitive client views. Re-unlock
   explicitly; do not automatically erase/re-pair or resend ambiguous messages.
9. Use `persist:false` for temporary settings and restore them deliberately.
10. Before closing, stop heartbeats and optionally send `sec.lock`; inactivity
    will lock later unless the configured timeout is zero.
