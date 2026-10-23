# Radio Driver

`RadioDriver` wraps pinned RadioLib 7.1.2 for the Heltec WiFi LoRa 32 V3 SX1262.
Pins and the 1.8 V DIO3 TCXO supply are defined in `BoardConfig.h`; verify against
your board revision. DIO2 controls the RF switch. LoRa uses explicit headers,
PHY CRC and automatic low-data-rate optimization. The sync word is expanded by
RadioLib with control bits 0x44, so 0x4F becomes 0x44F4.

The default serial mock profiles do not initialize radio hardware. Build
`pio run -e radio_diag` for real hardware diagnostics. Stage 1 initializes from
saved settings and stays in standby; `sys.status.radio` reports initialized,
state and the last RadioLib error. RX/TX and CAD follow in separate commits.

Initialization/configuration calls can wait inside RadioLib. No strict
non-blocking timing guarantee or real-device acceptance is claimed by a build.
Keep antennas connected and use lawful frequencies/power when RF is enabled.