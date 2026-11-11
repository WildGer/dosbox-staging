#!/usr/bin/env python3
"""Analyze the two dynamically pointed-to object fields sampled at MK2
scheduler dispatch boundaries in a new _dispatch.csv.

These are OBJECT CANDIDATES, NOT verified player/fighter structs.
The read-only fields are +0x1A and +0x1E. The snapshot occurs
BEFORE a callback dispatch. A changed value between samples is
not necessarily caused by the preceding callback: unrelated guest
code, asynchronous events, and pointer replacement can intervene.

Usage: python scripts/analyze_mk2_object_trace.py path/to/_dispatch.csv
"""
from __future__ import annotations

import argparse
from collections import Counter
import csv
from pathlib import Path
from statistics import median


def summarize_intervals(times: list[float]) -> str:
    diffs = [b - a for a, b in zip(times, times[1:]) if b > a]
    if not diffs:
        return "n/a"
    return f"median={median(diffs):.3f} ms, min={min(diffs):.3f}, max={max(diffs):.3f}"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    args = parser.parse_args()

    previous: dict[str, tuple[int, int, int] | None] = {"a": None, "b": None}
    changed = {"a": [], "b": []}
    pointers = {"a": Counter(), "b": Counter()}
    changes_after = {"a": Counter(), "b": Counter()}
    previous_callback = None
    total_dispatches = 0
    valid_counts = Counter()
    swaps = Counter()
    swaps_across_gaps = Counter()
    invalid_samples = Counter()
    last_valid_pointer: dict[str, int | None] = {"a": None, "b": None}
    had_gap: dict[str, bool] = {"a": False, "b": False}

    with args.csv_file.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {
            f"object_{slot}_{suffix}"
            for slot in ("a", "b")
            for suffix in ("valid", "pointer", "field_1a", "field_1e")
        }
        if not required.issubset(reader.fieldnames or []):
            parser.error(
                "This CSV predates the object-field diagnostic; rebuild "
                "mk2-timing-research and repeat the opt-in normal-core run."
            )
        for row in reader:
            if row["event"] != "dispatch":
                continue
            total_dispatches += 1
            now = float(row["emulated_ms"])
            for slot in ("a", "b"):
                if int(row[f"object_{slot}_valid"]) == 0:
                    invalid_samples[slot] += 1
                    previous[slot] = None
                    had_gap[slot] = True
                    continue
                valid_counts[slot] += 1
                ptr = int(row[f"object_{slot}_pointer"])
                f1a = int(row[f"object_{slot}_field_1a"])
                f1e = int(row[f"object_{slot}_field_1e"])
                pointers[slot][ptr] += 1
                last = previous[slot]
                # A pointer may switch while the global is NULL or a
                # sampled object field is unavailable. Compare with the
                # last valid pointer across these gaps, while clearly
                # separating switches of unknown timing.
                last_ptr = last_valid_pointer[slot]
                if last_ptr is not None and ptr != last_ptr:
                    swaps[slot] += 1
                    if had_gap[slot]:
                        swaps_across_gaps[slot] += 1
                last_valid_pointer[slot] = ptr
                had_gap[slot] = False
                if last and ptr == last[0] and (f1a, f1e) != last[1:]:
                    changed[slot].append(now)
                    changes_after[slot][
                        f"0x{previous_callback:08X}"
                        if previous_callback is not None
                        else "unknown"
                    ] += 1
                previous[slot] = (ptr, f1a, f1e)
            previous_callback = int(row["handler_eip"])

    print(f"File: {args.csv_file.name}")
    print(f"Scheduler dispatches: {total_dispatches:,}")
    for slot in ("a", "b"):
        print(f"\nObject {slot.upper()} candidate:")
        print(f"  Valid samples: {valid_counts[slot]:,}")
        print(f"  Invalid/gap samples: {invalid_samples[slot]:,}")
        print(f"  Observed pointer switches: {swaps[slot]:,} "
              f"(across invalid sample gaps: {swaps_across_gaps[slot]:,})")
        print(f"  Field changes (+0x1A and/or +0x1E): {len(changed[slot]):,}")
        print(f"  Change spacing: {summarize_intervals(changed[slot])}")
        print(f"  Top pointer values: "
              f"{[(hex(k), v) for k, v in pointers[slot].most_common(5)]}")
        print("  Previous task callback when a change was first observed:")
        for callback, count in changes_after[slot].most_common(12):
            print(f"    {callback}: {count:,} changes")
    print("\nCAUTION")
    print("- Fields +0x1A/+0x1E were selected because the callback code "
          "uses them as signed 16-bit values; coordinate semantics "
          "and object identity are not yet verified.")
    print("- Pointer switches across invalid samples are detected "
          "between the last and next valid snapshots; their exact "
          "transition time is not established.")
    print("- Samples occur before callback dispatch. The previously "
          "dispatched task is a correlation candidate, NOT a proven "
          "writer. A direct write watchpoint is needed for attribution.")
    print("- A stationary object generates no field changes even if "
          "its game simulation logic continues at full rate.")
    print("- A field-change interval is not automatically the combat "
          "simulation tick, and it cannot be equated with VGA refresh.")


if __name__ == "__main__":
    main()
