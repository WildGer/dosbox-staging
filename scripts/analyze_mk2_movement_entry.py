#!/usr/bin/env python3
"""Inspect dynamic entry at guest CS:0180 EIP:0023156C.

Requires a new *_movement_entry.csv from opt-in MK2 timing research build.
Optionally correlates *_signal.csv and *_object_steps.csv from the SAME run,
located alongside the entry file using its filename prefix.

- The stack DWORD at entry is a caller/return-address CANDIDATE, not a
  verified callsite: a JMP may enter without a pushed return address.
- Entry frequency does not automatically equal movement or combat tick
  frequency. Judge it against full-DWORD ADDs and the IRQ0 producer.
- All results remain local. No MK2 game bytes are distributed.

Usage:
  python scripts/analyze_mk2_movement_entry.py C:/.../trace_movement_entry.csv
"""
from __future__ import annotations

from bisect import bisect_right
from collections import Counter, defaultdict
import argparse
import csv
from pathlib import Path
from statistics import median


def intervals(times: list[float]) -> str:
    values = sorted(times)
    diffs = [b - a for a, b in zip(values, values[1:]) if b > a]
    if not diffs:
        return "n/a"
    return (f"median={median(diffs):.3f} ms, "
            f"min={min(diffs):.3f}, max={max(diffs):.3f}")


def same_run_path(entry_path: Path, suffix: str) -> Path | None:
    ending = "_movement_entry.csv"
    if not entry_path.name.endswith(ending):
        return None
    return entry_path.with_name(entry_path.name[:-len(ending)] + suffix)


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv_file", type=Path)
    parser.add_argument("--top", type=int, default=12)
    args = parser.parse_args()

    rows = read_rows(args.csv_file)
    valid = [r for r in rows if int(r["entry_eip"]) == 0x0023156C]
    overflow = [r for r in rows if int(r.get("dropped_count", 0)) != 0]
    by_return: Counter[int] = Counter()
    times_by_return: dict[int, list[float]] = defaultdict(list)
    entries_by_return: dict[int, list[dict[str, str]]] = defaultdict(list)
    by_obj_pair: dict[tuple[int, int], list[float]] = defaultdict(list)
    by_ebp: Counter[int] = Counter()
    by_eax: Counter[int] = Counter()
    ebp_object = Counter()
    entry_times = []
    for r in valid:
        now = float(r["emulated_ms"])
        entry_times.append(now)
        ret = int(r["stack_dword"])
        ap = int(r["object_a_pointer"])
        bp = int(r["object_b_pointer"])
        ebp = int(r["entry_ebp"])
        eax = int(r["entry_eax"])
        by_return[ret] += 1
        times_by_return[ret].append(now)
        entries_by_return[ret].append(r)
        by_obj_pair[(ap, bp)].append(now)
        by_ebp[ebp] += 1
        by_eax[eax] += 1
        ebp_object[ebp == ap or ebp == bp] += 1

    print(f"Input: {args.csv_file.name}")
    print(f"Actual 0180:0023156C entry boundaries: {len(valid):,}")
    print(f"Overflow rows: {len(overflow):,}; "
          f"dropped entries: {sum(int(r['dropped_count']) for r in overflow):,}")
    print(f"Overall entry spacing: {intervals(entry_times)}")
    print(f"Entry EBP matches either current object pointer: "
          f"{ebp_object[True]:,} yes; {ebp_object[False]:,} no")

    print("\nMost common stack DWORDs at function entry "
          "(possible return EIPs, not proven callers):")
    verified_returns = {
        0x0022FB5D: "CALL 0x22FB58 -> 0x23156C (verified from guest code)",
        0x00230106: "CALL 0x230101 -> 0x23156C (verified from guest code)",
    }
    for pc, n in by_return.most_common(args.top):
        note = verified_returns.get(pc, "caller not yet verified")
        print(f"  0x{pc:08X}: {n:,} entries, "
              f"{n / len(valid) * 100:.2f}% of entries "
              f"({note}); {intervals(times_by_return[pc])}")

    print("\nCurrent global object-pointer pairs at entry:")
    for (a, b), times in sorted(by_obj_pair.items(),
                                key=lambda it: -len(it[1]))[:args.top]:
        print(f"  A=0x{a:08X} B=0x{b:08X}: "
              f"{len(times):,}, {intervals(times)}")

    print("\nTop EBP values:")
    for ptr, n in by_ebp.most_common(args.top):
        print(f"  EBP=0x{ptr:08X}: {n:,}")
    print("\nTop EAX values:")
    for ptr, n in by_eax.most_common(args.top):
        print(f"  EAX=0x{ptr:08X}: {n:,}")

    signal_path = same_run_path(args.csv_file, "_signal.csv")
    steps_path = same_run_path(args.csv_file, "_object_steps.csv")

    producer = []
    if signal_path and signal_path.is_file():
        for r in read_rows(signal_path):
            # 09B0:08AF real-mode producer. Ignore consumer stores.
            if (r.get("event") == "signal_write" and
                    int(r.get("writer_cs", -1)) == 0x09B0 and
                    int(r.get("writer_eip", -1)) == 0x08AF):
                producer.append(float(r["emulated_ms"]))
        producer.sort()
        if producer:
            tick_indices = [bisect_right(producer, t) - 1
                            for t in entry_times]
            matched = [i for i in tick_indices if i >= 0]
            counts = Counter(matched)
            print("\nAgainst same-run IRQ0 producer:")
            print(f"  Producer writes: {len(producer):,}")
            print(f"  Entries after a producer event: {len(matched):,}")
            print(f"  Distinct producer event indices with entry: "
                  f"{len(counts):,}")
            print(f"  Producer events with multiple entries: "
                  f"{sum(n > 1 for n in counts.values()):,}")
            lag = [t - producer[i]
                   for t, i in zip(entry_times, tick_indices) if i >= 0]
            if lag:
                print(f"  Entry lag from preceding producer: "
                      f"median={median(lag):.3f} ms, "
                      f"min={min(lag):.3f}, max={max(lag):.3f}")
            print("  Distinct IRQ0 producer intervals by return candidate:")
            for pc, group in sorted(entries_by_return.items(),
                                    key=lambda it: -len(it[1]))[:args.top]:
                indices = [
                    bisect_right(producer, float(row["emulated_ms"])) - 1
                    for row in group
                ]
                selected = [i for i in indices if i >= 0]
                multiples = Counter(selected)
                print(
                    f"    0x{pc:08X}: entries={len(group):,}, "
                    f"IRQ0 intervals={len(multiples):,}, "
                    f"intervals with 2+ entries="
                    f"{sum(c > 1 for c in multiples.values()):,}"
                )
            # Separate repetitions of one CALL within an IRQ interval
            # from separate CALL sites sharing the same IRQ interval.
            group_intervals = {}
            for pc, group in entries_by_return.items():
                group_intervals[pc] = {
                    bisect_right(producer, float(row["emulated_ms"])) - 1
                    for row in group
                } - {-1}
            repeated_same_caller = sum(
                len(group) - len(group_intervals[pc])
                for pc, group in entries_by_return.items()
            )
            shared_between_callers = (
                sum(len(ticks) for ticks in group_intervals.values())
                - len(set().union(*group_intervals.values()))
            )
            print("  Multiplicity breakdown:")
            print(f"    Extra entries at same CALL site and tick: "
                  f"{repeated_same_caller:,}")
            print(f"    Extra cross-CALL-site tick memberships: "
                  f"{shared_between_callers:,}")
            print(f"    Sum of both: "
                  f"{repeated_same_caller + shared_between_callers:,} "
                  f"(should equal matched entries minus unique ticks)")
            print("  Caller-specific EAX and EBP context:")
            for pc, group in sorted(entries_by_return.items(),
                                    key=lambda it: -len(it[1]))[:args.top]:
                eax_obj = sum(
                    int(row["entry_eax"]) in (
                        int(row["object_a_pointer"]),
                        int(row["object_b_pointer"]),
                    )
                    for row in group
                )
                ebp_top = Counter(int(row["entry_ebp"]) for row in group)
                print(f"    return=0x{pc:08X}: "
                      f"EAX matches candidate object={eax_obj:,}/{len(group):,}, "
                      f"top EBP={[(f'0x{k:08X}', n) for k, n in ebp_top.most_common(3)]}")
        else:
            print("\nSignal CSV present, but no matching IRQ0 writes.")
    else:
        print("\nNo same-run _signal.csv; skipping IRQ0 correlation.")

    if steps_path and steps_path.is_file():
        steps = read_rows(steps_path)
        axis1 = [r for r in steps
                 if int(r.get("position_offset", -1)) == 0x18]
        print("\nAgainst same-run 32-bit movement ADDs:")
        print(f"  Observed axis +0x18 ADDs: {len(axis1):,}")
        for (obj, ptr), n in Counter(
            (r["object"], int(r["object_pointer"])) for r in axis1
        ).most_common(args.top):
            print(f"  {obj} ptr=0x{ptr:08X}: {n:,} integrations")
        if producer:
            move_ticks = Counter(
                bisect_right(producer, float(r["emulated_ms"])) - 1
                for r in axis1
            )
            ent_ticks = Counter(
                bisect_right(producer, t) - 1 for t in entry_times
            )
            both = set(move_ticks) & set(ent_ticks)
            print(f"  IRQ0 intervals containing both an entry "
                  f"and an object ADD: {len(both):,}")
            print("  Same-run integration overlap by return candidate "
                  "(not proof of a direct caller -> ADD relationship):")
            for pc, group in sorted(entries_by_return.items(),
                                    key=lambda it: -len(it[1]))[:args.top]:
                ticks = {
                    bisect_right(producer, float(row["emulated_ms"])) - 1
                    for row in group
                }
                ticks.discard(-1)
                common = ticks & set(move_ticks)
                print(f"    return=0x{pc:08X}: "
                      f"entry IRQ0 intervals={len(ticks):,}, "
                      f"with ADDs={len(common):,}, "
                      f"without recorded ADDs={len(ticks - set(move_ticks)):,}")
            print("  Entry-versus-ADD ordering within each IRQ0 interval:")
            # Same-tick coincidence is expected when most ticks have ADDs;
            # timestamp ordering is a stronger but still noncausal check.
            ordered_adds = defaultdict(list)
            for row in axis1:
                t = float(row["emulated_ms"])
                k = bisect_right(producer, t) - 1
                if k >= 0:
                    ordered_adds[k].append(
                        (t, row["object"], int(row["object_pointer"]))
                    )
            for calls in ordered_adds.values():
                calls.sort()
            all_move_ticks = set(ordered_adds)
            active_tick_baseline = len(all_move_ticks) / len(producer)
            print(f"  ADD-containing IRQ0 intervals: "
                  f"{len(all_move_ticks):,}/{len(producer):,} "
                  f"({active_tick_baseline * 100:.2f}% of observed producer events)")
            for pc, group in sorted(entries_by_return.items(),
                                    key=lambda it: -len(it[1]))[:args.top]:
                later = []
                earlier = 0
                no_adds = 0
                same_tick_add = 0
                later_objects = Counter()
                for entry in group:
                    t = float(entry["emulated_ms"])
                    tick = bisect_right(producer, t) - 1
                    if tick < 0:
                        continue
                    adds = ordered_adds.get(tick, [])
                    if not adds:
                        no_adds += 1
                        continue
                    same_tick_add += 1
                    if any(addtime < t for addtime, _, _ in adds):
                        earlier += 1
                    post = [(addtime, obj, ptr) for addtime, obj, ptr
                            in adds if addtime >= t]
                    if post:
                        first = min(post, key=lambda e: e[0])
                        later.append(first[0] - t)
                        later_objects[(first[1], first[2])] += 1
                print(f"    return=0x{pc:08X}: "
                      f"entry events with ADD in same tick={same_tick_add:,}; "
                      f"with later ADD={len(later):,}; "
                      f"with earlier ADD={earlier:,}; "
                      f"without ADD in tick={no_adds:,}")
                if later:
                    print(f"      Delay to first subsequent ADD in tick: "
                          f"median={median(later):.3f}ms, "
                          f"min={min(later):.3f}ms, "
                          f"max={max(later):.3f}ms")
                    print(f"      First subsequent tracked object: "
                          f"{[(obj, f'0x{ptr:08X}', n) for (obj, ptr), n in later_objects.most_common(4)]}")
            print("  Note: even a consistently later ADD does not prove "
                  "it is CALLED by this entry; unrelated work can run "
                  "later within the same IRQ interval.")
            print("  Note: multiple objects may integrate per timer "
                  "interval; do not equate event counts directly.")
    else:
        print("\nNo same-run _object_steps.csv; skipping motion comparison.")

    print("\nInterpretation limits:")
    print("- The function entry is observed dynamically, but its exact "
          "role and direct callers still require x86 disassembly.")
    print("- Stack DWORD is only a candidate return EIP.")
    print("- Multiple entries and updates may occur per IRQ0 tick.")
    print("- The display scanning at ~70 Hz does not establish the "
          "game's new image production cadence.")


if __name__ == "__main__":
    main()
