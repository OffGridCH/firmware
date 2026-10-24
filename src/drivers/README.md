# Hardware drivers

Drivers own typed hardware operations, not JSON, credentials, storage or mesh
policy. App owns and connects them to the secure domains.

## OLED

U8g2 2.36.15 drives SSD1306 at 400 kHz I2C with a 20-ms Wire timeout. Initialization
contains bounded library waits and a 100-ms Vext delay. Rendering updates at most
every 40 ms, pushing one changed 128-byte tile row per tick. It does not run an
animation in a blocking loop.

Power-on plays the OLED intro once, synchronized to blue strip ripples/fill using
shared timing landmarks and a 6600-ms timeline. Unlock/relock interrupts it.
After the intro, locked firmware shows a large closed padlock and "Locked", never empty Home
placeholders/private identity details. Real unlock opens the shackle over 550 ms,
holds briefly, then shrinks/moves the open lock into Home's corner by 1400 ms.
Relock cancels the transition and pending old frame rows. Locked state overrides
error overlays. Authorized reset shows "Factory reset" during the red LED fade.
`sys.status.ui.oled_scene` exposes the last rendered scene.
Pairing/fingerprint/error/activity views return automatically; no button is used.

Both physical boards passed locked/unlocking/Home scene checks and <250-ms host
RTT during transitions. This is not optical acceptance; owner inspection of the
appearance remains necessary.

## LED outputs

GPIO35 uses 5-kHz, 8-bit LEDC PWM. External GPIO45/46 each drive four owner-confirmed
GRB 800-kHz NeoPixels through pinned Adafruit NeoPixel 1.12.5. Both external pins
are strapping pins; external circuitry must not force wrong boot levels.

Both strips share four RGB pixel values; onboard PWM mirrors their brightest
channel. Defaults are a 5-second blue breathing glow while locked, white 1600-ms
directional send/receive waves, purple breathing while pairing, and a red
1800-ms authorized reset bloom (2000 ms for errors). Raw RF reception and ACK
events do not interrupt the default theme. Unlocked idle is off.

Smooth patterns use gamma-shaped envelopes and 250-ms crossfades; repeated
same-kind events coalesce rather than restarting/flickering. State timing
continues beneath finite previews/events. Quiet/disabled forces all LEDs off,
including startup/reset. Legacy white step patterns and custom steps retain
their explicit timing; finite on/off cycles are 1000 ms, pulse is 1500 ms.
Native RMT `show()` is synchronous, approximately 120 us of
data per four-pixel strip plus driver overhead, not falsely called asynchronous.
Output changes are limited to 100 Hz and skipped when unchanged.

The real authenticated LED checker verifies RGB channels, spatial animation,
continuity, switches, brightness, pairing and preview expiry, then restores
settings and locks. Lab-only `diag.led.selftest` checks envelopes at 10-ms
intervals, direction, colors and exact expiry. `led_duty`, `strip_rgb` and
`led_frame_ms` are software-level
evidence, not an optical/electrical measurement.

## SX1262 radio

Pinned RadioLib 7.8.1 uses SPI/SX1262 pins and 1.8-V DIO3 TCXO supply from
`BoardConfig.h`; DIO2 controls RF switching. LoRa explicit headers, PHY CRC and
automatic low-data-rate optimization are enabled. Sync word 0x4F expands with
control bits 0x44 to 0x44F4.

`startReceive`, `startTransmit`, `tick`, `popEvent` expose asynchronous operations.
DIO1 sets only an instance-owned ISR flag; the cooperative loop reads SPI and
handles ciphertext. Buffers hold at most 255 PHY bytes. Four event slots reserve
one for completion; loss is counted. TX success means actual TxDone, never queue
acceptance; requested receive resumes after TX. Interrupted/failed airtime is
counted too. Mesh frames are capped at 100 bytes.

CAD and random backoff gate sends by default. Access has a ten-second deadline;
CAD and TX have watchdog deadlines. Backoff is standby rather than continuous
receive: packets can be missed during half-duplex access windows.

The driver stages packet setup then issues bounded SX1262 SetTx, avoiding the
unbounded BUSY wait in the library convenience launch path. Runtime SPI/BUSY
waits are capped at 20 ms per transaction; multiple transactions can take longer.
Initialization/recovery and flash operations can stall and are not strict
real-time guarantees.

`queueConfig` applies the latest settings after current RF work. Hardware errors
stop progress rather than looping through reinitialization. A fresh real
lock/unlock explicitly reinitializes unavailable hardware.

App routes real RX/TX/error events to MeshEngine in every profile. Locked keyless
receive/relay is opt-in; unprovisioned firmware does not initialize RF. Mesh
enforces shared airtime pacing after TX: 9 x airtime when the channel lies inside 869.40–869.65 MHz
(10 %), 99 x elsewhere (1 %). This is a per-boot safeguard,
not a durable legal rolling-hour ledger across resets.

There is no raw-radio simulator backend. Explicit lab profiles add secure
fixtures, ciphertext trace and receipt-loss injection described in
[host documentation](../host/README.md). Two boards passed real encrypted
bidirectional traffic and lost-ACK retries; three/four-board relay acceptance
remains pending.
