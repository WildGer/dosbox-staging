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
file named like `mk2_40000_20261110_124505_scanmem.csv`. **The
2026-11-10 capture compiled and ran successfully on Windows**. All 10,505
sampled scanouts aligned exactly with the source-frame CSV hashes and times.
See the linked findings log for the classification of the remaining 696
source-image changes without observed VRAM writes or DAC RGB commits.

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

## Verified: sparse per-scanline source and renderer-cache comparison (`*_lines.csv`)

The sixth opt-in CSV, e.g. `mk2_40000_20261110_XXXXXX_lines.csv`,
collects **individual VGA scanline samples** in tightly scoped windows.
This is the follow-up to the 2026-11-10 five-CSV finding: 696 visually
different 320×200 frames had no observed VGA write or DAC RGB commit
in the previous scanout interval and no sampled net VRAM/palette boundary
changes. This result is **unexplained, not proven to be an emulation bug**.

### Collection strategy and interpretation

- Enabled through the **existing `DOSBOX_MK2_TRACE` variable**. There is no
  new command-line option.
- **Only** collect in Mode 13-style 320×200 VGA (the observed RGB source is
  320×200×4 bytes); **only** during the 1-second emulated windows
  **40–41, 60–61, 80–81, 100–101 and 120–121 seconds** after launch.
  The first sampled frame within each window is a baseline. This limits
  observer effects. Gameplay must overlap these windows for meaningful data.
- Immediately before each VGA line conversion, record its emulated time,
  actual `vga.draw.address`, masked video-memory offset, source kind
  (`1=raw vga.mem.linear`, `2=vga.fastmem`, `3=other`), a fingerprint
  of the 320 source-index bytes and a fingerprint of the **effective**
  256-colour RGB palette.
- Immediately after `VGA_DrawLine()`, fingerprint the 1280 bytes of its
  32-bit RGB output **before** they enter `ReelMagic_RENDER_DrawLine` and
  the software scaler.
- After the complete, non-aborted scanout has been latched by the renderer,
  hash the corresponding source-cache row and compare with the VGA line
  output. Sampling rows are written to the CSV **after frame completion**,
  never from inside individual line-event callbacks. No rendered image
  pixels, original assets or MK2 EXE are exported.
- All hashes are 64-bit FNV-1a fingerprints, suitable for comparing output,
  not cryptographic integrity proofs.

Columns:

| Column | Meaning |
|---|---|
| `scanout_index` | Join key to `*_frames.csv` and `*_scanmem.csv` |
| `line` | Source image row 0–199 |
| `frame_begin_ms`, `line_sample_ms`, `frame_complete_ms` | Fractional emulated PIC timestamps |
| `draw_address`, `masked_offset` | Video memory address requested by VGA and address masked into source |
| `source_kind` | Actual raw-memory or fastmem pointer class |
| `previous_valid` | Has another sampled frame of this line in the current window |
| `source_changed` | Source-index bytes differ from the preceding sampled frame |
| `palette_changed` | Effective RGB lookup differs from preceding sampled frame |
| `output_changed` | VGA-produced RGB line differs from preceding sampled frame |
| `cache_changed` | Renderer source-cache line differs from preceding sampled frame |
| `output_equals_cache` | VGA RGB bytes match the retained pre-scaler source-cache row |
| `source_hash`, `palette_hash`, `vga_output_hash`, `renderer_cache_hash` | Four hex fingerprints |

**Important limitations:** The RGB palette hash covers **all** entries,
including those unused by a specific line; a changed palette hash does not
prove that visible pixels changed. The source hash captures **a row at the
instant of the conversion**, not VRAM every CPU cycle. Writes that change
and restore pixels before the sample can escape detection. A mismatch with
the final renderer cache can be due to resizing, scan doubling, another
renderer behaviour, or true source/cache discrepancy; do not assume a bug.
The test is restricted to the observed MK2 graphics configuration and does
not claim coverage of every DOS graphics mode. These source-cache rows
are **not** equivalent to final GPU presentation timings.

**Windows runtime validated in 2026-11-10 capture**
`mk2_40000_20261110_131108`: 350 complete sampled graphics frames
and 70,000 line rows; all five one-second windows produced the expected
70 frames of 200 lines each; sampled line and frame indices align with
`*_frames.csv`. All 70,000 VGA RGB output line hashes match their
corresponding latched renderer-cache row hashes. Out of 69,000 comparable
(non-baseline) line rows, **every one of the 14,259 visibly changed RGB
lines had a changed source-index hash and/or effective palette hash**.
Not one observed RGB line change lacked both changes.

In that run, 837 changed graphics frames with no observed VGA writes/DAC
commits or net VRAM/palette changes between current scanout boundaries
**all followed scanouts with VRAM modified during the previous draw**.
32 of these were directly line-sampled, with source-index line changes.
This supports next-scanout visibility of writes made after earlier lines
were already read; it does not prove a defect or measure MK2 simulation
frequency. See the [research findings](mk2-timing-research-findings-2026-11-09.md)
for the full method, counts and limitations.

For additional capture, run MK2 for at least 85 seconds to include the
first three windows, and collect all **six CSV files from the same run**.
Compare sampled `scanout_index` with `_frames.csv` `region_mask`
and `_scanmem.csv` data, not with unrelated sessions.

## NEW: guest execution-context sampling (`*_guest.csv`)

This is a **discovery step toward measuring MK2's actual game simulation
ticks**, **not yet a simulation tick counter**. The previous six probes
characterized PIT (60.052 Hz), VGA (70.09 Hz), drawing bursts, the DAC,
completed source images and individual scanlines. They cannot identify the
program code that advances fighter positions, physics, input processing or
animation state.

The seventh probe observes the **guest CPU context** at meaningful events,
without patching the guest EXE, overriding CPU scheduling or changing video
timing. When `DOSBOX_MK2_TRACE` is enabled it creates a new file
`mk2_40000_<timestamp>_guest.csv` in addition to the six existing CSVs.

**Collection range:** emulated time **30 to 150 seconds** after launch.
Outside this range, no context rows are captured. Sampling is deliberately
limited rather than tracing every guest instruction:

| Event name | What is actually sampled |
|---|---|
| `irq0_pre` | CPU state immediately **before** DOSBox delivers the IRQ0 interrupt; this is **not** the IRQ handler entry |
| `vram_first_write_ms` | CPU state at the first *observed VGA page-handler write* in a 1-ms bucket, **not** the first write instruction of a frame |
| `vga_status_rising` | CPU state on a 3DAh read when the returned vertical-sync bit changes low→high |
| `vga_status_sample` | CPU state every 2048th 3DAh read |
| `clock_10ms` | Scheduler CPU-state sample every 10 emulated milliseconds, independent of graphics events |

A fixed-size internal buffer holds records and flushes in batches at the
emulated-second boundary (or when filled), rather than writing from the hot
IRQ, I/O and VRAM handlers. An exit-time flush preserves the last partial
batch. Because polling and writes are hot paths, the trace still has
observer overhead; run a no-trace control before trusting host timings.

The CSV exposes `sample_index`, `emulated_ms`, `event`, `cs`,
`cs_base`, `eip`, `linear_cs_eip`, `protected_mode`, `cpl`,
`vga_status`, and guest `EAX/EBX/ECX/EDX/ESI/EDI/EBP/ESP`.

**Critical limitations:**

- Under a **dynamic CPU core**, the saved `EIP` visible in a hardware or
  video callback may refer to a translation-block boundary rather than
  the exact triggering instruction. Even in the normal core, the callback
  may see a pre-instruction address or another nearby location. Treat all
  code addresses as **candidates to validate**, not exact source sites.
- The interrupted `CS:EIP` before IRQ0 is **not** an ISR-entry address,
  and seeing ~60 such events per second **does not prove** MK2's simulation
  advances 60 times per second.
- Status polling hotspots identify waiting loops; VRAM write hotspots
  identify rendering/memory copy routines. These can help locate a game
  loop but are not themselves logic ticks.
- `linear_cs_eip` uses the current CS segment base plus saved EIP. Keep
  CS selector, CS base and mode with an address when investigating a DOS
  protected-mode extender; a raw physical address alone is insufficient.
- No code breakpoints, instruction counting, guest-memory-state watches,
  modifications to MK2 or simulation-rate claims are made at this stage.

### Offline candidate discovery

A small **standard-library-only Python script** is provided:

```powershell
python scripts/analyze_mk2_guest_trace.py "$env:USERPROFILE\Documents\MK2Timing\mk2_40000_<timestamp>_guest.csv"
```

It reports the most frequent guest `CS:EIP` and clustered address
windows **separately by event type**, with sample count and time spacing.
It deliberately does **not** label hot locations as game simulation
updates. A later, targeted experiment must confirm a routine with guest
disassembly or direct state advancement before counting its calls.

**Validated on Windows (2026-11-10):** The user compiled and ran the
seventh guest-context probe at 40,000 cycles. The analyzer reported
**30,385 samples** over the 30–150s window. All 1,473
`vga_status_rising` and all 5,029 `vga_status_sample` events
reported `CS:EIP=0180:002196D5`; 7,097 of 7,207 IRQ0 pre-dispatch
samples reported `0180:00205489`; 4,676 first-VRAM-write
samples clustered mainly around EIP `00204200–002049FF`.

**This validates collection, not exact guest instruction addresses
or true logic ticks.** Protected-mode `core=auto` uses dynamic
translation. An I/O callback may report a saved EIP at a translated-block
boundary. Before locating a game-loop routine, run a short
`core=normal` comparison in a separate capture:

```powershell
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$env:DOSBOX_MK2_TRACE = "$env:USERPROFILE\Documents\MK2Timing\mk2_normal_40000_$stamp.csv"
& "$env:USERPROFILE\Documents\dosbox-staging\build\release-windows\Release\dosbox.exe" --nolocalconf --noautoexec --set "core=normal" --set "cpu_cycles_protected=40000" --set "presentation_mode=dos-rate" --set "vsync=off" "D:\GoG\Mortal Kombat 2\mk2\mk2.exe"
```

The normal core executes guest instructions individually and is
considerably slower. This is a **code-location verification run only**;
do not equate its host timing and changed-image cadence with the
existing dynamic-core measurements. Sample some active gameplay after
emulated 30s, then inspect the new `_guest.csv` using the same
`scripts/analyze_mk2_guest_trace.py` script.

Disassemble matching candidate code using the correct DOS extender
and load map. `EIP` is a guest virtual offset, not automatically an
offset into the original MK2.EXE file. A verified code site that updates
gameplay state will be needed before implementing a real simulation
tick counter.

## Local guest instruction snapshot (`*_guest_code.csv`)

After validating the sampled CPU locations in both `core=dynamic`
and `core=normal`, the next experiment is to **inspect a small amount of
actual guest machine code**. The 2026-11-10 normal-core run had
6,963 / 7,076 IRQ0-pre-dispatch samples in the 002054xx page, equally
distributed over EIP 00205489/8F/92/94. Every one of 6,933 sampled
3DAh status port reads used EIP 002196D5. These are **candidate
wait/poll routines, not identified game logic ticks**.

The updated `src/hardware/mk2_guest_probe.h` captures **at most
one 64-byte read-only memory snippet at each of six confirmed code sites**,
when the existing `DOSBOX_MK2_TRACE` variable is enabled.
No per-instruction hook is involved. The separate local
`*_guest_code.csv` contains a few memory bytes at
`0180:00205489`, `0180:002196D5`, `0180:0020479A`,
`0180:0020461A`, `0180:002042FC` and
`0180:0020C81E` when the relevant event actually occurs.
A row can be absent if the guest never reaches that PC while sampled.
The CSV has `site,emulated_ms,cs,cs_base,observed_eip,
snapshot_start,bytes_hex` fields.

These bytes are captured from **the user's private copy of the game
loaded into emulated memory**. The instrumentation in the public
repository contains **no actual game bytes**. Do not commit any generated
`_guest_code.csv` or extracted binaries to GitHub.

### Run and inspect

```powershell
cd "$env:USERPROFILE\Documents\dosbox-staging"
git switch mk2-timing-research
git pull --ff-only
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --build --preset=release-windows --parallel 8

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$env:DOSBOX_MK2_TRACE = "$env:USERPROFILE\Documents\MK2Timing\mk2_normal_40000_$stamp.csv"
& "$env:USERPROFILE\Documents\dosbox-staging\build\release-windows\Release\dosbox.exe" --nolocalconf --noautoexec --set "core=normal" --set "cpu_cycles_protected=40000" --set "presentation_mode=dos-rate" --set "vsync=off" "D:\GoG\Mortal Kombat 2\mk2\mk2.exe"

$codeCsv = Join-Path (Split-Path $env:DOSBOX_MK2_TRACE -Parent) ((Split-Path $env:DOSBOX_MK2_TRACE -LeafBase) + "_guest_code.csv")
Test-Path "$codeCsv"
python scripts/extract_mk2_guest_code.py "$codeCsv"
```

The Python stdlib script writes private 64-byte `.bin` files next to
the CSV and prints optional `ndisasm` (NASM) and GNU `objdump`
commands, with guest EIP address origins. Inspect the control flow
around the wait loop and port read and confirm calls/branches; never
assume this output establishes the simulation frequency. The snapshots
are **in-memory guest code**, not MK2.EXE raw file offsets.

**Status:** This targeted code-byte snapshot modification is
**committed but has not yet been compiled or runtime-validated** on
the user's Windows system. The previous seven CSV probes have been run
successfully. If the new CSV is empty, check that the game ran beyond
30 seconds of emulated time and that the compiled binary contains the
new branch head.

## NEW: targeted pending-work signal writer watch (`*_signal.csv`)

The privately decoded MK2 wait loop at `0180:00205489` repeatedly
reads `DS:[ESI+0x24]`, with ESI loaded from `DS:[0x27177C]`.
When the value is nonzero it saves it, adjusts another variable,
and resets `DS:[ESI+0x24]` to zero. We still do not know **who
produces the nonzero signal**, or whether it represents one combat
logic tick.

A new, **separately opt-in**, experimental memory-write watch
(`src/hardware/mk2_signal_probe.h`) records the signal producer's
address and timestamp.

- Just **before IRQ0 dispatch**, when `core=normal` is interrupted
  in the known `0180:00205489–00205494` consumer loop, it reads
  the real DS selector and descriptor base, then the DWORD at
  `DS.base + 0x27177C`, then resolves the watched linear address
  `DS.base + pointer + 0x24`. **Do not assume DS base is zero.**
- A guarded hook in `src/cpu/paging.h` observes normal-core
  byte/word/dword/qword memory writes. It only reads guest memory
  and captures CPU CS:EIP if a store **overlaps the 4-byte signal
  or its pointer variable**. Cross-page stores are observed as
  constituent byte stores.
- Buffered entries are flushed every emulated second. No code or
  guest state is modified. A small default-off branch remains in
  the normal memory-writing path when the watch is disabled.

Set **both** `DOSBOX_MK2_TRACE` (as in all earlier experiments)
and `DOSBOX_MK2_SIGNAL_WATCH=1` before launching. Use **core=normal**.
The probe samples emulated seconds 30–150. It does not capture
direct-mapped writes made by dynamic translation and may miss pointer
changes between consumer refreshes. It has **not yet been compiled
or runtime-validated on Windows**.

The `*_signal.csv` output contains event `resolve`,
`pointer_write`, `pointer_same`, `signal_write` or
`signal_same`, together with `emulated_ms`, consumer DS
selector/base, pointer value and location, actual watched linear
address, original guest store address/width, DWORD before/after,
and writer CS:EIP and DS context.

A nonzero producer store is **not automatically a gameplay tick**.
We must identify/disassemble the writer before assigning meaning.

### Run this experiment

```powershell
cd "$env:USERPROFILE\Documents\dosbox-staging"
git switch mk2-timing-research
git pull --ff-only
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --build --preset=release-windows --parallel 8

$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
$env:DOSBOX_MK2_SIGNAL_WATCH = "1"
$env:DOSBOX_MK2_TRACE = "$env:USERPROFILE\Documents\MK2Timing\mk2_normal_40000_$stamp.csv"

& "$env:USERPROFILE\Documents\dosbox-staging\build\release-windows\Release\dosbox.exe" --nolocalconf --noautoexec --set "core=normal" --set "cpu_cycles_protected=40000" --set "presentation_mode=dos-rate" --set "vsync=off" "D:\GoG\Mortal Kombat 2\mk2\mk2.exe"

$signalCsv = Join-Path (Split-Path $env:DOSBOX_MK2_TRACE -Parent) ((Split-Path $env:DOSBOX_MK2_TRACE -LeafBase) + "_signal.csv")
Test-Path "$signalCsv"
python scripts/analyze_mk2_signal_trace.py "$signalCsv"
Remove-Item Env:DOSBOX_MK2_SIGNAL_WATCH
```

Run at least 60 seconds of *emulated* game time with active combat
during part of seconds 30–150. Share `*_signal.csv` and the
analyzer's stdout; no full game EXE or large graphical CSVs
are needed for this stage. Empty/header-only output means we have
not yet found a matched producer event; check `core=normal`,
the requested environment variable, and the sampled consumer
addresses before making conclusions.

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
- `src/hardware/mk2_scanline_probe.h`: sparse line-level source indices, effective palette, VGA RGB and renderer-cache row fingerprints, `_lines.csv`.
- `src/hardware/mk2_guest_probe.h`: opt-in CPU register/CS:EIP samples at IRQ0, VGA status reads, write-burst starts and scheduler ticks (`_guest.csv`).
- `scripts/analyze_mk2_guest_trace.py`: offline code-location clustering and event summaries (Python standard library only).
- `scripts/extract_mk2_guest_code.py`: turn locally captured 64-byte code snapshots into private binaries; do not commit generated code data.
- `src/hardware/mk2_signal_probe.h`: normal-core, DS-resolved writer watchpoint with `_signal.csv` output.
- `src/cpu/paging.h`: opt-in guarded checks around ordinary guest RAM stores.
- `scripts/analyze_mk2_signal_trace.py`: compare producer and consumer events without assuming gameplay tick cadence.
- `src/hardware/video/vga_draw.cpp`: hook before/after VGA line conversion at precise emulated scanline time.
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
on Windows. **The five-stream integration, including the VGA register
and scanout-memory probes, has now been built and runtime-validated on the
user's Windows machine in the 2026-11-10 capture.** This verifies recording
and cross-file alignment; it does not establish the accuracy of every sampled
guest memory access or a cause for MK2's pacing.

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
