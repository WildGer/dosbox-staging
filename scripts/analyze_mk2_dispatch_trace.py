#!/usr/bin/env python3
"""Analyze MK2's normal-core task callback dispatches at 0180:00205531.

The opt-in _dispatch.csv captures the *destination* of each indirect
JMP DWORD PTR [ESI+0x1C], without assuming that every callback advances
the game's fighter simulation. All times are guest emulated ms.

Usage:
    python scripts/analyze_mk2_dispatch_trace.py path/to/_dispatch.csv
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
from pathlib import Path
from statistics import median


def intervals(times: list[float]) -> str:
    if len(times) < 2:
        return "n/a"
    differences = [b - a for a, b in zip(times, times[1:]) if b > a]
    if not differences:
        return "n/a"
    return (
        f"median={median(differences):.3f} ms; "
        f"min={min(differences):.3f}; "
        f"max={max(differences):.3f}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("--top", type=int, default=25)
    args = parser.parse_args()

    hits_by_target: dict[int, list[float]] = defaultdict(list)
    hits_by_task: dict[int, list[float]] = defaultdict(list)
    task_by_target: dict[int, set[int]] = defaultdict(set)
    remaining_values: dict[int, Counter[int]] = defaultdict(Counter)
    counts: Counter[str] = Counter()
    overflow_drops = 0
    all_times: list[float] = []

    with args.csv_file.open(encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle):
            event = row["event"]
            counts[event] += 1
            if event == "overflow":
                overflow_drops += int(row["dropped_count"])
                continue
            if event != "dispatch":
                continue
            when = float(row["emulated_ms"])
            target = int(row["handler_eip"])
            task = int(row["task_offset"])
            remaining = int(row["remaining"])
            hits_by_target[target].append(when)
            hits_by_task[task].append(when)
            task_by_target[target].add(task)
            remaining_values[target][remaining] += 1
            all_times.append(when)

    print(f"Input: {args.csv_file.name}")
    print(f"Dispatches: {counts['dispatch']:,}")
    print(f"Distinct task nodes: {len(hits_by_task):,}")
    print(f"Distinct callback target EIPs: {len(hits_by_target):,}")
    print(f"Dropped dispatches due to buffer overflow: {overflow_drops:,}")
    if all_times:
        print(f"Observed emulated time span: "
              f"{min(all_times):.3f} - {max(all_times):.3f} ms")
    if overflow_drops:
        print("WARNING: buffer overflow means callback counts are "
              "lower bounds, not complete.")

    print("\nCallback targets, ordered by observed call count:")
    for target, times in sorted(
            hits_by_target.items(), key=lambda kv: -len(kv[1])
            )[:args.top]:
        typical = remaining_values[target].most_common(3)
        print(f"  CS=0180 target EIP=0x{target:08X} "
              f"count={len(times):,}; tasks={len(task_by_target[target])}; "
              f"spacing {intervals(times)}")
        print(f"    Most frequent countdown-at-dispatch values: {typical}")

    print("\nTask nodes, ordered by observed dispatch count:")
    for task, times in sorted(
            hits_by_task.items(), key=lambda kv: -len(kv[1])
            )[:args.top]:
        print(f"  DS-relative task ESI=0x{task:08X} "
              f"count={len(times):,}; spacing {intervals(times)}")

    print("\nINTERPRETATION LIMITS")
    print("- The captured instruction is a near indirect JMP "
          "from 0180:00205531; its target retains CS 0180, "
          "whose base was zero in earlier traces.")
    print("- Repeated callbacks may be audio, input, animation, "
          "rendering, timers, or combat logic; no specific role "
          "can be inferred from counts alone.")
    print("- Countdown values are read after the scheduler already "
          "decremented the task field.")
    print("- Identify actual game-state transitions and disassemble "
          "candidate callback targets before calling one a "
          "simulation tick.")
    print("- The instrumentation is for core=normal only; "
          "host performance metrics are not comparable with core=dynamic.")


if __name__ == "__main__":
    main()
