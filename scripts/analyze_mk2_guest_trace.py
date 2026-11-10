#!/usr/bin/env python3
"""Find guest execution-context candidates in MK2's opt-in _guest.csv.

This does NOT measure simulation ticks. An IRQ event's CS:EIP is interrupted
guest execution (not the ISR entry). 3DAh and VRAM samples are biased toward
polling/drawing code, and dynamic translation can expose stale EIP state.

Usage:
  python scripts/analyze_mk2_guest_trace.py MK2Timing/mk2_40000_x_guest.csv
  python scripts/analyze_mk2_guest_trace.py ... --top 20 --page-size 256
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path
from statistics import median

EVENT_NAMES = (
    "irq0_pre",
    "vram_first_write_ms",
    "vga_status_rising",
    "vga_status_sample",
    "clock_10ms",
)


def fmt_address(value: int) -> str:
    return f"{value:08X}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="DOSBOX_MK2_TRACE _guest.csv")
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--page-size", type=int, default=256,
                        help="Group neighbouring EIP offsets into this many bytes")
    args = parser.parse_args()
    if args.page_size <= 0 or (args.page_size & (args.page_size - 1)):
        parser.error("--page-size must be a positive power of two")
    if args.top < 1:
        parser.error("--top must be positive")

    counts: dict[str, Counter[tuple[int, int, int]]] = defaultdict(Counter)
    times: dict[str, dict[tuple[int, int, int], list[float]]] = defaultdict(
        lambda: defaultdict(list)
    )
    exact: dict[str, Counter[tuple[int, int, int]]] = defaultdict(Counter)
    modes: Counter[int] = Counter()
    total = 0
    with args.trace.open(newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            kind = row["event"]
            cs = int(row["cs"])
            eip = int(row["eip"])
            base = int(row["cs_base"])
            mode = int(row["protected_mode"])
            # Keep both selector and base: DOS extenders can change descriptors.
            page_key = (cs, base, eip & ~(args.page_size - 1))
            exact_key = (cs, base, eip)
            counts[kind][page_key] += 1
            exact[kind][exact_key] += 1
            times[kind][page_key].append(float(row["emulated_ms"]))
            modes[mode] += 1
            total += 1

    print(f"Input: {args.trace.name}")
    print(f"Total context samples: {total:,}")
    print(f"Real/protected mode samples: {modes.get(0, 0):,}/{modes.get(1, 0):,}")
    print(f"Grouping address windows: {args.page_size} guest EIP bytes")
    for kind in EVENT_NAMES:
        count = sum(counts[kind].values())
        print(f"\n[{kind}] {count:,} samples")
        if not count:
            continue
        print("  #   count  share  CS:page EIP      CS base   time span / median spacing (ms)")
        for index, (key, hits) in enumerate(counts[kind].most_common(args.top), 1):
            cs, base, page = key
            t = times[kind][key]
            gaps = [b - a for a, b in zip(t, t[1:]) if b > a]
            span = t[-1] - t[0] if len(t) > 1 else 0
            typical = f"{median(gaps):.3f}" if gaps else "n/a"
            print(f"{index:>3} {hits:>7} {hits/count:>6.1%}  "
                  f"{cs:04X}:{fmt_address(page)}  {fmt_address(base)}"
                  f"  {span:>9.2f} / {typical}")

        print("  Exact CS:EIP exemplars:")
        for key, hits in exact[kind].most_common(min(5, args.top)):
            cs, base, eip = key
            print(f"    {cs:04X}:{fmt_address(eip)} base={fmt_address(base)}, "
                  f"{hits:,} samples")

    print("\nINTERPRETATION")
    print("- Frequent VGA status PCs likely indicate wait loops, not logic updates.")
    print("- First VRAM write PC likely indicates rendering or a memcpy routine.")
    print("- IRQ0 pre-dispatch PCs show interrupted code, not ISR entry.")
    print("- Clock_10ms PCs are coarse phase samples, not actual instruction counts.")
    print("- CS:EIP may be stale/approximate under dynamic recompilation.")
    print("- Confirm candidate addresses with disassembly or a targeted guest hook")
    print("  before calling any recurrence a simulation or animation tick.")


if __name__ == "__main__":
    main()
