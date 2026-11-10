#!/usr/bin/env python3
"""Analyze the MK2 work-ready memory producer/consumer watchpoint.

The source is an opt-in diagnostic CSV captured from the user's own game
at DS:[uint32[DS:0x27177C]+0x24]. Neither a write nor a clear is
automatically an MK2 simulation tick.

Usage:
    python scripts/analyze_mk2_signal_trace.py path/to/trace_signal.csv
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path
from statistics import median


def stat(values: list[float]) -> str:
    if not values:
        return "n/a"
    return f"median={median(values):.3f} ms; min={min(values):.3f}; max={max(values):.3f}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("file", type=Path)
    parser.add_argument("--top", type=int, default=15)
    args = parser.parse_args()
    with args.file.open(newline="", encoding="utf-8") as fh:
        events = list(csv.DictReader(fh))
    print(f"File: {args.file.name}")
    print(f"Rows: {len(events):,}")
    if not events:
        print("No watch events; check env DOSBOX_MK2_SIGNAL_WATCH=1, "
              "core=normal, gameplay in emulated seconds 30-150, "
              "and the consumer 0180:00205489. This does not prove no writes.")
        return

    by_type: Counter[str] = Counter()
    locations = Counter()
    pointers = Counter()
    targets = Counter()
    writer_changes: dict[tuple[int, int, int], list[float]] = defaultdict(list)
    rising = []
    clearing = []
    for row in events:
        event = row["event"]
        by_type[event] += 1
        ptr = int(row["pointer_value"])
        target = int(row["target_linear"])
        pointers[ptr] += 1
        targets[target] += 1
        old, new = int(row["before"]), int(row["after"])
        if event.startswith("signal"):
            pc = (int(row["writer_cs"]), int(row["writer_cs_base"]),
                  int(row["writer_eip"]))
            locations[(event, *pc)] += 1
            if old == 0 and new != 0:
                rising.append(float(row["emulated_ms"]))
                writer_changes[pc].append(float(row["emulated_ms"]))
            elif old != 0 and new == 0:
                clearing.append(float(row["emulated_ms"]))

    print("\nEvent counts:")
    for event, count in by_type.most_common():
        print(f"  {event:<20} {count:>8,}")
    print("\nObserved pointer values and watched effective linear addresses:")
    for val, cnt in pointers.most_common(5):
        print(f"  pointer=0x{val:08X}: {cnt:,} rows")
    for val, cnt in targets.most_common(5):
        print(f"  target=0x{val:08X}: {cnt:,} rows")
    print("\nMost frequent source PCs for writes to the watched DWORD:")
    for (event, cs, cs_base, eip), hits in locations.most_common(args.top):
        print(f"  {event:<15} CS={cs:04X} base=0x{cs_base:08X} "
              f"EIP=0x{eip:08X}  hits={hits:,}")
    def diffs(times: list[float]) -> list[float]:
        return [t2 - t1 for t1, t2 in zip(times, times[1:]) if t2 > t1]
    print(f"\nObserved 0 -> nonzero signal transitions: {len(rising):,}; "
          f"spacing {stat(diffs(rising))}")
    print(f"Observed nonzero -> 0 clears: {len(clearing):,}; "
          f"spacing {stat(diffs(clearing))}")
    print("\nWriter PCs producing 0 -> nonzero transitions:")
    for (cs, cs_base, eip), t in sorted(writer_changes.items(),
                                        key=lambda item: -len(item[1]))[:args.top]:
        print(f"  {cs:04X}:{eip:08X} base=0x{cs_base:08X}: "
              f"{len(t):,} transitions; {stat(diffs(t))}")

    print("\nCAVEATS")
    print("- Event timestamps are guest emulated milliseconds.")
    print("- The watch resolves DS at the interrupted consumer; different "
          "DS values, pointer changes and writes outside normal core "
          "may be missed.")
    print("- Only monitored normal-core memory write helpers are covered. "
          "This is not an instruction-by-instruction trace or exhaustive "
          "watchpoint for dynamic/direct native writes.")
    print("- A 0->nonzero write can be a work/clock signal; it is NOT "
          "proof that a combat simulation tick occurred.")
    print("- Verify any candidate producer EIP with disassembly and a "
          "real gameplay state transition before interpreting frequency.")


if __name__ == "__main__":
    main()
