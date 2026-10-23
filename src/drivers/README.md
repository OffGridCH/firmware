# Radio Driver

`RadioDriver` wraps pinned RadioLib 7.8.1 for the Heltec WiFi LoRa 32 V3 SX1262.
Pins and the 1.8 V DIO3 TCXO supply are defined in `BoardConfig.h`; verify against
your board revision. DIO2 controls the RF switch. LoRa uses explicit headers,
PHY CRC and automatic low-data-rate optimization. The sync word is expanded by
RadioLib with control bits 0x44, so 0x4F becomes 0x44F4.

The default serial mock profiles do not initialize radio hardware. Build
`pio run -e radio_diag` for real hardware diagnostics. Initialization uses saved
settings and stays in standby; `sys.status.radio` reports initialized, state and
the last RadioLib error. Diagnostic serial controls follow with integration.

`startReceive`, `startTransmit`, `tick`, and `popEvent` implement asynchronous
operations. DIO1 only sets an instance-owned flag under an ISR-safe critical
section; the application loop reads SPI and handles packets. TX/RX buffers hold
up to 255 bytes. Four event slots reserve one slot from RX traffic for operation
completion; excess received events are counted as drops. The caller must drain
events promptly. TX success is reported only after TxDone, and RX is restored
when requested. CAD scans and randomized backoff gate sends by default. Access
has a ten-second deadline, each CAD has a symbol-based deadline, and TX has both
a hardware timeout and a loop watchdog. Backoff is standby, not continuous RX;
packets during these half-duplex operation windows may be missed.

The driver uses RadioLib's staged packet setup, then issues the three-byte
SX1262 SetTx command through its bounded Module SPI interface. It deliberately
does not use startTransmit/launchMode: those convenience paths contain an
unbounded BUSY wait, including in 7.8.1. DIO2 performs this board's RF switching.
Runtime SPI/BUSY waits are capped at 20 ms per library transaction; a multi-command
operation can take longer. Initialization/recovery can take substantially longer
and must not be treated as normal real-time ticks.

`queueConfig` retains the latest requested settings until RX/standby is safe;
TX/CAD/backoff finish first. Configured or Failed events report the outcome.
Hardware failures stop automatic progress. `recover` explicitly reinitializes
the pending or last working settings and returns to standby. It never loops
through repeated reinitialization on every tick.

Initialization/configuration calls can wait inside RadioLib. No strict
non-blocking timing guarantee or real-device acceptance is claimed by a build.
Keep antennas connected and use lawful frequencies/power when RF is enabled.