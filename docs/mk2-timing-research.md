# MK2 DOS timing research (experimental)

**Research log and handoff:** [MK2 timing findings (2026-11-09)](mk2-timing-research-findings-2026-11-09.md).
This is the authoritative continuity document for observations, CSV filenames,
uncertainties, current work, and how to resume after a chat reset.


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

## VGA memory-write timeline (`*_vram.csv`)

The trace also creates `mk2_40000_<timestamp>_vram.csv` next to the two
existing files. This third CSV correlates **guest writes to VGA page handlers**
with IRQ0 delivery, VGA vertical-refresh events, and completed source scanouts.

Data is aggregated into **1 millisecond of emulated time per row**. Idle
millisecond buckets are omitted. No files are written from individual video
memory write callbacks. Only the **first write timestamp** in each bucket is
measured exactly; subsequent last-write timestamps are sampled every 32
observed writes to reduce observer effect. All the event timestamps are in
DOSBox's emulated PIC clock domain. Compare rows to `*_frames.csv` for the
times of visually distinct images.

| Column | Meaning |
|---|---|
| `emulated_ms_end` | End of the 1 ms emulated time bucket |
| `write_ops` | Guest write operations (byte, word, dword) handled by VGA callbacks |
| `guest_write_bytes` | Sum of guest write widths; **not** number of changed pixels |
| `write8`, `write16`, `write32` | Breakdown of observed write operations by guest access width |
| `first_write_ms` | Timestamp of the first guest VGA write in this 1 ms bucket |
| `last_write_sample_ms` | Timestamp sampled at first write and every 32nd write; **not guaranteed exact last-write time** |
| `irq0_dispatches`, `last_irq0_ms` | IRQ0 dispatch count and last IRQ0 timestamp in the bucket |
| `vga_refreshes`, `last_vga_ms` | VGA refresh-event count and last timestamp |
| `scanouts`, `last_scanout_ms` | Completed source-frame count and last completion time |
| `vga_mode` | Internal VGA memory mode enum for the selected page-handler configuration |
| `page_handler_covered` | 1 when the selected conventional VGA handler intercepts writes, 0 when direct mapped |
| `mapping_changes` | Number of VGA handler-mode changes in the bucket |

### Coverage limitations and interpretation

**The write counts only include VGA page-handler callbacks.** DOSBox sometimes
maps guest writes directly to backing memory (`VGA_Map_Handler` and LFB direct
mapping), bypassing those callbacks. This probe **does not reroute those
mappings**, deliberately avoiding timing and memory-behaviour changes.

- If `page_handler_covered=0`, the absence of `write_ops` cannot be
  interpreted as an absence of VRAM writes. For high-confidence conclusions,
  analyze samples with `page_handler_covered=1` and nonzero write counts.
- `page_handler_covered=1` describes the **selected conventional VGA page
  handler**, not all possible concurrent direct-mapped LFB or external
  hardware engines. This is particularly relevant for SVGA/VESA games, though
  it should be less relevant to MK2's conventional VGA modes.
- A guest write operation may rewrite a value that is already present, touch
  an unused video-memory bank, or update part of a line still waiting to be
  scanned out. The actual image change is established separately by
  `*_frames.csv`, not by counting guest write operations.
- Because the probe samples the first write timestamp and only every 32nd
  subsequent write, it is suitable for **millisecond-scale activity bursts**,
  **not** locating each individual CPU store within a scanline.
- CPU overhead and CSV volume can be significant in write-intensive scenes.
  Repeat a comparable no-trace control (`DOSBOX_MK2_TRACE=0`) to ensure
  observer effects have not changed MK2's perceived timing.

### Next analysis

For each recurring 3-1-1-2 sequence in `*_frames.csv`, align
`emulated_ms` to the 1 ms `emulated_ms_end` buckets and ask:

1. Does MK2 issue a similar write burst every ~16.65 ms after IRQ0, or do
   distinct write bursts precede separate visually changed frames?
2. Are the apparent 14/28/43 ms image-change intervals already visible in
   **write activity**, or do they emerge only on the VGA scanout boundary?
3. Do long unchanged screen intervals correspond to no writes, writes to an
   unobserved mapping, or writes that do not alter visible pixels?

This instrumentation cannot by itself determine the game's simulation
update rate; that would require targeted guest-side hook points.

## VGA display registers / palette event timeline (`*_regs.csv`)

A fourth opt-in file is generated alongside the PIT/VGA, visual-frame and
VRAM-write traces: `mk2_40000_<timestamp>_regs.csv`.

Unlike the 1 ms-aggregated VRAM-write CSV, this file contains **individual
register-related events with fractional PIC emulated-millisecond timestamps**.
The timing source is the same as `*_frames.csv` and `*_vram.csv`.

Each event includes one or more of the programmed or latched values below:

| Event (`event`) | Meaning |
|---|---|
| `crtc_start_write` | A write to CRTC index 0Ch or 0Dh, including the combined programmed start address |
| `start_latch` | VGA applies the programmed display-start address (`real_start`) on a scheduled display-start latch event |
| `pel_pan_write` | Attribute register 13h programs horizontal pixel panning |
| `pan_latch` | VGA applies the programmed horizontal pixel panning to the draw state |
| `dac_rgb_commit` | The third DAC component byte (port 3C9h) completes a 6-bit R/G/B palette entry, before index increment |
| `dac_mask_write` | Port 3C6h changed the PEL mask |
| `attr_palette_map` | A 16-colour attribute palette entry was mapped to a DAC index |
| `vga_refresh` | VGA refresh callback, with snapshot of programmed and latched state |
| `scanout_complete` | Completed source image, with snapshot of programmed and latched state |

Columns:

- `event_index`: monotonically increasing sequence number, useful for
  preserving ordering when multiple events share the same timestamp.
- `emulated_ms`: fractional PIC clock timestamp in milliseconds.
- `register`, `value`: written register/index and raw value when relevant;
  negative one (`-1`) represents an unused field.
- `display_start_programmed`: current `vga.config.display_start`.
- `display_start_latched`: current `vga.config.real_start`; this may
  lag the programmed address until a scheduled VGA latch event.
- `panning_programmed`: current `vga.config.pel_panning`.
- `panning_latched`: current `vga.draw.panning`.
- `dac_index`, `dac_red6`, `dac_green6`, `dac_blue6`: the selected DAC
  entry and its component values (6-bit components, 0–63). An attribute
  palette mapping event reports only the destination DAC index.

The events are gathered only with `DOSBOX_MK2_TRACE` enabled; otherwise the
probe does no logging. Entries are buffered and flushed every 256 events,
not on every individual I/O write. The trace records programmed values
*after* existing DOSBox register logic runs, without altering it.

### Correlating the traces

For a changed image in `*_frames.csv`:

1. Find the corresponding `scanout_complete` by timestamp in
   `*_regs.csv`. Check whether the latched display start or panning differs
   from the previous completed scanout.
2. Inspect nearby `crtc_start_write` or `pel_pan_write` events and their
   later `start_latch` / `pan_latch` application.
3. Check `dac_rgb_commit`, `dac_mask_write` and
   `attr_palette_map` in the same interval. A palette update may change the
   visible RGB output without a corresponding page-handler VRAM write.
4. Compare those events against write bursts in `*_vram.csv` and the
   `region_mask` in `*_frames.csv`, using emulated timestamps throughout.

**Interpretation limits:** A DAC entry programmed to its previous RGB value
is still logged as a commit; likewise, re-writing an unchanged CRTC value
is logged. A change of a register that affects an unused palette entry may
not alter visible pixels. Display-start and panning are not the only ways
to alter VGA presentation. Per-scanline changes, uncovered direct-mapped
VRAM writes and GPU/host presentation timing are not diagnosed by this probe.
The register CSV identifies candidate causes, not proof of guest simulation
rate or a unique rendering root cause.

## NEW: scanout boundary memory/colour comparison (`*_scanmem.csv`)

In addition to the four existing CSV streams, the branch now emits a **fifth**
file named like `mk2_40000_20261109_XXXXXX_scanmem.csv`. It is **new and
has not yet been built or runtime-tested on Windows**.

It compares read-only fingerprints captured **at the start of scheduled
scanline drawing and at completion of the same non-aborted scanout**:

- `vga.mem.linear` — raw VGA backing memory, limited to the first 256 KiB.
- `vga.fastmem` — accelerated VGA drawing buffer, first 128 KiB.
- `vga.dac.palette_map` — current effective 256-entry RGB colour mapping.
- The final source image `visible_hash` / `scanout_index` from
  `*_frames.csv`, reused without calculating that expensive image hash again.

The new CSV records whether each memory/palette state changed **within the
scanout** (`*_changed_during`) and **between the preceding scanout's end and
the current one's start** (`*_changed_between`). It records scanout geometry,
actual source pointer class (raw/fastmem/other), selected `vga.mode`,
display-start and panning, sample byte lengths, begin/end timestamps and
fingerprints.

**Caution:** whole-buffer hashes at just two instants do not identify when
each line is fetched. Transient changes between snapshots or data outside
these caps are invisible. An unchanged pair of boundary hashes cannot rule
out transient writes or a scanline-mixed image. A changed hash may represent
off-screen data rather than new visible pixels. Compare against
`*_regs.csv`, `*_vram.csv`, and `*_frames.csv` before attributing a cause.
The new sampler increases CPU work only when tracing is enabled.

Test it with a 40k-cycle fight of ~60–90 s and keep all **five** CSV files.
See the [research log](mk2-timing-research-findings-2026-11-09.md) for the
validation checklist and recorded findings.

## Source hooks

- `src/hardware/timer.cpp`: PIT0 reprogramming and scheduled PIT0 events.
- `src/hardware/pic.cpp`: IRQ0 dispatches to the emulated CPU.
- `src/hardware/video/vga_misc.cpp`: port 3DAh reads and vertical retrace bit.
- `src/hardware/video/vga_draw.cpp`: one call per emulated vertical refresh.
- `src/hardware/mk2_timing_probe.h`: low-overhead counters and CSV writer.
- `src/hardware/video/vga_memory.cpp`: per-write, read-only VGA page-handler callbacks and coverage mode.
- `src/hardware/mk2_vram_probe.h`: 1 ms write-event timeline, IRQ0/VGA/scanout correlation and CSV writer.
- `src/hardware/mk2_vga_regs_probe.h`: per-register emulated-timestamp CSV (`_regs.csv`).
- `src/hardware/mk2_scanmem_probe.h`: raw/fastmem and effective VGA palette boundary hashes, `_scanmem.csv`.
- `src/gui/render/mk2_frame_probe.h`: exposes existing visible image hash to the boundary sampler without rehashing.
- `src/hardware/video/vga_crtc.cpp`: CRTC 0Ch/0Dh start-address programming events.
- `src/hardware/video/vga_attr.cpp`: attribute palette mapping and horizontal panning programming.
- `src/hardware/video/vga_dac.cpp`: DAC RGB entry commits and PEL-mask changes.
- `src/gui/render/render.cpp`: completed source-frame sampling after source latch.
- `src/gui/render/mk2_frame_probe.h`: frame and visible-palette fingerprints, per-scanout CSV.

The instrumentation is global to DOSBox when enabled, not tied to an MK2
executable hash. Use it for MK1/MK3 comparisons too. Avoid treating the
early DOS prompt rows as MK2 gameplay measurements.

## Validation and limits

The PIT/VGA CSV implementation was previously built and exercised by the user
on Windows. **The newly added VGA register probe has not yet been compiled or
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
