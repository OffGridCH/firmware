# Offgrid Protocol v1: Design Specification

> **Status:** Implemented v1 wire protocol; physical three/four-board mesh acceptance pending
> **Purpose:** Encrypted, meshed, infrastructure-free chat over raw LoRa
> **Message types:** text, reaction, geolocation
> **Out of scope for v1:** group chats, forward secrecy, store-and-forward, fragmentation

---

## Table of Contents

1. [Requirements](#1-requirements)
2. [Plain-Language Summary](#2-plain-language-summary-no-crypto-degree-required)
3. [Why LoRa and Not LoRaWAN](#3-why-lora-and-not-lorawan)
4. [Architecture and Radio Parameters](#4-architecture-and-radio-parameters)
5. [Identity and Key Exchange](#5-identity-and-key-exchange)
6. [Encryption](#6-encryption)
7. [Packet Format](#7-packet-format)
8. [Message Types](#8-message-types)
9. [Message IDs, Counters, Dedupe and Replay Protection](#9-message-ids-counters-dedupe-and-replay-protection)
10. [Receive Receipts (ACK)](#10-receive-receipts-ack)
11. [Mesh Relaying](#11-mesh-relaying)
12. [Reliability: Retries and Staying in Sync](#12-reliability-retries-and-staying-in-sync)
13. [Known Gaps and How They Are Closed](#13-known-gaps-and-how-they-are-closed)
14. [Limitations and Future Work](#14-limitations-and-future-work)
15. [Worked Example: Byte by Byte](#15-worked-example-byte-by-byte)
16. [Appendix A: Implementation and Verification](#appendix-a-implementation-and-verification)
17. [Appendix B: Constants](#appendix-b-constants)

---

## 1. Requirements

| # | Requirement | How it is met |
|---|---|---|
| R1 | End-to-end encryption | AEAD (AES-128-CCM) with per-contact keys. Relays never hold keys |
| R2 | Meshing (hopping over other radios) | Managed flooding with hop limit and duplicate suppression |
| R3 | Three message types | Text, Reaction, Geolocation |
| R4 | Key exchange **not** over the air | QR code scanned in person (alternatives: NFC, BLE) |
| R5 | Receive confirmations | Tiny 16-byte ACK, cryptographically bound to the original message |
| R6 | Minimal bytes | Binary format, 8-byte header, 8-byte tag, no JSON, no explicit IDs in ACKs |

**Design principle:** every byte on air costs time. Anything a receiver can derive locally is never transmitted.

---

## 2. Plain-Language Summary (No Crypto Degree Required)

If you only read one section, read this one.

### The big picture

Imagine two friends who want to send each other secret postcards across a city using only walkie-talkies, with no phone network and no internet.

**1. Meeting in person (key exchange).**
Before anything else, the two friends meet and scan each other's QR code. This is like swapping two halves of a secret code in person. Nobody can intercept it because it never travels through the air. From these QR codes both phones independently calculate the *same* secret keys. The keys themselves are never sent anywhere.

**2. Sealing the message (encryption).**
Every message is locked with the shared key before sending. Anyone listening can see *that* a message exists and roughly who it is from and to, but not what it says. If someone changes even one bit, the receiver notices and throws the message away.

**3. Shouting through the crowd (meshing).**
LoRa radios only reach a few kilometres. If the friend is too far away, other Offgrid devices in between pick up the message and **repeat it**, like people passing a note along a row. Each message carries a small "hops left" counter, so it does not travel forever.

**4. Not repeating the same thing twice (dedupe).**
Each message has a **number** from its sender (1, 2, 3, ...). Every device remembers the messages it has already seen and never repeats the same one twice. Otherwise the same message would bounce around the network forever.

**5. The receipt (ACK).**
When the message arrives, the receiver sends back a very short receipt. The clever part: **the receipt does not say which message it is for**. Instead, the receiver seals the receipt *using the message number as a secret ingredient*. The sender tries its few waiting messages against the receipt, and exactly one matches. That saves bytes and nobody can forge a receipt, because forging needs the secret key.

**6. If something gets lost (reliability).**
If no receipt arrives, the sender simply tries again with the **same message number** plus a "retry" marker. The receiver recognises the duplicate, does not show the message twice, and re-sends the receipt. No shared state needs repairing, so the two devices cannot get "out of sync".

### Cheat sheet

| Concept | Everyday analogy | Technical name |
|---|---|---|
| QR scan | Swapping secret code halves in person | X25519 key exchange |
| Shared key | A padlock only you two have the key to | AES-128 key via HKDF |
| Message number | A ticket number on each postcard | 24-bit counter (CTR) |
| "Hops left" | A note that says "pass this on 3 more times" | HOP_LEFT |
| Seal on the envelope | Wax seal that breaks if tampered with | MIC / authentication tag |
| Receipt | A stamp only the right person can make | Empty AEAD packet with bound AD |
| Retry | Sending the same postcard again, marked "2nd attempt" | RETRY field |

### What the status icons mean

| Icon | Meaning |
|---|---|
| ⏳ | Waiting to be sent |
| ✓ | Sent (or a neighbour was heard repeating it) |
| ✓✓ | Confirmed by the recipient |
| ✗ | Gave up after 3 retries |

---

## 3. Why LoRa and Not LoRaWAN

**LoRa** is only the radio modulation. **LoRaWAN** is a full network architecture on top of it. Offgrid uses **raw LoRa** and defines its own MAC and mesh layer.

| Aspect | LoRaWAN | Offgrid needs |
|---|---|---|
| Topology | Star of stars: devices, gateways, network server | Peer-to-peer mesh |
| Infrastructure | Gateway and network server required | None |
| Device-to-device | Not possible | Required |
| Relaying | Not part of the spec | Core feature |
| Key exchange | OTAA join (over the air) or hardcoded keys (ABP) | Out-of-band only |
| E2E trust | Network server knows the NwkSKey | True E2E |
| Payload | ca. 51 bytes at SF12 | Flexible |
| Fair use | e.g. TTN: ca. 30 s airtime/day/device | Not applicable |

Recommended hardware: SX1262-based boards (Heltec V3, RAK4631, LilyGO T-Beam) or SX1276-based boards.

---

## 4. Architecture and Radio Parameters

```
┌───────────────────────────────────────────────┐
│ Application   Text · Reaction · Geo · ACK     │
├───────────────────────────────────────────────┤
│ Security      AES-128-CCM (AEAD), per contact │
├───────────────────────────────────────────────┤
│ Mesh / MAC    Header, flooding, dedupe, retry │
├───────────────────────────────────────────────┤
│ PHY           LoRa (SX126x / SX127x)          │
└───────────────────────────────────────────────┘
```

### Default radio parameters

| Parameter | Value |
|---|---|
| Frequency | EU868 (CH/EU) |
| Bandwidth | 125 kHz |
| Spreading factor | SF9 (configurable SF7–SF12) |
| Coding rate | 4/5 |
| Preamble | 8 to 16 symbols |
| Sync word | Custom, **not** 0x34 (LoRaWAN public) and **not** 0x12 |
| CRC | On (PHY-level) |

All nodes in a mesh must use identical parameters.

### Approximate airtime (BW125, CR 4/5, 40-byte packet)

| SF | Airtime |
|---|---|
| 7 | ca. 80 ms |
| 9 | ca. 290 ms |
| 10 | ca. 530 ms |
| 12 | ca. 2 s |

A 16-byte ACK at SF9 takes ca. 165 ms.

### Airtime pacing and regulatory limits

Own data, ACKs and relays share a
99-times-measured-airtime cooldown after each TX, including interrupted/failed
transmits with known airtime. This conservatively paces to 1% during a continuous
boot, but is not a durable rolling-hour ledger across resets or a certification
of lawful frequency/power/antenna use in every jurisdiction.

---

## 5. Identity and Key Exchange

### 5.1 Identity

Each device generates **one X25519 key pair** once.

```
NodeID = SHA256(public_key)[0:2]      (16 bit)
```

The NodeID does **not** need to be unique. Collisions are resolved by the authentication tag (see [section 11](#11-mesh-relaying)).

### 5.2 Pairing (out of band)

Both users show their QR code and scan the other's.

```
QR payload = "OG1" | public_key (32 B) | pairing_salt (16 B, random) | name (optional, UTF-8)
```

### 5.3 Key derivation

Both sides compute the same values independently:

```
dh      = X25519(own_private, peer_public)
salt    = salt_X || salt_Y            // ordered by public key, byte-wise ascending (role independent)
k_A→B   = HKDF-SHA256(ikm = dh, salt = salt, info = "og1" || pubA || pubB, L = 16)
k_B→A   = HKDF-SHA256(ikm = dh, salt = salt, info = "og1" || pubB || pubA, L = 16)
```

### 5.4 One key or two?

| Layer | Count |
|---|---|
| Key pair per device | **1** (X25519) |
| Shared secret per contact | **1** (DH output) |
| Symmetric keys per contact | **2**, one per direction |

Why two directional keys:

- **Independent nonce spaces.** A and B count their messages separately. With one shared key a nonce collision between A and B would be possible, and a nonce collision in CCM is catastrophic.
- **No reflection attacks.** A packet sent by A can never verify as "from B" at A.

### 5.5 Why the pairing salt matters

If a device is reset, its counters restart at 0. Without a fresh salt the derived keys would be identical, causing **nonce reuse**. With a new salt, every re-pairing produces completely new keys. **After any reset or restore, re-pair.**

### 5.6 Pairing states

A contact stays in state **pending** until the first valid packet from the peer has been verified. This catches half-completed pairings (A scanned B, but B never scanned A: the salts do not match and all tags fail).

### 5.7 Optional hardening

After pairing, display a short fingerprint (for example four emojis derived from both public keys) so users can compare them. This protects against a tampered QR code.

---

## 6. Encryption

**AES-128-CCM, tag length M = 8 bytes, nonce length 13 bytes (L = 2).**

| Reason | Detail |
|---|---|
| Standardised | RFC 3610. Used by 802.15.4, Zigbee and BLE (BLE even uses a 4-byte tag) |
| Hardware support | ESP32, nRF52, STM32WL have AES accelerators |
| Authenticated | Tampering with header or payload is detected |
| Tag size | 8 bytes gives a 2⁻⁶⁴ forgery probability per attempt. Airtime limits attackers to a few attempts per second. A 16-byte tag would add ca. 30 % airtime to a 25-byte packet |

If a target has no AES hardware, ChaCha20-Poly1305 is the drop-in alternative.

### Nonce (13 bytes, never transmitted)

```
Nonce = SRC (2) | DST (2) | CTR (3) | RETRY (1) | 0x00 × 5
```

It is rebuilt by the receiver from the header. Uniqueness is guaranteed because keys are per direction, counters are per contact and direction, and RETRY differs on every retransmission.

### Associated Data (AD)

```
AD = header with HOP_LEFT bits zeroed          (data packets)
AD = header with HOP_LEFT bits zeroed || orig_CTR (3 B)   (ACK packets)
```

**HOP_LEFT is zeroed because relays decrement it.** If it were authenticated, the tag would break after the first hop.

### Counter persistence

Each contact has one transmit-counter sequence per direction, shared by data and ACK packets. The first transmitted counter is 1; 0 is reserved. Every new packet consumes the next counter. A data retry reuses that data packet's counter and increments `RETRY`; sending an ACK again uses a fresh counter and `RETRY = 0`.

Persist an exclusive reservation end before using any counter in its block. For a next counter `n`, reserve `[n, min(n + 256, 2^24))`. After reboot, resume at the persisted reservation end and skip every unused value in the prior block. Never wrap: when the next counter reaches `2^24`, sending to that contact stops until the peers re-pair and derive new keys. This makes nonce reuse impossible after power loss, assuming the reservation commit succeeds before a counter is used.

---

## 7. Packet Format

```
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
┌─────────────────┬───────────────────────────────┬───────────────────────────────┐
│ Routing byte    │ DST (16 bit)                  │ SRC (16 bit)                  │
├─────────────────┴───────────────────────────────┴───────────────────────────────┤
│ CTR (24 bit)                                                                    │
├─────────────────────────────────────────────────────────────────────────────────┤
│ Encrypted: Inner header (1 B) + Body (n B)                                      │
├─────────────────────────────────────────────────────────────────────────────────┤
│ MIC (8 B)                                                                       │
└─────────────────────────────────────────────────────────────────────────────────┘
```

Flat layout:

| Offset | Size | Field | Authenticated | Encrypted | Mutable by relays |
|---|---|---|---|---|---|
| 0 | 1 | Routing byte | yes (HOP_LEFT masked) | no | **HOP_LEFT only** |
| 1 to 2 | 2 | DST | yes | no | no |
| 3 to 4 | 2 | SRC | yes | no | no |
| 5 to 7 | 3 | CTR | yes | no | no |
| 8 | 1 | Inner header (TYPE and FLAGS) | yes | **yes** | no |
| 9 to n | var | Body | yes | **yes** | no |
| last 8 | 8 | MIC | n/a | no | no |

Total overhead: **8 (header) + 1 (inner) + 8 (MIC) = 17 bytes**.

### Routing byte

```
 bit:  7 6 5 | 4 3 2 | 1 0
       HOP_START | HOP_LEFT | RETRY
```

| Field | Bits | Meaning |
|---|---|---|
| HOP_START | 3 | Hop limit set by the sender (0 to 7, default 3) |
| HOP_LEFT | 3 | Remaining hops, decremented by every relay |
| RETRY | 2 | 0 for first transmission, 1 to 3 for retransmissions |

`hops travelled = HOP_START − HOP_LEFT`

### Inner header (encrypted)

```
 bit:  7 6 5 4 | 3 2 1 0
       TYPE    | FLAGS
```

| TYPE | Name |
|---|---|
| 0x0 | TEXT |
| 0x1 | REACTION |
| 0x2 | GEO |
| 0x3 to 0xF | Reserved |

| Flag bit | Meaning |
|---|---|
| 3 | `ACK_REQ`: sender wants a receipt |
| 2 | TEXT: compressed (optional Unishox2). REACTION: `TARGET_OWN` |
| 1 | GEO: has altitude |
| 0 | GEO: has age of fix |

### How the receiver knows it is an Offgrid packet

1. The **LoRa sync word** filters at PHY level (0 bytes on air).
2. The **PHY CRC** discards noise.
3. The **AEAD tag** is the real validation. A magic number in the header would waste bytes and add no security.

The protocol version is implicit in the HKDF label `"og1"`. A different version simply fails tag verification.

### Why type and flags are encrypted

Relays do not need them. Keeping them inside the ciphertext reduces metadata leakage. (An ACK is still recognisable by its length of exactly 16 bytes.)

### Size guidance

Recommended total packet size is **at most about 100 bytes** (about 83 bytes of text body). The hard PHY limit is 255 bytes. Long texts are split by the app into several messages (real fragmentation is future work).

---

## 8. Message Types

### 8.1 TEXT (0x0)

```
Inner header | UTF-8 text
```

The length is implied by the packet length. Optional compression flag (Unishox2) saves roughly 30 to 50 % on short texts.

### 8.2 REACTION (0x1)

```
Inner header | ref (2 B) | emoji (1 B)
```

| Field | Description |
|---|---|
| `ref` | Lower 16 bits of the target message's CTR |
| `TARGET_OWN` flag | 1 if the target is a message written by the reacting user, 0 if written by the peer |
| `emoji` | Index into a fixed emoji table. `0x00` removes the reaction |

Together, `TARGET_OWN` and `ref` identify the target message within the conversation, because counters are per contact and direction. Semantics:

- One reaction per user per message, a new one replaces the old.
- The operation is idempotent, so duplicates are harmless.
- If the target message has not arrived yet, the app keeps the reaction briefly as an orphan and applies it if the message shows up.
- Ambiguity from the 16-bit truncation only occurs after more than 65 000 messages in one direction. The newest match wins.

### 8.3 GEO (0x2)

```
Inner header | lat (3 B) | lon (3 B) | [alt (2 B)] | [age (1 B)]
```

| Field | Encoding | Resolution |
|---|---|---|
| lat | `round((lat + 90) / 180 × 2²⁴)` | ca. 1.2 m |
| lon | `round((lon + 180) / 360 × 2²⁴)` | ca. 2.4 m at the equator |
| alt (optional) | signed int16, metres | 1 m |
| age (optional) | seconds since fix ÷ 10, 255 = 42.5 min or more | 10 s |

No timestamps are used anywhere in the protocol, because devices may have no clock. The receiver timestamps messages on arrival.

Firmware codec limits: reject nonfinite/out-of-range coordinates; clamp rounded
upper endpoints to `0xFFFFFF` (never wrap +90/+180 to zero). Age is rounded to
the nearest 10 seconds, capped at 2550 seconds. Reserved types/flags/emoji indices,
malformed lengths and invalid UTF-8/NUL text are rejected after authentication.
The firmware caps complete frames at 100 bytes and text bodies at 83 bytes;
optional compressed text returns an explicit unsupported result.

### 8.4 ACK (no inner header)

See [section 10](#10-receive-receipts-ack).

### Default ACK policy

| Type | Default `ACK_REQ` |
|---|---|
| TEXT | yes |
| REACTION | no (optional) |
| GEO | no for periodic beacons, yes for a one-off share |

---

## 9. Message IDs, Counters, Dedupe and Replay Protection

### 9.1 Message ID

> **Message ID = (SRC, DST, CTR)**

The counter is **per contact and per direction**, not one global counter per device. It is shared by all new encrypted packets in that direction, including ACKs; retransmissions of a data packet reuse its counter and are distinguished by `RETRY`.

**Why not global?** With a global counter, A could talk to other contacts in the meantime and push the counter far ahead. If an ACK to B got lost, B would later see the retry of an old CTR fall outside its replay window and silently drop it, so no ACK would ever be sent. With per-contact counters, the window only moves with traffic in that very conversation.

The first counter is 1; counter 0 is reserved. The CTR serves four purposes:

1. Part of the nonce
2. Duplicate identification
3. Replay protection and ordering
4. Reference for reactions and ACKs

It is 24 bits wide (16.7 million messages per direction). Re-pair before it wraps.

### 9.2 Dedupe in relays

Relays hold no keys and cannot verify tags. They identify duplicates with:

```
dedupe_key = Hash32( packet bytes with HOP_LEFT zeroed, including MIC )
```

- **Include the MIC.** If only (SRC, CTR) were used, an attacker could pre-send forged packets with predicted counters and make relays drop the genuine one later. A forgery has a different tag and therefore a different key.
- Firmware uses **HMAC-SHA256 truncated to 32 bits with a random 16-byte per-boot key**. A plain CRC32 can be collided deliberately.
- Storage: FIFO ring of 256 entries at 4 bytes each.
- Relays also **cancel a pending rebroadcast** if they overhear a neighbour sending the same packet.

### 9.3 Replay protection at the receiver

Per contact the receiver keeps `highest_ctr` and a **64-bit sliding window bitmap** (as in IPsec or DTLS).

| Incoming CTR | Action |
|---|---|
| greater than `highest_ctr` | accept, shift window (gaps of any size are fine) |
| inside the window, not yet seen | accept (reordering) |
| inside the window, already seen | duplicate: do not deliver again; **re-ACK if requested and RETRY > 0** |
| more than 63 below `highest_ctr` | stale: drop, even when the tag is valid or `RETRY > 0` |

The window state is updated **only after the tag has verified**.

Commit the accepted highest before every new delivery, including reordered
acceptance, rather than leaving an unsaved interval. After a reboot, treat
everything at or below the saved highest as already seen.
This prevents reboot re-delivery at the expense of more flash writes; endurance
and mixed-load timing must be accepted before release.

Per-contact counters prevent traffic to other contacts from advancing this window. Heavy traffic in the same direction can still move an unacknowledged packet outside it. Such a retry is rejected and the sender may eventually report `timeout`; this is the intentional security-first limit of the 64-counter replay window.

---

## 10. Receive Receipts (ACK)

### The idea

The ACK is a normal packet with **empty plaintext**. It consists only of the 8-byte header and the 8-byte MIC: **exactly 16 bytes**.

- DST = original sender, SRC = original receiver
- CTR = the ACK sender's own counter for that direction
- RETRY = 0
- AD includes the original message's CTR, which is **not transmitted**

```
ACK AD = header (HOP_LEFT zeroed) || orig_CTR (3 bytes)
```

### How the sender matches it

When a 16-byte packet arrives from a known contact, the sender tries each of its pending messages (at most about 8 to 16) as `orig_CTR`. Only one verifies. That message is confirmed.

| Property | Result |
|---|---|
| Extra bytes for the ID | **0** |
| Forgeable by relays? | No, requires k_B→A |
| Replayable? | No, replay window rejects it |
| Cost | A few AES operations |

If the pending list ever grows large, an optional 1-byte hint can be added.

### Hop limit for the ACK

```
ACK.HOP_START = min(7, (HOP_START − HOP_LEFT of received message) + 1)
```

So the ACK does not flood the entire network. **ACKs are never acknowledged.**

### When the ACK is sent

- Once per message, when it is delivered.
- Again if a duplicate with `RETRY > 0` arrives (the earlier ACK was probably lost).

---

## 11. Mesh Relaying

Offgrid uses **managed flooding**.

### 11.1 Receive algorithm

```
1. Check sync word, PHY CRC, packet length (at least 16 bytes), and routing fields.
2. If DST == own ID, try authentication with every contact whose NodeID matches SRC. On success, apply replay handling: deliver a new packet once; for an authenticated duplicate, do not deliver again and send a fresh ACK when the original requested one and `RETRY > 0`.
3. Check the relay dedupe cache using the packet with HOP_LEFT zeroed and the MIC included. A hit cancels any pending rebroadcast and suppresses forwarding. Local transmissions are inserted into this cache before radio launch, so this also suppresses self-echoes.
4. If DST == own ID and authentication failed for every candidate (or no contact matched), treat it as transit traffic. A 16-bit NodeID collision must not make a node swallow another node's packet.
5. If HOP_LEFT > 0, schedule rebroadcast with HOP_LEFT − 1.

Never drop a packet solely because `SRC == own ID`: NodeIDs can collide. Endpoint authentication/replay handling runs before relay dedupe so a duplicate addressed packet can trigger the required fresh ACK without being delivered twice.

The endpoint authentication step is:

```
if DST == own ID:
   for each contact whose NodeID == SRC: try to verify the tag
        success → replay check, deliver once, ACK duplicates as specified
```
```

Step 4 matters: a 16-bit NodeID can collide. A collision must never make a node swallow someone else's packet.

### 11.2 Rebroadcast timing

- **CAD** (listen-before-talk) before every transmission.
- Delay = random backoff + **SNR weighting**. The worse the SNR of the received packet (the farther away the sender), the earlier the node rebroadcasts. This maximises coverage per hop.
- Suppress the rebroadcast if a neighbour is overheard sending the same packet first.

### 11.3 Safety limits

| Rule | Purpose |
|---|---|
| Max `HOP_START` = 7, default 3 | Bound the flood |
| Drop if `HOP_LEFT > HOP_START` | Reject malformed packets |
| Rate limit per SRC | Limit junk flooding |
| Own messages and ACKs before relayed traffic | Priority |
| Rate limit on tag failures per SRC | Limit verify-DoS |
| Authenticated destination packets are not relayed | Save airtime without swallowing transit traffic on NodeID collisions |

Everyone relays everyone's packets, including those of strangers. This is also good for anonymity.

---

## 12. Reliability: Retries and Staying in Sync

### 12.1 Sender state machine

| State | Meaning |
|---|---|
| ⏳ queued | Waiting for channel |
| ✓ sent | Transmitted, or a neighbour was heard repeating it (implicit ACK for the first hop only) |
| ✓✓ delivered | End-to-end ACK verified |
| ✗ failed | No ACK after 3 retries |

### 12.2 Retransmission

- Same **CTR**, **RETRY** incremented (1, 2, 3); each retry starts with `HOP_LEFT = HOP_START`
- New nonce, new ciphertext, new MIC. Relays therefore do not treat it as a duplicate, while the receiver recognises the same CTR in its window.
- Timeout: `(2 × hops + 2) × (airtime + max_jitter) × 2^retry`
  Example at SF9 and 3 hops: about 6 s, then 12 s, then 24 s.
- A manual "send again" after ✗ uses a **new CTR** and may produce a duplicate. This is accepted.

### 12.3 Why devices do not get out of sync

There is no running shared state that has to be tracked packet by packet:

| Property | Effect |
|---|---|
| Static keys (v1) | Every packet is decryptable by itself |
| Nonce rebuilt from header | No dependence on previous packets |
| Window accepts any larger CTR | Lost packets only leave holes, never block |
| Retry reuses CTR | Receiver can recognise duplicates |
| ACK idempotent | Lost ACK is simply re-sent |

### 12.4 Situations that can desync and their remedies

| Situation | Consequence | Remedy |
|---|---|---|
| Sender loses its counter (flash wipe, old backup restored) | CTR lower than `highest` at receiver, everything looks like replay | **Re-pair.** New salt gives new keys, counters and windows reset on both sides |
| Receiver loses replay state (reboot) | Old packets could be accepted again | Persist `highest_ctr` periodically (9.3). Minor duplicate risk, fine for lab |
| Half-finished pairing | All tags fail | Contact stays `pending` until first valid packet |
| Contact deleted on one side only | One side drops everything, other sees ✗ | Re-pair. No automatic repair over the air |
| Reaction for a lost message | Target missing | Keep as orphan briefly, drop if the message never arrives |
| Gaps in CTR | Not necessarily lost messages, ACKs and reactions also consume counters | Do not show gaps in the UI |
| Original arrives after ✗ | Duplicate in chat | Accepted |

---

## 13. Known Gaps and How They Are Closed

| Problem | Solution |
|---|---|
| Nonce reuse after reset or power loss | Pairing salt (5.5) and flash reservation of counters (6) |
| Mutable HOP_LEFT would break the tag | HOP_LEFT zeroed in AD |
| Dedupe poisoning with predicted counters | MIC included in dedupe key, keyed hash (9.2) |
| 16-bit ID collisions | Trial verification, transit fallback (11.1) |
| Echo and loops | Locally originated packets enter the normalized dedupe cache before TX; hop limit |
| Retransmit swallowed by mesh | RETRY field changes MIC and dedupe key |
| Retry outside replay window | Drop as stale; same-contact traffic can advance the 64-counter window (9.3), so sender may time out |
| ACK forgery or mix-up | AD bound to original CTR, replay window |
| ACK storm | One ACK per message, small hop limit, no ACK for ACK |
| Flooding exhausts airtime | CAD, random backoff, bounded relay/source budgets and shared per-boot airtime pacing |
| No clock on device | No timestamps in protocol, receiver stamps arrival |
| Lost or stolen device | Delete contact and re-pair. Revocation over the air is impossible because the key is compromised |
| Flood of invalid tags | Rate limit per SRC |
| Half-finished pairing | `pending` state (5.6) |
| Tampered QR code | Optional fingerprint comparison (5.7) |

---

## 14. Limitations and Future Work

**Accepted limitations in v1**

| Limitation | Note |
|---|---|
| **Metadata visible** | SRC, DST, CTR and packet size are in the clear. An observer sees who talks to whom and can recognise ACKs by length. Rotating pseudonym tags could improve this |
| **No forward secrecy** | A stolen device or key exposes captured past traffic |
| **Offline recipient** | The message is lost after the retries. Store-and-forward on relays is future work |
| **No fragmentation** | Long texts must be split by the app |
| **Airtime accounting resets at boot** | Shared 1% pacing is not a durable rolling-hour ledger or regulatory certification |

**Possible extensions**

- **Hash ratchet** for coarse forward secrecy: `epoch = CTR >> 12`, `k_epoch = HKDF(k, epoch)`, delete old keys. No handshake needed and tolerant of packet loss.
- **Group chats** with shared group keys (DST = group ID, sender keys).
- **Read receipts** as an additional type.
- **Pseudonym tags** instead of static SRC/DST.
- **Store-and-forward** on relay nodes.
- **Fragmentation** with a fragment type.
- Other frequency plans (US915 and others).

---

## 15. Worked Example: Byte by Byte

> The ciphertext and MIC bytes below are **real AES-CCM output**, reproduced exactly by the on-device codec checks described in [Appendix A](#appendix-a-implementation-and-verification). These keys are test vectors only. **Never use them in production.**

### 15.1 Setup

| Item | Value |
|---|---|
| Alice private key | `SHA256("alice")` |
| Bob private key | `SHA256("bob")` |
| Alice public key | `2e8b2883c27b391359d7d0c15d00815a6693290a38dd8eba341cc07ddaa8ed4d` |
| Bob public key | `c0b126b5aa134b84e64625256b8de8973787612b1f3839d2c60526d2114e886d` |
| Alice NodeID | `E3 F0` |
| Bob NodeID | `E8 41` |
| Alice salt | `10 11 12 … 1F` |
| Bob salt | `20 21 22 … 2F` |
| k_A→B | `b6c3836e8bfc1d784b0688a5963e1497` |
| k_B→A | `869a272db66553bf737a40f9fb8929ca` |

Scenario: Alice and Bob are paired. A relay node (R) sits between them. `HOP_START = 3` for data packets.

### 15.2 Step 1: Alice sends the text "Hello!" (CTR 1)

**Plaintext before encryption**

| Bytes | Meaning |
|---|---|
| `08` | Inner header: TYPE = 0 (TEXT), FLAGS = `1000` (ACK_REQ) |
| `48 65 6C 6C 6F 21` | "Hello!" in UTF-8 |

**Packet on air (23 bytes)**

```
6C  E8 41  E3 F0  00 00 01  5D 1D 3F D9 2A 3E 04  DA 79 40 24 DB 76 AD BC
└┬┘ └──┬─┘ └──┬─┘ └───┬───┘ └──────────┬─────────┘ └──────────┬──────────┘
 │     │      │       │                │                      │
 │     │      │       │                │                      └ MIC (8 B)
 │     │      │       │                └ Ciphertext (7 B): inner header + "Hello!"
 │     │      │       └ CTR = 1
 │     │      └ SRC = Alice (E3 F0)
 │     └ DST = Bob (E8 41)
 └ Routing byte
```

**Routing byte `6C`**

```
0x6C = 0 1 1 | 0 1 1 | 0 0
       HOP_START=3 | HOP_LEFT=3 | RETRY=0
```

**Nonce (13 bytes)**

```
E3 F0 | E8 41 | 00 00 01 | 00 | 00 00 00 00 00
SRC     DST     CTR        RETRY  padding
```

**Associated Data (8 bytes)**

The AD is the 8-byte header with the HOP_LEFT bits zeroed:

```
60 E8 41 E3 F0 00 00 01
└┬┘
 └ 0x6C with HOP_LEFT bits zeroed = 0x60
```

### 15.3 Step 2: The relay forwards it

The relay does not know any keys. It only does three things:

1. Checks its dedupe cache (not seen before, so continue).
2. Decrements HOP_LEFT from 3 to 2.
3. Rebroadcasts after CAD and backoff.

```
Before: 6C  E8 41 E3 F0 00 00 01 ...
After:  68  E8 41 E3 F0 00 00 01 ...      (everything else identical)

0x68 = 0 1 1 | 0 1 0 | 0 0
       HOP_START=3 | HOP_LEFT=2 | RETRY=0
```

**The tag still verifies**, because HOP_LEFT is zeroed (`0x60`) in the AD for both versions. If the relay had altered any other byte, for instance SRC, verification would fail. The reference code confirms this (tamper test: detected).

### 15.4 Step 3: Bob receives and sends the ACK

Bob's receive logic:

1. DST `E8 41` is his own ID. SRC `E3 F0` matches Alice. He verifies the tag with k_A→B: **OK**.
2. CTR 1 is greater than `highest_ctr` (0): accept and deliver "Hello!".
3. `ACK_REQ` is set, so he sends an ACK.

Hops travelled: `HOP_START − HOP_LEFT = 3 − 2 = 1`, so `ACK.HOP_START = min(7, 1 + 1) = 2`.

**ACK packet (16 bytes)**

```
48  E3 F0  E8 41  00 00 01  7F 15 64 6B 08 38 4B 1D
└┬┘ └──┬─┘ └──┬─┘ └───┬───┘ └──────────┬──────────┘
 │     │      │       │                └ MIC (8 B), no ciphertext at all
 │     │      │       └ Bob's own CTR for Bob→Alice = 1
 │     │      └ SRC = Bob
 │     └ DST = Alice
 └ Routing byte
```

**Routing byte `48`**

```
0x48 = 0 1 0 | 0 1 0 | 0 0
       HOP_START=2 | HOP_LEFT=2 | RETRY=0
```

**Associated Data (11 bytes)**

```
40 E3 F0 E8 41 00 00 01   00 00 01
└──────── header ───────┘ └ orig_CTR = 1 (not transmitted!)
```

The ACK contains **no field saying which message it confirms**. The information lives only in the AD.

**Nonce**

```
E8 41 | E3 F0 | 00 00 01 | 00 | 00 00 00 00 00
```

(Bob is the sender, so SRC and DST are swapped compared to step 1.)

### 15.5 Step 4: Alice matches the ACK

Alice receives a 16-byte packet from Bob. Length 16 means ACK. She has two messages pending, CTR 7 and CTR 1 (7 is an invented second message to illustrate the trial):

| Candidate `orig_CTR` | Result |
|---|---|
| 7 | tag **fails** |
| 1 | tag **verifies**, message 1 is now ✓✓ |

### 15.6 Step 5: Bob reacts with 👍 (CTR 2 in direction Bob→Alice)

**Plaintext**

| Bytes | Meaning |
|---|---|
| `10` | TYPE = 1 (REACTION), FLAGS = `0000`: no ACK requested, `TARGET_OWN` = 0 (the target is Alice's message, not Bob's own) |
| `00 01` | `ref` = lower 16 bits of Alice's CTR = 1 |
| `01` | emoji index 1 (👍) |

**Packet (20 bytes)**

```
6C  E3 F0  E8 41  00 00 02  08 02 40 45  09 20 B2 0D 48 3B BE 72
└┬┘ └──┬─┘ └──┬─┘ └───┬───┘ └────┬─────┘ └──────────┬──────────┘
 │     │      │       │          │                   └ MIC (8 B)
 │     │      │       │          └ Ciphertext (4 B): 10 00 01 01
 │     │      │       └ CTR = 2 (Bob→Alice: his ACK used 1, this is 2)
 │     │      └ SRC = Bob
 │     └ DST = Alice
 └ Routing byte (3 / 3 / 0)
```

Alice decrypts to `10 00 01 01` and applies 👍 to her message CTR 1.

### 15.7 Step 6: Alice shares her location (CTR 2 in direction Alice→Bob)

Location: Lucerne, 47.0517° N, 8.3075° E.

```
lat24 = round((47.0517 + 90) / 180 × 2²⁴) = 12774144 = 0xC2EB00
lon24 = round((8.3075 + 180) / 360 × 2²⁴) = 8775766  = 0x85E856
```

**Plaintext**

| Bytes | Meaning |
|---|---|
| `20` | TYPE = 2 (GEO), FLAGS = `0000`: no altitude, no age, no ACK |
| `C2 EB 00` | latitude |
| `85 E8 56` | longitude |

**Packet (23 bytes)**

```
6C  E8 41  E3 F0  00 00 02  FD 27 70 16 A3 AA 4E  6F AD D2 E9 AB C5 D9 93
└┬┘ └──┬─┘ └──┬─┘ └───┬───┘ └──────────┬─────────┘ └──────────┬──────────┘
 │     │      │       │                └ Ciphertext (7 B)     └ MIC (8 B)
 │     │      │       └ CTR = 2 (Alice→Bob; her text used 1)
 │     │      └ SRC = Alice
 │     └ DST = Bob
 └ Routing byte
```

Bob decodes the position as 47.05170° N, 8.30751° E (about 1 to 2 m rounding error).

### 15.8 Size summary

| Packet | Total bytes | Header | Inner | Payload | MIC |
|---|---|---|---|---|---|
| TEXT "Hello!" | 23 | 8 | 1 | 6 | 8 |
| ACK | 16 | 8 | 0 | 0 | 8 |
| REACTION | 20 | 8 | 1 | 3 | 8 |
| GEO (no alt/age) | 23 | 8 | 1 | 6 | 8 |

### 15.9 What a passive observer sees

| Visible | Hidden |
|---|---|
| That `E3 F0` talks to `E8 41` | The message type (text, reaction, geo) |
| Message counters | The content |
| Packet sizes (a 16-byte packet is an ACK) | Whether an ACK was requested |
| Hop count and retry count | Which message an ACK confirms |

---

## Appendix A: Implementation and Verification

The maintained codec is [PacketCodec.cpp](../src/mesh/PacketCodec.cpp). Its
isolated self-test reproduces all four encrypted vectors in section 15, tests
mutable-hop authentication, wrong keys/tags, ACK counter binding, UTF-8 bounds,
reactions and GEO quantization. It is compiled only into explicit lab profiles:

```sh
pio run -e radio_diag -t upload --upload-port /dev/ttyUSB0
python3 tools/serial-check.py --port /dev/ttyUSB0 --command diag.codec.selftest --allow-secure-changes
```

Restore normal secure firmware after diagnostics. The self-test uses published
fixture keys, not live identity/contact keys, and does not transmit. Passing it
verifies codec behavior, not physical multi-hop forwarding. The former incomplete
Python development sketch has been removed rather than offered as runnable code.

---

## Appendix B: Constants

### Emoji table

| Index | Emoji |
|---|---|
| `0x00` | remove reaction |
| `0x01` | 👍 |
| `0x02` | ❤️ |
| `0x03` | 😂 |
| `0x04` | 😮 |
| `0x05` | 😢 |
| `0x06` | 🙏 |
| `0x07` to `0xFF` | reserved |

### Protocol constants

| Constant | Value |
|---|---|
| HKDF label | `"og1"` |
| QR magic | `"OG1"` |
| AES-CCM tag length | 8 bytes |
| AES-CCM nonce length | 13 bytes |
| Key length | 16 bytes |
| Default `HOP_START` | 3 |
| Max `HOP_START` | 7 |
| Max retries | 3 |
| Replay window | 64 |
| Counter reservation block | 256 |
| `highest_ctr` persistence interval | Every newly accepted packet, before delivery |
| Dedupe cache | 256 entries |
| ACK length | 16 bytes |
| Maximum complete packet | 100 bytes |
