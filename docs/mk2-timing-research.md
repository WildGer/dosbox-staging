# MK2 DOS timing research (experimental)

This fork includes **opt-in CSV telemetry** to investigate why
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
20-30 seconds, then exit DOSBox. The PIT/VGA summary file gets a row per ~1 emulated second,
and flushed to disk **from the core millisecond ticker**, not from the VGA
vertical refresh callback. This keeps logging when VGA output changes mode or
stops sending refresh callbacks. Recording video is **not required** to collect
the CSV.

If `DOSBOX_MK2_TRACE` is unset or set to `0`, both trace streams stay disabled.
Set it to `1` for `mk2_timing.csv` plus `mk2_timing_frames.csv` in the
working directory, or set an absolute filename as shown above. The folder must
already exist. With `mk2_40000.csv`, the second file is `mk2_40000_frames.csv`.

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
| `host_elapsed_ms` | Real monotonic time since the first emulated clock tick observed |
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

## Source scanout frame pacing (`*_frames.csv`)

A separate CSV now captures **every completed, non-aborted source scanout**,
just after the renderer latches its complete source frame. The timestamp is
taken **before hashing**. This is an emulated/renderer boundary, not a
measurement of what the monitor physically displayed or of the moment the
game wrote to VRAM.

For each scanout, the probe computes:

- An FNV-1a fingerprint of the source pixel data.
- For indexed 8-bit frames, a fingerprint of each pixel's **mapped RGB
  colour**, plus a separate diagnostic fingerprint of only the palette
  entries actually used. Changed raw palette indices that result in exactly
  the same displayed colours do **not** count as a new visible image; a changed
  unused palette entry does not count either.
- Changes to frame geometry (width/height/pitch/pixel format).

Visible-image changes are determined by comparing the RGB-equivalent
fingerprint and geometry with the previous complete scanout.
The hashing does not modify VRAM, PIT, IRQ0, VGA settings, source pixels or
presentation controls. Pause-time synthetic repaints and aborted scanouts
are excluded. It does add some CPU load while tracing is enabled.

| Column | Meaning |
|---|---|
| `scanout_index` | Completed source scanout sequence number |
| `emulated_ms` | PIC emulated clock timestamp in milliseconds, with fractional precision |
| `host_elapsed_ms` | Monotonic real time since the first trace frame |
| `scanout_interval_emulated_ms` | Time between this and prior completed scanout, emulated |
| `scanout_interval_host_ms` | Same interval on the real host clock |
| `changed` | 1 when the RGB-equivalent source image or geometry changed |
| `pixels_changed` | 1 when source pixel bytes differ from previous scanout |
| `visible_palette_changed` | 1 when used palette RGB fingerprint differs |
| `geometry_changed` | 1 on resolution, pitch, or pixel format change |
| `consecutive_repeats` | Count of consecutive identical source scanouts after last change |
| `change_interval_emulated_ms` | Time since the preceding changed image, or 0 if unchanged / first |
| `change_interval_host_ms` | Same visual-change interval on host clock |
| `width`, `height`, `pixel_format` | Source frame geometry and pixel format enum |
| `pixel_hash`, `visible_palette_hash`, `visible_hash` | 64-bit hexadecimal fingerprints of raw bytes, used colours, and RGB-equivalent image respectively; **not** original image contents |
| `region_mask` | 40-bit hexadecimal mask: 8 columns x 5 rows, bit 0 top-left, bit 39 bottom-right; one bit per region whose pixels changed |
| `regions_changed` | Number of changed grid cells, 0–40 |
| `regions_valid` | 1 when region fingerprinting supports the source pixel layout, otherwise 0 (ignore mask) |

Interpretation:

- Count changed frames per **emulated second**, then separately compare the
  **host** change intervals. A 70 Hz scanout can contain repeated images.
- For a 60 Hz source shown by a 70 Hz scanout, expect repeated images at
  intervals, but **do not presume** that the game necessarily generates
  60 distinct images per second.
- For repeated captures, compare the distribution of nonzero
  `change_interval_emulated_ms` (median, p95/p99, repeated scanout runs),
  not just average changed frames/sec.
- Exclude the loading screen and mode transitions from gameplay statistics.
- A changed source image need not mean a game-logic update: HUD changes,
  palette animation, and different portions of a scroll are counted as image
  changes. `pixels_changed=1` alone is **not** proof of a visually changed
  image when indexed colours map to identical RGB. Hash collisions are
  theoretically possible but extremely unlikely.
- Hashes represent sampled source scanout images, *not* GPU swaps, desktop
  compositor timing, monitor presentation, or when the game wrote pixels to
  video memory. Separating actual simulation ticks would require an
  additional instrumentation stage.

The frame CSV is written next to the existing PIT/VGA CSV, using the same
environment variable and launch script. No new switch is required.

### Regional follow-up: 8x5 changes

The frame trace now adds a 40-bit `region_mask` for each completed
source scanout. This divides the frame into **8 columns by 5 rows**.

```text
+--+--+--+--+--+--+--+--+
|00|01|02|03|04|05|06|07|  top row
+--+--+--+--+--+--+--+--+
|08|09|10|11|12|13|14|15|
+--+--+--+--+--+--+--+--+
|16|17|18|19|20|21|22|23|
+--+--+--+--+--+--+--+--+
|24|25|26|27|28|29|30|31|
+--+--+--+--+--+--+--+--+
|32|33|34|35|36|37|38|39|  bottom row
+--+--+--+--+--+--+--+--+
```

For example, `region_mask=0000000001` means only the top-left cell
changed; `0000000100` means only cell 8 changed (leftmost cell in
the second row). A 320x200 source frame uses roughly 40x40-pixel cells.

A regional change is based on RGB colours for Indexed8 and on source
pixel bytes for other supported packed formats. The implementation runs
only when `DOSBOX_MK2_TRACE` is enabled and does not alter frame output.
The 8x5 region probe adds **another read-only pass** across source
pixels, so check observer overhead if results are unexpectedly different.

To study whether the 3-1-1-2 sequence is produced by changes across
the entire image or particular characters/background elements, compare
the `region_mask` across successive `changed=1` rows. No image
content or game executable is exported.

## Source hooks

- `src/hardware/timer.cpp`: PIT0 reprogramming and scheduled PIT0 events.
- `src/hardware/pic.cpp`: IRQ0 dispatches to the emulated CPU.
- `src/hardware/video/vga_misc.cpp`: port 3DAh reads and vertical retrace bit.
- `src/hardware/video/vga_draw.cpp`: one call per emulated vertical refresh.
- `src/hardware/mk2_timing_probe.h`: low-overhead counters and CSV writer.
- `src/gui/render/render.cpp`: completed source-frame sampling after source latch.
- `src/gui/render/mk2_frame_probe.h`: frame and visible-palette fingerprints, per-scanout CSV.

The instrumentation is global to DOSBox when enabled, not tied to an MK2
executable hash. Use it for MK1/MK3 comparisons too. Avoid treating the
early DOS prompt rows as MK2 gameplay measurements.

## Validation and limits

The PIT/VGA CSV implementation was previously built and exercised by the user
on Windows. **The newly added frame probe has not yet been compiled or
runtime-validated on the user's Windows machine.**

Time-counter overhead is minimized by counting in hot paths and writing once
per emulated second. Frame tracing adds source-buffer and per-region fingerprints for each
completed scanout (typically ~70 per second), and buffers CSV writes;
nonzero measurement overhead remains. Always run
a control test without tracing to check for behavioral changes.

If the game never reaches its expected emulated frame rates, compare
`window_host_ms` to `window_emulated_ms`; a host that takes much longer than
the emulated interval may not keep up. If rates remain consistent at all three
cycle settings, the next step is instrumenting the game's simulation-update
routine, **not** changing PIT or VGA timing blindly.

## Direct launch without DOS MOUNT commands

DOSBox Staging accepts a host-side `.EXE` path as a **positional argument**;
it mounts the containing directory as C: and runs the executable automatically.
This is preferable to quoting `MOUNT` inside repeated `-c` arguments.

From PowerShell after rebuilding the latest branch, set the environment variable
and run your compiled DOSBox with an absolute `MK2.EXE` path:

```powershell
$env:DOSBOX_MK2_TRACE = "$env:USERPROFILE\Documents\MK2Timing\mk2_40000.csv"
& 'C:\Users\Usuario\Documents\dosbox-staging\build\release-windows\dosbox.exe' --noautoexec --set 'cpu_cycles_protected=40000' 'D:\GoG\Mortal Kombat 2\MK2.EXE'
```

Adjust the actual executable locations. The destination directory must exist.
If the CSV has only the header or unexpectedly few rows, compare the emitted
`window_host_ms` and `window_emulated_ms` values, and ensure the executable
was rebuilt with the latest `mk2-timing-research` commits.
