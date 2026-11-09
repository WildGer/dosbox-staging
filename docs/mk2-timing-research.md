# MK2 DOS timing research (experimental)

This fork includes **opt-in, low-volume CSV telemetry** to investigate why
*Mortal Kombat II* for MS-DOS appears less fluid than MK1/MK3.

**No MK2 executable or game data is included or modified.**
The probe doesn't change PIT divisors, IRQ timing, VGA scanout, CPU cycles,
gameplay logic or video presentation.

## Build

This branch follows the fork's current `main` (November 2026 development
code), **not** the 0.83.0 release tag. For up-to-date Windows prerequisites,
see [build-windows.md](build-windows.md).

From PowerShell:

```powershell
git clone --branch mk2-timing-research https://github.com/WildGer/dosbox-staging.git
cd dosbox-staging
$env:VCPKG_ROOT = "C:\vcpkg"
# Visual Studio 2022 + Clang:
cmake --preset=release-windows-vs2022
cmake --build --preset=release-windows-vs2022
```

If using Visual Studio 2026, replace `release-windows-vs2022` with
`release-windows`. CMake, Clang tools and vcpkg must be installed first.

## Enable the CSV trace

Create a writable directory (for example `C:\MK2Captures`), then launch
the **newly compiled executable** from the **same PowerShell session**:

```powershell
New-Item -ItemType Directory -Force C:\MK2Captures
$env:DOSBOX_MK2_TRACE = "C:\MK2Captures\mk2_40000.csv"
# Run the executable produced by the build, with your normal MK2 config.
# Locate it with: Get-ChildItem .\build -Recurse -Filter "dosbox*.exe"
```

The trace starts automatically when DOSBox runs. Start MK2, let a fight run for
20-30 seconds, then exit DOSBox. A new row is written per ~1 emulated second,
and flushed to disk. Recording video is **not required** to collect the CSV.

If `DOSBOX_MK2_TRACE` is unset or set to `0`, the trace stays disabled.
Set it to `1` for `mk2_timing.csv` in the working directory, or set an
absolute filename as shown above. The folder must already exist.

## Suggested tests

Keep video, audio, machine type, presentation mode and gameplay scenario
constant, and repeat with:

| Run | `cpu_cycles_protected` | CSV filename |
|---|---:|---|
| A | 20000 | `mk2_20000.csv` |
| B | 40000 | `mk2_40000.csv` |
| C | 80000 | `mk2_80000.csv` |

Include ~5 seconds before the fight so PIT reprogramming can be seen. Keep
the full CSV: the early rows may show DOS at ~18.2 Hz and later rows MK2 at
~60 Hz. A control test at the DOS prompt is also useful.

## CSV columns

| Field | Meaning |
|---|---|
| `emulated_ms` | Time on DOSBox's emulated PIC clock at end of window |
| `host_elapsed_ms` | Real monotonic time since first VGA refresh observed |
| `window_emulated_ms` | Length of reporting window in emulated time |
| `window_host_ms` | Real time consumed by the same window |
| `pit0_events` | Number of PIT channel 0 scheduled events (not necessarily delivered IRQs) |
| `pit0_measured_hz` | PIT events per emulated second |
| `irq0_dispatches` | Number of IRQ0 hardware interrupts actually dispatched to the CPU |
| `irq0_measured_hz` | IRQ0 dispatches per emulated second |
| `vga_refreshes` | Emulated vertical refresh events |
| `vga_measured_hz` | Emulated refresh events per emulated second |
| `vga_3da_reads` | Reads of the VGA input-status register (port 3DAh) |
| `vga_3da_vsync_high` | Those status reads where vertical retrace bit 3 was set |
| `pit0_divisor` | Last fully programmed PIT0 count (0 denotes not yet observed) |
| `pit0_programmed_hz` | Latest requested PIT0 frequency |
| `pit0_programs` | Number of completed PIT0 reprogramming operations in the window |

The actual per-window event rates are more informative than the divider alone.
A **programmed 60 Hz PIT does not prove that game logic or rendering runs at
60 FPS**. Likewise, VGA refresh frequency does not indicate the number of
unique rendered frames. Compare separately against captured AVI frame-change
analysis and the fight timer.

The code records **scheduled PIT0 events vs. dispatched IRQ0 interrupts**
separately, so a masked/pending interrupt cannot be mistaken for a game tick.
It does **not** yet prove that MK2's IRQ handler runs once per simulation tick.

## Source hooks

- `src/hardware/timer.cpp`: PIT0 reprogramming and scheduled PIT0 events.
- `src/hardware/pic.cpp`: IRQ0 dispatches to the emulated CPU.
- `src/hardware/video/vga_misc.cpp`: port 3DAh reads and vertical retrace bit.
- `src/hardware/video/vga_draw.cpp`: one call per emulated vertical refresh.
- `src/hardware/mk2_timing_probe.h`: low-overhead counters and CSV writer.

The instrumentation is global to DOSBox when enabled, not tied to an MK2
executable hash. Use it for MK1/MK3 comparisons too. Avoid treating the
early DOS prompt rows as MK2 gameplay measurements.

## Validation and limits

The standalone CSV helper has been syntax-compiled and exercised with a
small C++20 harness. **The full DOSBox build and MK2 runtime integration
still require validation on the Windows machine.**

Trace overhead is minimized by counting in hot paths and writing approximately
once per emulated second, but nonzero measurement overhead remains. Always run
a control test without tracing to check for behavioral changes.

If the game never reaches its expected emulated frame rates, compare
`window_host_ms` to `window_emulated_ms`; a host that takes much longer than
the emulated interval may not keep up. If rates remain consistent at all three
cycle settings, the next step is instrumenting the game's simulation-update
routine, **not** changing PIT or VGA timing blindly.
