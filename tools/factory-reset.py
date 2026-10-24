#!/usr/bin/env python3
"""Erase an OffGrid device using its owner's master reset key, without an app token."""

import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(
        description="Erase an OffGrid identity, contacts and settings, then reboot unprovisioned.",
        epilog="Requires responsive secure firmware trusting the master key. Firmware is preserved.",
    )
    parser.add_argument("--port", required=True, help="serial port, e.g. /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--master-key", type=Path,
                        default=Path.home() / ".config/offgrid/master-reset.pem")
    parser.add_argument("--confirm-erase", action="store_true", help="confirm permanent data loss")
    args = parser.parse_args()
    if not args.confirm_erase:
        parser.error("permanently erases device data; pass --confirm-erase to proceed")
    return subprocess.call([
        sys.executable, str(Path(__file__).resolve().with_name("serial-check.py")),
        "--port", args.port, "--baud", str(args.baud),
        "--master-key", str(args.master_key), "--master-reset", "--confirm-erase",
    ])


if __name__ == "__main__":
    sys.exit(main())
