#!/usr/bin/env python3
"""Extract *local* MK2 guest code snippets from the opt-in _guest_code.csv.

Reads the user's own guest memory snapshots and saves only tiny .bin files
for local disassembly. No game code or assets are committed by this script.

Example:
    python scripts/extract_mk2_guest_code.py \
        C:/Users/Usuario/Documents/MK2Timing/mk2_normal_40000_TIME_guest_code.csv

Then, if NASM/ndisasm is installed:
    ndisasm -b 32 -o 0x205489 snapshots/irq_wait_loop_00205489.bin

Or with GNU objdump:
    objdump -D -b binary -m i386 -M intel --adjust-vma=0x205489 \
        snapshots/irq_wait_loop_00205489.bin
"""
from __future__ import annotations

import argparse
import csv
from pathlib import Path
import re


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Default: <csv stem>_snapshots next to CSV")
    args = parser.parse_args()

    target = args.output_dir or (
        args.csv_file.parent / (args.csv_file.stem + "_snapshots")
    )
    target.mkdir(parents=True, exist_ok=True)

    saved = 0
    with args.csv_file.open(newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            label = row["site"]
            # Avoid treating externally edited CSV text as a path component.
            safe_label = re.sub(r"[^A-Za-z0-9_-]", "_", label)
            start = int(row["snapshot_start"])
            hexdigits = row["bytes_hex"].strip()
            data = bytes.fromhex(hexdigits)
            if not data or len(data) > 256:
                raise ValueError(f"Invalid code snapshot size for {label}")
            output = target / f"{safe_label}_{start:08X}.bin"
            output.write_bytes(data)
            saved += 1
            print(f"{output}  ({len(data)} bytes, start guest EIP 0x{start:08X})")
            print(f"  ndisasm -b 32 -o 0x{start:X} \"{output}\"")
            print(f"  objdump -D -b binary -m i386 -M intel "
                  f"--adjust-vma=0x{start:X} \"{output}\"")

    print(f"Extracted {saved} local snapshots.")
    if not saved:
        print("No matched guest code sites were captured. Use core=normal "
              "and ensure that live combat overlaps 30-150 s emulated.")


if __name__ == "__main__":
    main()
