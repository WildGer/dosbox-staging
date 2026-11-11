#!/usr/bin/env python3
"""Analyze MK2 object 32-bit position ADD stores, including fractional-only updates.

Source: *_object_steps.csv from the research fork, emitted when verified
guest instructions 0180:002315B1 / 0180:002315B7 write an entire DWORD.

These are candidate fixed-point position accumulators (+0x18/+0x1C).
The observed +0x1A/+0x1E WORDs are the upper halves of these DWORDs.
Analyze full 32-bit stores to distinguish a movement integration rate
from *conditional* integer-coordinate changes. They are not equivalent
to the full combat simulation rate.

Usage:
    python scripts/analyze_mk2_object_steps.py C:/.../_object_steps.csv
"""
from __future__ import annotations

from collections import Counter, defaultdict
from pathlib import Path
from statistics import median
import argparse
import csv


def spacing(times: list[float]) -> str:
    differences = [b - a for a, b in zip(times, times[1:]) if b > a]
    if not differences:
        return "n/a"
    return (
        f"median={median(differences):.3f} ms, "
        f"min={min(differences):.3f}, max={max(differences):.3f}"
    )


def signed32(value: int) -> int:
    return value - 0x100000000 if value >= 0x80000000 else value


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv_file", type=Path)
    p.add_argument("--top", type=int, default=15)
    args = p.parse_args()

    all_rows = 0
    delta_mismatches = 0
    event_times: dict[tuple[str, int, int], list[float]] = defaultdict(list)
    counts = Counter()
    integer_changes = Counter()
    fractional_only = Counter()
    stationary = Counter()
    velocity = Counter()
    ebp_matches = Counter()
    cs = Counter()

    with args.csv_file.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        required = {
            "emulated_ms", "object", "object_pointer", "position_offset",
            "old_raw32", "new_raw32", "expected_delta32", "actual_delta32",
            "delta_matches", "old_integer", "new_integer", "integer_changed",
            "writer_eip", "writer_ebp", "object_linear", "writer_cs",
        }
        if not required.issubset(reader.fieldnames or []):
            p.error("Missing _object_steps.csv columns. Rebuild the new probe.")
        for row in reader:
            all_rows += 1
            label = row["object"]
            if label not in ("A", "B"):
                continue
            ptr = int(row["object_pointer"])
            off = int(row["position_offset"])
            key = (label, ptr, off)
            counts[key] += 1
            event_times[key].append(float(row["emulated_ms"]))
            old = int(row["old_raw32"])
            new = int(row["new_raw32"])
            velocity_dword = int(row["expected_delta32"])
            actual = int(row["actual_delta32"])
            if ((new - old) & 0xFFFFFFFF) != actual or (
                    actual != velocity_dword or int(row["delta_matches"]) != 1):
                delta_mismatches += 1
            if int(row["integer_changed"]) == 1:
                integer_changes[key] += 1
            elif old != new:
                fractional_only[key] += 1
            else:
                stationary[key] += 1
            velocity[(label, off, signed32(velocity_dword))] += 1
            ebp_matches[int(row["writer_ebp"]) == int(row["object_linear"])] += 1
            cs[(int(row["writer_cs"]), int(row["writer_eip"]))] += 1

    print(f"Input: {args.csv_file.name}")
    print(f"Observed full-DWORD integration stores: {all_rows:,}")
    print(f"ADD delta/EBX mismatches: {delta_mismatches:,}")
    print(f"EBP equals selected object linear pointer: "
          f"{ebp_matches[True]:,} yes; {ebp_matches[False]:,} no")
    print("Writer CS:EIP counts:")
    for (segment, eip), n in cs.most_common():
        print(f"  {segment:04X}:{eip:08X}: {n:,}")
    print("\nObject + 32-bit position-offset groups:")
    for key, n in counts.most_common(args.top):
        label, ptr, off = key
        print(f"  {label} ptr=0x{ptr:08X} offset=0x{off:02X} "
              f"stores={n:,}: {spacing(event_times[key])}")
        print(f"    Integer-high-word changed: {integer_changes[key]:,}; "
              f"fractional-only: {fractional_only[key]:,}; "
              f"unchanged DWORD/zero delta: {stationary[key]:,}")
    print("\nMost frequent signed 32-bit increments (raw fixed-point units):")
    for (label, off, v), n in velocity.most_common(args.top):
        print(f"  {label} +0x{off:02X}: increment {v:+d} "
              f"({v / 65536.0:+.6f} if 16.16): {n:,}")
    print("\nINTERPRETATION")
    print("- The x86 writer instruction is ADD DWORD [EBP+0x18/+0x1C], EBX. "
          "Raw after-before modulo 2**32 should equal EBX. "
          "Mismatches indicate instrumentation or aliasing problems.")
    print("- Position update/ADD frequency can be measured even when "
          "the upper WORD (integer candidate) does not change.")
    print("- 16.16 is an interpretation supported by consuming the "
          "upper words; inspect actual increments and game behavior "
          "before assigning spatial units or player identity.")
    print("- The watched object pointers refresh only at scheduled "
          "dispatch boundaries; other entities and transient pointer "
          "replacements can be missed.")
    print("- Full combat simulation frequency remains a separate "
          "question; this measures these MOVEMENT instructions only.")


if __name__ == "__main__":
    main()
