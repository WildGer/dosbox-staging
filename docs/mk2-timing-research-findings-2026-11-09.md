# Mortal Kombat II (DOS) — timing investigation log and handoff

**Project:** DOSBox Staging research fork, `WildGer/dosbox-staging`
**Branch:** `mk2-timing-research`
**Working platform:** Windows 11, Visual Studio 2026 (CMake `release-windows` preset), ClangCL, vcpkg at `C:\vcpkg`
**Research date:** 2026-11-09
**Game:** GOG installation of *Mortal Kombat II*, launched as an external DOS .EXE.
**Status (updated 2026-11-10):** Six CSV probes have been compiled and runtime-validated on Windows; the sixth scanline probe captured 70,000 lines and confirmed 70,000 matching VGA vs renderer-cache hashes. The next task—locating MK2 guest game-loop code—now has a **seventh** opt-in `_guest.csv` execution-context sampler committed to the branch, **pending Windows compilation and test**. No direct simulation-tick measurement has yet been made.

## Scope and non-negotiable constraints

The goal is to understand the **source of inconsistent visual frame pacing in the DOS version of MK2**, preserving original simulation, timing, input and gameplay behavior while avoiding unnecessary resource usage. FPS numbers alone are not adequate: distinguish logic ticks, guest VRAM writes, VGA scanout, new visual images and host presentation.

- Research only, opt-in using `DOSBOX_MK2_TRACE`. Do **not** change the guest EXE, PIT divisor, IRQ0 schedule, VGA clocks or CPU-cycle rate to improve a graph without isolating the root cause.
- No copyrighted game executable or assets are part of the fork. The user owns a legitimate installation. Keep source files and CSV datasets separate from any commercial game contents.
- The work branch is based on DOSBox Staging development code, not an older release. Changes are experimental and must not alter behavior when tracing is off.
- The observed `*_frames.csv` reports changes in the **source image constructed by DOSBox**, not physical GPU presentation, and `changed=1` is **not** equivalent to a simulation tick or a fighter animation frame.
- Expect nonzero overhead from memory hashing, event logging and CSV output. Always compare a trace-enabled run with a control run when performance is in doubt.

## Reproducible launch configuration

The existing PowerShell launch script was used to run:

- DOSBox: `C:\Users\Usuario\Documents\dosbox-staging\build\release-windows\Release\dosbox.exe`
- Game: `D:\GoG\Mortal Kombat 2\mk2\mk2.exe`
- Example flags: `--nolocalconf --noautoexec --set cpu_cycles_protected=40000 --set presentation_mode=dos-rate --set vsync=off`, followed by the guest EXE path.
- Trace directory: `C:\Users\Usuario\Documents\MK2Timing`.
- The launch script supplies `DOSBOX_MK2_TRACE` pointing at `mk2_<cycles>_<timestamp>.csv`.
- Run with protected-mode CPU cycles of 20,000, 40,000 and 80,000; leave screen mode, game scene, presentation, audio and host configuration the same when comparing.
- The launcher is a **local user script**, not currently a tracked repository artifact. If it is lost, reconstruct the command from the lines above and the instructions in [mk2-timing-research.md](mk2-timing-research.md).

Build from repository:

```powershell
cd "$env:USERPROFILE\Documents\dosbox-staging"
git switch mk2-timing-research
git pull --ff-only
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --preset=release-windows
cmake --build --preset=release-windows --parallel 8
```

If CMake has already been configured with this preset, the build step alone is sufficient.

## Instrumentation inventory

All filenames share the launch's `DOSBOX_MK2_TRACE` prefix:

| File suffix | Purpose | Source hooks |
|---|---|---|
| `.csv` | PIT0 events/programming, IRQ0 delivery, VGA vertical refresh, 3DAh polls, host/emulated second windows | `hardware/timer.cpp`, `hardware/pic.cpp`, `hardware/video/vga_misc.cpp`, `hardware/video/vga_draw.cpp`, `hardware/mk2_timing_probe.h` |
| `_frames.csv` | Per-completed-scanout RGB-equivalent image hash, repeated images, real/emulated intervals, 8×5 regional change mask | `gui/render/mk2_frame_probe.h`, `gui/render/render.cpp` |
| `_vram.csv` | VGA memory-page-handler write-operation bursts aggregated by **1 ms emulated**, plus IRQ0, VGA and completion timestamps and handler coverage | `hardware/video/vga_memory.cpp`, `hardware/mk2_vram_probe.h` |
| `_regs.csv` | Individual CRTC start-address programming/latching, horizontal panning programming/latching, DAC colour commits and refresh/scanout state | `hardware/video/vga_crtc.cpp`, `hardware/video/vga_attr.cpp`, `hardware/video/vga_dac.cpp`, `hardware/video/vga_draw.cpp`, `hardware/mk2_vga_regs_probe.h` |
| `_scanmem.csv` | **Validated in 2026-11-10 capture.** Start/end raw VRAM, fastmem, effective DAC RGB hashes and frame cross-reference | `hardware/mk2_scanmem_probe.h`, `hardware/video/vga_draw.cpp`, `gui/render/render.cpp` |
| **`_lines.csv`** | **New; Windows validation pending.** Sparse, per-scanline source, palette, VGA output and completed renderer source-cache hashes | `hardware/mk2_scanline_probe.h`, `hardware/video/vga_draw.cpp`, `gui/render/render.cpp` |

`_frames.csv` has a sequential scanout index. `_regs.csv` timestamps are fractional PIC-emulated milliseconds; `_vram.csv` buckets are **coarser** (1 ms) and cannot timestamp each individual CPU store exactly.

### Data captured locally (NOT committed into Git)

The project conversation had the following user-supplied CSV captures; do not imply that they exist in the Git repository.

| CPU cycles | File prefix | Probe coverage |
|---|---|---|
| 40,000 | `mk2_40000_20261109_191929` | PIT/VGA (earlier baseline) |
| 40,000 | `mk2_40000_20261109_200416` | PIT/VGA and frame-level visual changes |
| 20,000 | `mk2_20000_20261109_202724` | PIT/VGA and frames |
| 80,000 | `mk2_80000_20261109_202857` | PIT/VGA and frames |
| 40,000 | `mk2_40000_20261109_204156` | PIT/VGA, frames + 8×5 region grid |
| 40,000 | `mk2_40000_20261109_214037` | PIT/VGA, frames + regions, VRAM write counters |
| 40,000 | `mk2_40000_20261109_220957` | PIT/VGA, frames + regions, VRAM, VGA register timeline |
| 40,000 | `mk2_40000_20261110_124505` | **All five streams**, including scanmem boundary hashes; findings verified in this document |

A future researcher should ask for these files again if necessary. All files are private experimental data; *do not claim to possess them in Git*. Each prefix denotes its own session, often with different fight/menu durations, so do not align different sessions by absolute emulated time.

## Results: directly verified from the supplied CSVs

### Timers

- The DOS version of MK2 **reprograms PIT channel 0 to divisor 19869**, approximately **60.052 Hz**, following an initial ~18.2 Hz period in DOS.
- VGA refreshing is approximately **70.09 Hz** (one emulated scanout about **14.268 ms**); IRQ0 and VGA are independent clocks.
- The frequency ratio is close to **6 IRQ periods : 7 VGA periods** over ~99.9 ms. That is a mathematical relationship, **not proof** of a particular MK2 game-loop design or actual logic-frame count.
- The overall emulated vs host-time windows were stable in the prior 40,000-cycle trace, but that does not eliminate individual host frame spikes.

### The 3–1–1–2 visual change pattern

- Many sequences of distinct **source images** were observed at intervals of **3, 1, 1, 2 VGA refreshes**, i.e. about **42.8 / 14.27 / 14.27 / 28.54 ms**. Four changes are distributed over seven refreshes (~100 ms), roughly **40 visibly different images per second** in those windows.
- The pattern was observed in the **20k, 40k, and 80k CPU-cycle tests**. This makes a pure CPU-starvation explanation less likely, but the fights were not frame-perfect repetitions, so it does **not** prove all scenes perform equally across cycles.
- Some other windows exceed 50 distinct source-image changes per emulated second. Thus **40 fps is not a global fixed MK2 visual-frame cap**.
- Even a source-image change is not necessarily a fighter frame; it can be HUD, background, partial scanout or colour animation.

### Regional 8×5 visual changes

- The 320×200 display is divided into 40 cells (8 columns × 5 rows), each roughly 40×40 pixels.
- Different steps of the recurring 3–1–1–2 cycle often affect **different parts of the image**, not the entire screen. This supports investigating asynchronously changing parts of the display, but the region grid alone cannot separate drawing, palette animation, scanline race or simulation.
- The frame probe's `pixel_format` for mode 320×200 was `32` in the collected samples (the **source as exposed to this renderer is already 32-bit BGRX**, although guest VGA uses indexed colours). Consequently, `pixels_changed` can reflect both changes of guest indices and palette-dependent RGB output.

### VRAM write bursts

- In the 40k sample `20261109_214037`, the VGA handler intercepted millions of guest memory-write operations, strongly clustered into bursts; some bursts followed IRQ0 by a small emulated interval.
- At least some irregular cadence exists in **VRAM write activity before the completed image is presented**. Hence changing the output refresh alone should not be assumed a complete fix.
- **Crucial caveat:** The counter counts callbacks via VGA page handlers. Certain direct-mapped memory accesses bypass those callbacks. Even `page_handler_covered=1` does not prove every possible write to video memory was captured.
- A write is not proof that a visible pixel changed; writes can repeat the same values, target offscreen memory, or finish before the scanout samples them.
- Any apparent mismatch between VRAM writes and changed images is an *unresolved observation*, not proof of a defect.

### CRTC / panning / palette (verified against 20261109_220957)

The `20261109_220957` session contains:

- **8,055** completed source scanouts and **3,030** distinct source images over roughly **115 seconds**.
- In 320×200 mode, **7,601** completed scanouts with **2,998** `changed=1` rows.
- The `_regs.csv` contains **50,227** register/refresh/scanout events, including **17,921** `dac_rgb_commit`, **12** `crtc_start_write`, and **3** `pel_pan_write` events across the full session.
- In a typical graphics interval 30,000–110,000 emulated ms, no new `crtc_start_write` or `pel_pan_write` events occurred. The refresh/scanout snapshots show programmed/latched **display start = 0** and **panning = 0**. That rules out *these particular register changes* as an explanation during that interval, not all display-addressing effects.
- There is substantial DAC activity, including repeated changes in certain high palette indices. Prior exploratory correlation noted DAC changes repeating at ~99.876 ms (seven VGA refreshes); this exact cadence should be re-validated by script if important to a proposed fix.

**Unexplained image changes:** Re-evaluating this same capture's 2,997 changed 320×200 frames with a simple *previous-to-current complete-scanout* interval join gives the following approximate classification:

| Observed VGA handler writes in interval | DAC entry commits in interval | Changed source images |
|---|---|---:|
| yes | yes | 977 |
| yes | no | 914 |
| no | yes | 357 |
| **no** | **no** | **749** |

These classifications are **approximate**: VRAM writes are aggregated at 1 ms boundaries, while DAC events and completed frames are sub-ms. An earlier in-chat analysis with slightly different bucket boundaries reported **748** cases with neither recorded event. The one-frame difference underscores the boundary ambiguity. Do not label these cases as impossible changes: buffering between scanlines, incomplete write coverage, other VGA state or persistence of earlier draws could explain them.

## New experiment: scanout memory-boundary fingerprinting

A new opt-in `*_scanmem.csv` file has been implemented on the branch. **Not yet validated on Windows.**

For each successful frame scanout, it:

1. Samples at **the start of the scheduled scanline work**, after the VGA draw-source pointer is selected.
2. Samples again at **the completed, non-aborted scanout**.
3. Hashes the **raw VGA memory backing store** (`vga.mem.linear`, up to 256 KiB), the **fast rendering memory cache** (`vga.fastmem`, up to 128 KiB), and **the effective VGA DAC palette** (`vga.dac.palette_map`, every colour in 24-bit RGB).
4. References the **already computed source-image hash** from `*_frames.csv` rather than recalculating the rendered-image hash.
5. Records changes **during the scanout** (start to end) and **between adjacent scanouts** (previous end to current start), including mode, draw-source kind, active display-start/panning, and byte-coverage fields.
6. Skips aborted frames and synthetic pause-time repaints; it has zero CSV output unless the same environment variable enables tracing.

Fields include `raw_changed_during`, `fast_changed_during`,
`palette_changed_during`, `raw_changed_between`,
`fast_changed_between`, `palette_changed_between`, the scanout index
and `visible_image_hash`.

**Important limitations:**

- These are **two whole-memory fingerprints per scanout**, not a video-frame capture at every scanline. If memory changes and changes back between samples, a hash will not reveal the transient modification. A hash may change due to offscreen writes.
- The raw and fastmem caps may miss display data in higher SVGA banks. Check `begin_raw_bytes`, `begin_fast_bytes`, `begin_source_kind` and VGA mode. The raw/fast storage is not equivalent to the final source image in 32-bit colour.
- `vga.fastmem` can be the selected renderer source in some modes; comparing only `vga.mem.linear` would be insufficient.
- The effective palette hash detects *palette state*, not whether a changed DAC entry was actually used in the image. The existing per-source-frame hash answers the visual question.
- This sampling incurs read-only memory bandwidth and computation in the trace-enabled code path. Keep the no-trace control.
- A changed completed source image with unchanged boundary hashes is **not yet proof** of a mixed old/new scanout: writes may be unobserved, transient, or performed outside the sampled span. Do not patch clocks based on that fact alone.

### How to analyze the next run

1. Compile branch `mk2-timing-research` with the command above. A compile-only check has **not** yet been completed for the new probe.
2. Start a 40,000-cycle run and collect ~60–90 seconds of gameplay, including moving fighters and a short static interval.
3. Collect **all five CSVs**: base `.csv`, `_frames.csv`, `_vram.csv`, `_regs.csv`, and **`_scanmem.csv`**.
4. Verify that `_scanmem.csv` is nonempty, `scanout_index` matches `_frames.csv`, and `visible_image_hash` agrees with `visible_hash` after joining.
5. For changed 320×200 images with no observed VRAM writes or DAC commits, categorize by `raw_changed_during`, `fast_changed_during`, `palette_changed_during`, plus their `*_between` flags.
6. Check changes occurring in the same scanout interval against `region_mask`, `crtc_start_write`, `start_latch`, `pan_latch` and palette events.
7. If no clear cause emerges, consider **per-scanline** timestamped state sampling in a narrowly scoped subset of frames rather than globally increasing logging volume.
8. Only after attributing changes should the experiment evaluate possible timing modifications. Preserve gameplay logic, input responsiveness and original animation pacing.

## Follow-up capture 2026-11-10 — five-stream scanout boundary verification

**Status: the new `_scanmem.csv` instrumentation builds and runs on the
user's Windows computer.** The user supplied all five generated CSV files,
with prefix `mk2_40000_20261110_124505`. The files are **not in GitHub**;
the following calculations were made from those uploaded CSVs. Keep the
original CSVs if later independent replication is desired.

### Integrity and sampling coverage

- Full trace: approximately **149.9 s emulated**; 10,505 completed
  `_frames.csv` scanouts, 3,678 changed source images.
- Of these, VGA graphics mode **320×200**, `pixel_format=32`, accounts
  for **9,975 scanouts and 3,642 changed source images**.
- The other 530 scanouts were 720×400 text mode with 36 changed images.
- All **10,505** `_scanmem.csv` rows align 1:1 with `_frames.csv`:
  scanout indices, completion timestamps (`end_ms=emulated_ms`),
  `visible_changed=changed`, and `visible_image_hash=visible_hash`
  all match exactly. This is a positive integration check, **not** proof
  of a fully accurate scanout model.
- In all sampled 320×200 frames, `begin_vga_mode=8` (`M_VGA`);
  `begin_source_kind=2` (**`vga.fastmem`**). Both the raw backing store
  and fastmem were valid. The probe hashed **262,144 raw bytes** and
  **131,072 fastmem bytes** at each boundary. Full relevant mode
  coverage is plausible, but the actual active memory addressing must
  still be checked before claiming it is complete.
- Across those graphics-mode samples, the raw and fastmem **change flags**
  agree for each 'during' and 'between' category. Their hash values are
  different because they represent different layouts.
- `_vram.csv` includes **4,276,922 intercepted write operations** across
  the full session; 4,268,122 are 32-bit operations in covered VGA mode,
  with the rest predominantly startup text-mode writes.
- The register trace has **19,915 `dac_rgb_commit` events**.
  The base `_csv` has 149 one-second summary rows; the shorter summary
  duration than 149.9 s reflects its final partial second not being
  emitted in that stream.

### Classification of the 3,642 changed VGA graphics-mode source images

These categories are **mutually exclusive**, processed in this order:
(1) backing VRAM changed within the sampled scanout;
(2) backing VRAM changed only between scanouts;
(3) effective palette changed only between scanouts;
(4) no detected net change in any sampled backing buffer or palette.

| Boundary signature | Changed source images | Share |
|---|---:|---:|
| Raw VGA/fastmem changed **during** scanout | **1,519** | 41.7% |
| Raw VGA/fastmem changed **between** scanouts, but not during this one | **308** | 8.5% |
| **Only effective palette** changed between scanouts | **1,060** | 29.1% |
| **No difference** in sampled raw/fastmem/palette boundary hashes | **755** | 20.7% |
| **Total** | **3,642** | **100%** |

The effective-palette change is much more frequent **between** scanouts
than **during** them: among 320×200 frames, 1,744 had
`palette_changed_between=1`, while only **1** had
`palette_changed_during=1`. Some of these did not produce different
visible images (for example, changes to unused palette entries).

**Caution:** These classes describe boundary-state differences, **not**
proof that a particular state change caused the displayed image change.
`vga.fastmem` was the source of the VGA scanout in this mode. A write to
raw backing memory and its corresponding fastmem update can be the same
guest drawing operation, not independent work.

### Rechecking the previously unexplained image changes

For each of the **3,642** changed graphics frames, join the previous
completed-scanout timestamp and current one, and check observed VRAM
write buckets and `dac_rgb_commit` timestamps in that interval.
A boundary-sensitive join that counts write buckets with
`previous_end < emulated_ms_end <= current_end` yields:

| Observed VRAM writes | DAC RGB commits | Frames | With any sampled memory/palette state change | Without |
|---|---|---:|---:|---:|
| No | No | **696** | **0** | **696** |
| No | Yes | **1,059** | 1,059 | 0 |
| Yes | No | **1,332** | 1,273 | **59** |
| Yes | Yes | **555** | 555 | 0 |
| **Total** | | **3,642** | **2,887** | **755** |

The 696 cases belong to **this 2026-11-10 capture**, and should not
be equated with the approximately 748/749 no-event cases in the **different
2026-11-09 capture**. These are separate gameplay sessions.

The **696** images with neither intercepted write operations nor DAC RGB
commits in the preceding scanout interval also had **no net change** in
sampled VRAM, fastmem or effective palette **during or between** the
two scanouts. A further 59 changed images did have write callbacks but
the sampled boundary state was unchanged.

This is the most important unresolved result:

> We can no longer explain those 696 changed source images merely by
> postulating a **persistent change** to one of the sampled backing
> buffers or effective palette that our write counter missed.
> However, we **cannot** infer the absence of intermediate writes,
> transient states, scanline mixing, other VGA control state,
> or renderer/cache effects.

The source-frame regional masks for the 755 boundary-invariant changes
are not random; common patterns include `6666660000` (111 times),
`0000000010` (89), and `c0c0c00000` (45). In 565 of those 755
cases, the interval from the **previous changed source image** is
approximately one 70 Hz VGA refresh (~14.3 ms), but others are separated
by two or more refreshes. The repeated, localized masks justify targeted
inspection of the line scanout and renderer rather than blindly changing
PIT/VGA frequencies.

### Interpretation and next controlled experiment

The scanmem hook samples after the frame's VGA drawing source has been
selected, before the scheduled scanline work, and once after completing
that scanout. The sample interval is around **12.711 ms** in the stable
graphics mode, while the vertical refresh period is **14.268 ms**.
This brackets most of the picture but **does not sample every line**.

Possible mechanisms for the 696 cases include:

1. **Transient modifications** to VRAM that are restored before the
   next boundary sample (either in a visible or hidden bank).
2. Source lines read from VRAM at **different moments** during a scanout,
   producing a mixed/stale source image, even if the two boundary
   snapshots are equal.
3. Other VGA draw-state changes not fully covered by the CRTC/panning
   and palette trace (including scanline-specific state).
4. **Renderer/scaler cached-line behavior**, overlay or other host-side
   changes that modify the source cache independent of stable VGA memory.
5. The possibility of misinterpreted **1 ms write bucket boundaries**;
   individual guest write timestamps are not available for each store.

Next, trace **specific scanlines or sparse frame samples only during
short windows** around repeatedly occurring masks, not all lines for
the full game. For each targeted line record:

- The precise emulated time its draw handler runs.
- Its display-memory address / split-line state and pixel format.
- Its **raw/fastmem source-line hash immediately before drawing**, plus
  the produced renderer source-line hash after drawing, where feasible.
- Whether a source line differs from the previous completed scanout
  while its source backing pixels and effective palette still match.
- Optional granular sampling of actual guest VRAM writes to that line
  in just the selected short time window.

Compare these samples with the `*_frames.csv` `region_mask` and the
existing `*_regs.csv`, avoiding timing or guest-program modifications.

**Do not conclude that the 696 cases are emulation bugs without the
targeted line-level evidence.** We still have not measured actual
MK2 simulation ticks.

## New experiment: sparse line-level VGA/renderer trace (2026-11-10)

**Implementation committed on `mk2-timing-research`; not yet compiled or
runtime-validated on the user's Windows machine.**

The new `src/hardware/mk2_scanline_probe.h` is called immediately before
`VGA_DrawLine()` reads a source scanline and immediately after it returns
its 320×4-byte RGB line; at frame completion we compare with the
corresponding row of the `RENDER_EndUpdate` source latch.

Collection is restricted to the **320×200 M_VGA** mode seen in the test.
The probe does not touch MK2 game files, emulated timing, IRQs, VRAM, the DAC
or resulting pixels. It captures a source-index-line fingerprint and effective
palette fingerprint when the line is drawn, the VGA-produced BGRX line
fingerprint, and the renderer source-cache line fingerprint on completion.
The hook records the **actual draw address and masked source offset** and
**fractional emulated PIC time**.

The sixth file is `mk2_40000_<timestamp>_lines.csv`; it is activated by
the **existing** `DOSBOX_MK2_TRACE` environment variable, with no new launcher
flag. To limit observer effect, sampling happens **only** in emulated seconds
40, 60, 80, 100 and 120 (one-second windows each). During the windows all
200 source lines of eligible full scanouts are recorded. Per-line records
are buffered in RAM and written after the frame completes, not on the VGA
line events.

**Fields of interest:** `scanout_index`, `line`, `frame_begin_ms`,
`line_sample_ms`, `frame_complete_ms`, `draw_address`,
`masked_offset`, `source_kind`, `source_changed`,
`palette_changed`, `output_changed`, `cache_changed`,
`output_equals_cache`, and the four hashes. The first sampled frame
of each one-second window has `previous_valid=0` (baseline).

### What the next capture should determine

1. When `*_frames.csv` reports a new visible image but `_scanmem.csv`
   sees unchanged boundary states, do **particular source-index lines**
   differ at the instant VGA reads them, even though start/end whole-VRAM
   hashes agree?
2. When source indices are unchanged, did effective palette entries change
   between sampling particular lines? A whole-palette hash can change for
   unused colours, so compare against observed output-line changes.
3. Is `vga_output_hash` different from the latched
   `renderer_cache_hash` in any line? That would identify a discrepancy
   between VGA output and renderer source-cache retention (the probe is
   before ReelMagic insertion and software scaling; interpret cautiously).
4. How do changed lines map to the 8×5 `region_mask` for the same
   `scanout_index`? Example repeated masks in the previous capture were
   `6666660000`, `0000000010`, and `c0c0c00000`.
5. Does a line become visible with different RGB values despite an
   unchanged sampled source index and palette? That would motivate a more
   targeted trace of graphics register state during line drawing.

### Test and handoff

```powershell
cd "$env:USERPROFILE\Documents\dosbox-staging"
git switch mk2-timing-research
git pull --ff-only
$env:VCPKG_ROOT = "C:\vcpkg"
cmake --build --preset=release-windows --parallel 8
```

Use the same `Launch-MK2-Timing-Direct.ps1` script at 40,000 cycles,
and play long enough to overlap at least the 40 s, 60 s and 80 s
sample windows. Upload all **six** CSVs for the same prefix.

First validate the `_lines.csv` schema, confirm lines 0–199,
sampled `scanout_index` values are a subset of `_frames.csv`, and
check typical frame row coverage. If `_lines.csv` is empty or frames
lack rows, **investigate source mode/line dimensions before interpreting
absence of changes as evidence**. Treat trace overhead as a limitation
and use a trace-disabled control if necessary.

The previous five streams are already validated; this sixth stream
alone requires build/runtime validation. No conclusions about MK2 game
simulation FPS or a fix to clocks should be made from instrumentation
until the line-level data are analyzed.

## Follow-up capture 2026-11-10 — six-stream line-level results (**new verified finding**)

The user built DOSBox Staging on Windows at **13:08:33 local time** and
successfully launched MK2 with **40,000 protected-mode cycles**. Exit code was
0. Uploaded files all share the prefix
`mk2_40000_20261110_131108`:
`.csv`, `_frames.csv`, `_vram.csv`, `_regs.csv`,
`_scanmem.csv`, `_lines.csv`, plus
`launch_20261110_131108.txt`.
**Raw CSVs and game files are not committed to GitHub.**

### Six-stream integrity and coverage

| Measurement | Verified result |
|---|---:|
| PIT/VGA summary windows | 163 completed emulated seconds |
| Total completed scanouts in `_frames.csv` | **11,434** |
| Total source-image changes | **4,206** (4,175 in 320×200; 31 in 720×400) |
| 320×200 VGA scanouts | **10,985** |
| 720×400 scanouts | 449 |
| `_scanmem.csv` rows | **11,434**, exactly matching every frame index, completion timestamp, change flag and visible-image hash in `_frames.csv` |
| `_lines.csv` rows | **70,000** |
| Per-scanline sampled frames | **350**, each with all **200 rows**, with 70 frames in each one-second window |
| Sampled windows (emulated seconds) | **40, 60, 80, 100, 120** |
| Source read mode for every sampled line | **2: `vga.fastmem`**, mode `M_VGA` |
| Draw source addressing | **`draw_address = masked_offset = line × 320`** in **all 70,000 samples**, ranging from 0 to 63,680 |
| VGA line output vs renderer source-cache hash | **70,000 / 70,000 identical** |
| VGA writes intercepted across full run | **10,192,885** operations |
| DAC RGB commits in register stream | **21,773** |

The source-cache comparison uses a 64-bit hash, not a byte-for-byte
external GPU capture. Equality across 70,000 corresponding line hashes is
strong evidence against corrupt or mis-retained renderer source lines in
the observed windows. It cannot verify GPU/compositor presentation, and
hash collisions are theoretically possible.

Each of the five sampling windows has 70 complete 320×200 frames; their
visually different frames number **34, 49, 27, 22 and 31**, respectively.
The comparison baseline (first sampled frame in each window) has
`previous_valid=0` and must be excluded from change-cause statistics.

### Crucial controlled line-level comparison

Across the **69,000** individual line rows having `previous_valid=1`:

| Source-index bytes changed? | Effective palette hash changed? | VGA RGB line changed? | Rows |
|---|---|---|---:|
| No | No | No | **48,023** |
| No | Yes | No (palette change not visible in this line) | **6,718** |
| No | Yes | Yes | **565** |
| Yes | No | Yes | **9,377** |
| Yes | Yes | Yes | **4,317** |
| **No** | **No** | **Yes** | **0** |

The output of the VGA RGB line and its latched source-cache line also
has **identical change flags in all rows**. In other words:

- **14,259** of these 69,000 comparable lines visibly changed.
- Every visible line change is accompanied by at least one detected change
  to the indexed source line and/or effective palette at its draw time.
- Only **565** changed lines had unchanged source indices with a changed
  effective palette; palette cycling is real, but does not explain all
  motion.
- **6,718** line comparisons observed palette state changes that did **not**
  change RGB output. This shows why a palette update alone does not imply a
  displayed image change.
- No discrepancy in sampled VGA-produced RGB versus the renderer cache was
  found. Accordingly the earlier hypothesis of a corrupting
  source-cache stage lacks supporting evidence in this test.

### Resolving the earlier “changed image with no writes” ambiguity

The **2026-11-10 13:11:08** session, *separate from* the earlier 12:45:05
session, has 4,175 changed 320×200 frames. Among them:

- **885** changed frames have no *net* start/end or inter-frame change
  in the sampled raw VGA memory, fastmem or effective palette boundary
  fingerprints.
- **837** of these have **zero** observed VGA page-handler write operations
  and **zero** DAC RGB commits in the completion-to-completion interval,
  according to `_vram.csv` (1-ms buckets) and `_regs.csv` timestamps.
- **All 837 of these 837** follow a scanout that reported
  `fast_changed_during=1` and `raw_changed_during=1` in
  `_scanmem.csv`. The preceding completion-to-completion interval also
  contained nonzero intercepted VGA writes **in every one of the 837
  cases** (median **1,363** write operations, minimum 8).
- **32** of these 837 cases fall in one of the five sparse scanline
  windows. **All 32** had source-index line changes
  (`source_changed=1` on affected lines), **no effective palette hash
  change** relative to the previous sampled frame on those lines, and
  RGB line changes that match the retained source-cache lines.
- Sample case: at scanout **2834** (40,463.6731 ms), 21 source lines
  changed and the `region_mask` was `0000000010`. There were no
  observed VGA write operations or DAC commits since the preceding
  scanout completion, and no *net* current boundary state changes,
  **but the preceding scanout had changed VGA memory during drawing**.
- Another case: scanout **4206** (60,039.4571 ms) changed 193 source
  lines and all 40 image regions; likewise it followed a scanout with
  VRAM changes.

This establishes a **consistent, directly observed temporal sequence**:

1. While VGA progressively draws scanout **N**, the game writes VRAM.
2. Some lines in N have already been read, so N's completed RGB source
   image can still contain older line contents.
3. Scanout **N+1** reads the same VRAM areas later, exposing those
   already-written pixels, **even with no new writes in the interval since
   N completed**.
4. The full raw/fastmem hashes may remain unchanged over the
   start/end boundaries of N+1, because those writes occurred during N.

The pattern is not proof that *every one* of the 837 cases was caused
exclusively by a read-after-write interleave; source-line hashes are
sampled sparsely and other VGA state can exist. However, the complete
preceding-scanout correlation and the 32/32 line-level confirmations
strongly favour **ordinary progressive scanout visibility of earlier
writes** over unexplained pixel generation or a renderer-cache defect.

**This supersedes the earlier interpretation that boundary-invariant
source-image changes necessarily suggested mysterious writes, a
corrupted renderer cache, or transient memory restoration.** They are
consistent with memory mutations that were captured *in the previous
scanout*, but not yet visible in its earlier-rendered lines.

### What remains open

- We have **not** measured MK2's actual simulation loop/update ticks,
  and a different source image does not equal a fighter animation update.
- The recurrent 3-1-1-2 visual change cadence at all CPU-cycle settings
  may be a combination of **~60.052 Hz PIT, ~70.09 Hz VGA, nonuniform
  game rendering bursts, palette cycling and progressive scanout**.
  The present data do not assign exact weights to these contributors.
- Hardware-faithfulness of the scanout model, actual input response
  latency, and final GPU presentation timing remain unverified.
- Avoid changing PIT, refresh or scaler settings merely to turn output
  into equal interval gaps. Doing so could break original timing.
- A future worthwhile experiment is a *targeted guest simulation-update
  counter* or comparison to reference execution/hardware, rather than
  expanding general VGA logging indefinitely.
- Continue comparing tracer-on and tracer-off host performance to
  quantify measurement overhead.

## Seventh stage: guest execution-context candidate discovery (2026-11-10)

**Status: implemented on `mk2-timing-research`, not yet compiled or
runtime-tested on Windows.** The six existing trace streams remain in
place. No change to the original MK2 executable, video clocks, PIT or
guest memory has been made.

Our six-stream capture `mk2_40000_20261110_131108` closed a substantial
graphics-side question: 70,000 / 70,000 VGA RGB line hashes matched their
latched source-cache counterparts. The 837 changed images with no observed
VRAM write/DAC commit since their previous completion all followed VRAM
changes during the **preceding** scanout. 32 / 32 directly sampled cases
showed changes in the source indices at line-read time. These observations
do not identify MK2's **simulation tick** or fighter animation update rate.

The next stage adds the new opt-in `_guest.csv` stream. It collects saved
guest CPU registers and segmented `CS:EIP` context at five event types:
(1) just before IRQ0 delivery, (2) the first intercepted VRAM write in
each 1-ms bucket, (3) VSYNC-low-to-high reads of VGA status port 3DAh,
(4) every 2048th port 3DAh read, and (5) independent 10-ms scheduler
samples. Sampling is restricted to **30–150s emulated time**. The
seventh probe uses a fixed-size memory buffer and batches CSV writes,
so it avoids file I/O in video/CPU hot callbacks.

Files implemented/changed:

- `src/hardware/mk2_guest_probe.h` — new guest context sampler and CSV
  writer; output columns include `event`, emulated timestamp, CS selector,
  CS base, EIP, mode/CPL, VGA status and EAX/EBX/ECX/EDX/ESI/EDI/EBP/ESP.
- `src/hardware/pic.cpp` — sample interrupted code immediately before
  IRQ0 delivery, plus 10-ms scheduler samples.
- `src/hardware/mk2_vram_probe.h` — sample context on first VGA page
  handler write in a millisecond bucket.
- `src/hardware/video/vga_misc.cpp` — sparse sampling of 3DAh polling
  sites, reusing an existing emulated timestamp.
- `scripts/analyze_mk2_guest_trace.py` — stdlib Python summary of
  frequently sampled segmented code addresses by event class.

**These observations are NOT simulation-tick counters.** `CS:EIP` inside
an emulator callback may be imprecise/stale under a dynamic CPU core. IRQ0
pre-dispatch is *interrupted* code, not the interrupt handler's entry.
Polling/VRAM write sites may be in loops or copy routines rather than
game-state updates. A 60-Hz IRQ rate or recurring hot address cannot
establish simulation speed without identifying MK2 code and verifying
specific state transitions.

### Required next step

1. Build latest `mk2-timing-research` on Windows with the existing
   `release-windows` preset and local vcpkg root.
2. Run MK2 at 40,000 cycles at least **160 emulated seconds**, preferably
   with live combat in the 30–150s window, including some static moments.
3. Collect **seven** CSV files (base, `_frames`, `_vram`, `_regs`,
   `_scanmem`, `_lines`, and **`_guest`**) plus the launch log.
   The file `_lines.csv` still only samples seconds 40,60,80,100,120.
4. Run
   `python scripts/analyze_mk2_guest_trace.py "<path-to-_guest.csv>"`
   and attach the textual output with the seven CSVs.
5. Identify candidate code locations by overlap of VRAM drawing,
   recurring 3DAh polling, IRQ preemption and independent 10-ms samples.
   Separate real-mode DOS startup from protected-mode gameplay.
6. **Only after disassembly or a targeted watchpoint confirms a genuine
   guest logic-update site**, consider a narrowly scoped counter at that
   site and compare logic tick intervals against PIT, VRAM, image changes,
   and input response timing. Do not alter timing to force desired FPS.

## Seventh-stage first results: sampled MK2 guest code locations (2026-11-10)

**Data source:** user-provided stdout from
`scripts/analyze_mk2_guest_trace.py`, input
`mk2_40000_20261110_133415_guest.csv` (raw CSV not attached in this
turn and **not stored in GitHub**). This is a *summary of selected CPU
contexts*, not a complete dynamic trace or disassembly.

### Sample totals and prominent execution contexts

- 30,385 context samples collected; 30,328 in protected mode and 57
  in real mode (the 120-second 30–150s sample interval includes some
  DOS or BIOS activity).
- Of 7,207 `irq0_pre` samples (about **60.06/s** over the ~120 s
  sampling window), 7,097 (98.5%) had `CS:EIP =
  0180:00205489`, `cs_base=0`. Another 87 (1.2%) had
  `0180:002196D5`. This captures **interrupted** code; it does not
  locate IRQ0's handler or the simulation update.
- All **1,473** `vga_status_rising` and all **5,029**
  `vga_status_sample` events reported
  `0180:002196D5` with CS base 0. Median spacing of
  VSYNC rising samples was **99.876 ms**, matching seven approximately
  70.09-Hz VGA periods; this is compatible with regular polling of
  retrace, not proof of a 10-Hz simulation step. Number of observed
  rising samples across the window is greater than 1 per 100ms: the
  median alone should not be treated as the global frequency.
- **4,676** `vram_first_write_ms` samples concentrated in code
  pages `0180:00204200` through `0180:002049FF`;
  prominent exact saved positions included `002042FC` (587
  samples), `0020482A` (556), and `0020452A` (442).
  Median recurrence within individual code *pages* varied (about
  33, 59, 67, 99, 100, and 199 ms). Because samples are the
  **first observed VGA write in a 1-ms bucket** and pages aggregate
  many actual offsets, these are not render-frame intervals or
  evidence of an intrinsic 10-Hz engine.
- In **12,000** independent 10-ms scheduler samples,
  `0180:00205489` appeared 7,923 times (66.0%),
  `0180:002196D5` 1,041 times (8.7%),
  the page `0180:0020C800` 733 times (6.1%),
  and `0180:00208700` 375 times (3.1%).
  Other code pages may be animation, logic, audio, resource management
  or helper routines; they **must be verified** before labelling.

### Tentative roles and what we have NOT proved

| Guest CS:EIP / page | Supported observation | Working hypothesis |
|---|---|---|
| `0180:002196D5` | Captured at every sampled 3DAh read | Port-3DAh polling or retrace-wait routine |
| `0180:00205489` | Dominates interrupted and periodic CPU samples | Idle/wait location, or stable EIP retained at dynamic-core block boundary |
| `0180:00204200–002049FF` | First intercepted VGA page-handler writes | Pixel drawing, screen compositing, copy/fill routine(s) |
| `0180:0020C800`, `0180:00208700` | Recurrent periodic samples; no guest disassembly | Unknown code; possible candidates for further investigation |

**Critical qualification:** DOSBox Staging's CPU configuration documents
`core=auto` as selecting **dynamic core in protected mode**. The dynamic
core translates instructions in blocks, so the `reg_eip` saved in
callbacks (VGA I/O, VRAM write, pre-IRQ) might refer to a translated
block's cached or entry IP instead of the exact guest instruction
responsible for the event. The report above used
`cs_base=00000000`, but it does **not** establish how a guest linear
address maps to offsets within MK2's executable on disk. There has
been no static disassembly and no simulation-state watchpoint.

### Recommended controlled validation before a guest logic counter

Use **two short diagnostic captures** of the same game action,
preferably at 40,000 cycles:

1. With the existing `core=auto`, keep the sampled PC clusters as a
   baseline.
2. Temporarily run with DOSBox Staging `--set core=normal` to interpret
   guest instructions individually, not from dynamic translations.
   The normal core can be significantly slower than real time;
   this run is **only for locating candidate code**. Do not use its
   frame pacing or wall-time metrics as an equivalent benchmark.
3. Capture at least some gameplay inside the already defined
   **30–150s** guest-probe window and rerun
   `scripts/analyze_mk2_guest_trace.py`. Look for whether
   `0180:002196D5`, `0180:00205489` and the VGA-write
   code pages remain recognizable or shift to adjacent instructions.
4. Disassemble candidate *guest* code with its actual DOS extender and
   load mapping. Do not assume `EIP=002196D5` is a raw file
   offset. Before a targeted counter is meaningful, confirm a
   specific routine updates a game-state variable or animation state.
5. Only then instrument confirmed guest state-transition boundaries
   (e.g. a fighter-position/animation-step update) and measure the
   intervals relative to PIT/VGA. A merely frequent PC or IRQ0
   occurrence is **not a game logic tick**.

The findings section and handoff should be updated again once an actual
normal-core or disassembly check is performed.

## Normal-core verification and next local guest code snapshots (2026-11-10)

**Evidence source:** User pasted the output of
`scripts/analyze_mk2_guest_trace.py` for
`mk2_normal_40000_20261110_134308_guest.csv` from a Windows MK2 run
using `core=normal`. This is a different run from the earlier
`mk2_40000_20261110_133415_guest.csv` dynamic/auto-core capture.
**Neither raw CSV nor game binaries are committed.** Numbers below are
the analyzer's output, not direct examination of instruction bytes.

### Dynamic-core vs normal-core results

| Statistic | Dynamic / auto | Normal interpreter |
|---|---:|---:|
| Guest context samples | 30,385 | **29,519** |
| Protected-mode samples | 30,328 | **29,446** |
| IRQ0 pre-dispatch samples | 7,207 | **7,076** |
| IRQ0 pre-dispatch in EIP page `00205400` | 7,097 (98.5%) | **6,963 (98.4%)** |
| VGA 3DAh status rising samples at `0180:002196D5` | 1,473 / 1,473 | **1,531 / 1,531** |
| VGA 3DAh periodic samples at `0180:002196D5` | 5,029 / 5,029 | **5,402 / 5,402** |
| First-intercepted-VRAM-write samples | 4,676 | **3,726** |
| Independent 10-ms samples | 12,000 | **11,784** |

The four exact `core=normal` EIP positions within the interrupted
CPU's most common code page are:

| EIP | Normal-core IRQ0 pre-dispatch hits |
|---|---:|
| `0180:00205489` | 1,774 |
| `0180:0020548F` | 1,670 |
| `0180:00205492` | 1,766 |
| `0180:00205494` | 1,753 |
| **Total** | **6,963** |

The four EIPs are clustered within 11 guest bytes, with similar
occurrence counts, supporting an **extremely tight, frequently
interrupted execution loop** or short cycle of instructions. A direct
instruction decode remains necessary to determine its branch
conditions and whether it is idle, polling, game-state or input-related.
It is **not yet identified as the simulation loop**.

The VGA polling address `0180:002196D5` is identical in both runs,
in *all* sampled 3DAh read events. This is a strong candidate for an
I/O read instruction or polling routine. The median time between
recorded status-rising observations remains **99.876 ms**, but
the 1,531 samples over ~117.77 seconds cannot be reduced to “10 Hz”
without examining all interval gaps and how often the guest actually
polls. The VGA hardware clock remains ~70.09 Hz.

The normal interpreter's leading write-site samples include
`0180:0020479A` (517), `0180:0020461A` (514),
`0180:002042FC` (297), `0180:002045A2` (154),
and `0180:002045EA` (141), all inside or adjacent to the
`00204200–002049FF` graphics code span observed earlier. Their
intra-run frequency is **not** a simulation-tick frequency.

The normal-core 10-ms scheduler samples still spend 66.2% of samples
in page `00205400`. Other sampled pages include `0020C800`,
`0020A300`, `0020A400`, and `00208700`, with no confirmed
function identity. Normal-core samples are for *code location
validation*, not FPS or real-time speed comparisons.

### New targeted experiment: small in-guest code snapshots

**Implemented on branch, not yet Windows compiled/runtime-validated.**

The modified `src/hardware/mk2_guest_probe.h` now records **at most
one 64-byte code snapshot per verified site**, starting at the
observed protected-mode guest EIP. Candidate sites:

| Label | Expected guest EIP |
|---|---|
| `irq_wait_loop` | `00205489` (trigger from IRQ0 pre-dispatch at 5489/548F/5492/5494) |
| `vga_3da_poll` | `002196D5` |
| `vram_draw_479a` | `0020479A` |
| `vram_draw_461a` | `0020461A` |
| `vram_draw_42fc` | `002042FC` |
| `periodic_c81e` | `0020C81E` |

When enabled by the existing `DOSBOX_MK2_TRACE` variable, the same
guest CPU context hooks now take these tiny read-only snapshots from
the **running user's own DOS environment** (selector 0180, CS base 0,
protected mode). They are buffered and flushed at most every emulated
second to a local **`_guest_code.csv`**. No game bytes are written
to the repository or distributed by this code change. The CSV may
contain copyrighted byte sequences from the user's own MK2.EXE:
**keep it private and do not commit it or the extracted .bin files**.

An additional local utility
`scripts/extract_mk2_guest_code.py` extracts each 64-byte hex field
to a private per-site .bin and prints
`ndisasm -b 32 -o <guest EIP> ...` and `objdump -D -b binary -m i386
-M intel --adjust-vma=<guest EIP> ...` examples. The starting EIP is
intended to be an instruction boundary in the normal interpreter;
disassembling earlier arbitrary bytes can yield misleading offsets.
Use `core=normal` for the first snapshot run; after validating the
instruction stream, dynamic-core runs can be compared.

**Limitation:** The read-only code snapshot is taken inside an
emulator event callback after matching an observed EIP. It describes
the guest memory contents at that event, not the original file layout
on disk. The selected instruction could be in a DOS extender, a
shared routine, or loaded/relocated code. Disassembly must interpret
calling context, branches and reference addresses; these bytes alone
do **not** prove a real simulation tick. The new capture has not been
compiled or runtime-tested in the user's Windows build.

### Next action

Build the current branch, run MK2 with **`core=normal`** and
`DOSBOX_MK2_TRACE` enabled during live gameplay after 30s emulated,
and inspect `*_guest_code.csv`. Some of the six candidate sites might
not be encountered during a particular short run, so fewer than six
rows are valid and not an automatic failure. Use the extractor locally
to produce short disassemblies, then **verify the loop logic and call
relationships before adding a game-state tick counter**. No changes
to PIT, VGA, IRQs, guest program or game assets are needed.

## Guest code snapshot extraction confirmed (2026-11-10, 14:06 capture)

The user successfully ran `scripts/extract_mk2_guest_code.py` on
`mk2_normal_40000_20261110_140646_guest_code.csv` after fixing a
PowerShell filename-construction error. The extractor reported
**six local binary snippets, 64 bytes each**, with guest start EIPs:

- `irq_wait_loop_00205489.bin`
- `vga_3da_poll_002196D5.bin`
- `vram_draw_42fc_002042FC.bin`
- `periodic_c81e_0020C81E.bin`
- `vram_draw_479a_0020479A.bin`
- `vram_draw_461a_0020461A.bin`

**This verifies extraction and snapshot availability only.** No
instruction bytes or decoded disassembly have yet been presented in
the chat. Do **not** infer particular instructions or logic tick
behavior from file names. The local snippet CSV and binaries contain
bytes from the user's legally owned MK2 installation, and are **not
committed to GitHub**.

Next: obtain a disassembly of these snippets, either by analyzing
a privately supplied `_guest_code.csv` or locally running
`ndisasm -b 32 -o 0x205489 <irq_wait_loop.bin>`,
`ndisasm -b 32 -o 0x2196D5 <vga_3da_poll.bin>`, etc. The two
highest-priority sites are the wait loop and the status-port read.
A 64-byte snippet only shows a limited window; decoding at its start
does not establish the full function call graph or simulation tick.
After decoding, verify with branch control flow and (if needed)
targeted further reads before instrumenting gameplay simulation.

### PowerShell filename correction

The original example using
`[System.IO.Path]::ChangeExtension($env:DOSBOX_MK2_TRACE, $null)`
left a trailing `.` in this PowerShell environment, yielding a wrong
`._guest_code.csv` path. The working expression is:

```powershell
$codeCsv = $env:DOSBOX_MK2_TRACE.Substring(0, $env:DOSBOX_MK2_TRACE.Length - 4) + "_guest_code.csv"

> MK2 DOS frame-timing research: GitHub repo `WildGer/dosbox-staging`, branch `mk2-timing-research`. Start with this findings document and `docs/mk2-timing-research.md`. Earlier phases measured PIT 60.052 Hz, VGA 70.09 Hz, scanouts, VRAM writes, palette cycling; 70,000/70,000 line hashes matched VGA and renderer cache, and 837 changed source frames without new writes were compatible with prior-scanout progressive VRAM sampling. Seventh phase guest `_guest.csv` samples were compared using dynamic and normal CPU cores. In normal the dominant 4 EIPs near 0x205489 were 0x205489, 0x20548F, 0x205492 and 0x205494; all VGA 0x3DA polls at 0x2196D5. **Six 64-byte guest memory snippets have now been privately supplied and actually disassembled.** At `0180:00205489` the game loads ESI from `[0x27177C]`, busy-waits on `[ESI+0x24] != 0`, saves the nonzero value to `0x275B0C`, updates `0x275B10`, resets `[ESI+0x24]` to 0 and branches to `0x2054FE` (outside snippet). The **writer/producer of this work-ready value is unknown**; we must identify it before claiming a game simulation tick. At `0180:002196D5` actual `in al,dx`, `test al,8`, backwards jump waits for VGA VSYNC, then conditionally programs palette DAC index 0 through `0x3C8/0x3C9`. Snippets `0x2042FC`, `0x20461A`, `0x20479A` are large unrolled conditional memory compare/copy patterns. The `0x20C81E` sample is byte arithmetic/table lookups, not a proven timer. Next: targeted normal-core watchpoint for guest linear address `uint32[0x27177C] + 0x24`, record writer CS:EIP and time; inspect code around writer and 0x2054FE; only then count confirmed game-state updates. No copyrighted game bytes in GitHub. Do not change PIT, VGA clocks, MK2 EXE, or gameplay timing.

**Do not assume the source CSVs are in the repository.** If necessary, upload all seven CSVs from the next run. Maintain this document when new evidence changes a hypothesis.
, '_guest_code.csv'
Test-Path "$codeCsv"
python scripts/extract_mk2_guest_code.py "$codeCsv"
```

The main instrumentation guide now uses PowerShell 5.1-compatible
`-replace` to avoid the trailing-dot issue and unsupported `-LeafBase` option.

## Six guest code snippets: confirmed x86 instruction analysis (2026-11-10)

**Source:** The user supplied the **six 64-byte, protected-mode memory
snippets** extracted from their own MK2 installation after a
`core=normal` run. Each snippet was decoded as 32-bit x86 with
`objdump -D -b binary -m i386 -M intel` using its actual guest EIP
as the origin. These exact bytes are user-private and are **not
copied or distributed in this repository**. The observations below are
from the actual machine instructions, not address-frequency guesses.

### A. `0180:00205489`: wait for a nonzero work/clock value

The first four instructions read a pointer from guest **DS-relative offset**
`0x27177C`, load a 32-bit value at `DS:[ESI+0x24]`, test for zero and
branch **back to `0x205489` while zero**:

```text
ESI = uint32[DS:0x27177C]
value = uint32[DS:ESI + 0x24]
if (value == 0) goto 0x205489
```

On nonzero, it performs these observable actions:

```text
uint32[DS:0x275B0C] = value
uint32[DS:0x275B10] =
    (uint32[DS:0x275B10] + (value << 1)) >> 1
uint32[DS:ESI + 0x24] = 0
ESI = 0x275B04
goto 0x2054FE
```

The code starts at `0x205489`, the nonzero path at
`0x205496`, the reset instruction at `0x2054AD`, and
the final forward branch at `0x2054B9`. The latter targets
`0x2054FE`, **outside the 64-byte snapshot**, so the downstream
logic is unknown.

**Directly demonstrated:** This is a **busy-wait / consume-and-clear**
mechanism for a 32-bit memory value, likely representing pending work
or timing signals. Because it can read any nonzero value, it cannot be
assumed to be strictly boolean or a one-per-frame flag.

**Not demonstrated:** What writes `DS:[uint32[DS:0x27177C]+0x24]`;
whether the writer is IRQ0, another interrupt, sound, a helper loop,
or combat-logic code; whether one nonzero read advances one fighter
simulation tick; or what subsequent code does after `0x2054FE`.

**Best next step:** identify the producer/writer of this exact
**dynamic DS-relative guest address** and record its transitions/timestamps
independently of the consumer. Do not increment a gameplay-tick
counter merely whenever this busy-wait loop exits.

### B. `0180:002196D5`: VGA vertical-retrace wait AND DAC update

The first instructions perform an `IN AL,DX`, test bit `0x08`,
and branch back to `0x2196D5` if the bit is clear. The caller's
DX was proven by the hook to be the VGA status port `0x3DA`.
Thus this **waits for vertical retrace**.

On success it tests bit `0x8000` of the 32-bit word at
the DS-relative offset `0x2B5CE8`, skips via a conditional branch to `0x21971F`
if set, and otherwise:

- Writes DAC palette-index **0** to I/O port **`0x3C8`**;
- Changes DX to **`0x3C9`**;
- Writes three DAC components computed from the word at
  `0x2B5CE8`: `(value >> 9) & 0x3F | 1`,
  `(value >> 4) & 0x3F | 1`, and
  `(value << 1) & 0x3F | 1`.

The sample contains all three `OUT DX,AL` instructions, ending
with the third at `0x219714`. Precise loop *calling frequency*
cannot be inferred from the 99.876-ms **median** between sampled
VSYNC-rising events: the game samples selected retraces and not
necessarily every physical/emulated VGA interval.

**Directly demonstrated:** This site is a guest **vertical-sync
polling loop followed by palette DAC entry-zero programming** under
a conditional test, not simply a hypothetical wait site.

**Not demonstrated:** That this routine is the simulation's master
clock or that every call updates gameplay state. Its immediate
effect is conditional palette I/O.

### C. Three VGA-write-site snippets: unrolled conditional memory loops

At `0x2042FC`, `0x20461A`, and `0x20479A`, repeated
patterns are directly visible:

```text
EAX = uint32[ESI + offset]
compare uint32[EDI + offset] to EAX
if equal: branch to earlier code elsewhere in the routine
uint32[EBP + offset] = EAX
repeat for the next 4-byte aligned offset
```

Examples of those earlier branch targets include `0x203E18`,
`0x20408E`, and `0x2041AE`; all **outside the respective
64-byte slices**. These are very likely parts of a large, unrolled
conditional buffer-processing or block-copy/difference routine,
which is consistent with the previously sampled VGA-write PCs.

**Not demonstrated:** That ESI is the original game's sprite buffer,
EDI is a framebuffer, or EBP is VGA memory. The addresses and pointer
initialisation outside these snippets are required before assigning
specific roles. The fragment ends may bisect instructions; do not
trust partial terminal disassembly.

### D. `0x20C81E`: byte arithmetic, nested loop and lookup-table access

This fragment is not an independent function entry. It begins with
byte arithmetic and branches backward to **`0x20C808`**
(outside the snippet), modifies EDI, ESI and EBP, and includes
`RET` instructions at `0x20C82D` and `0x20C844`.
A later sequence beginning at `0x20C848` masks a byte of EAX and
indexes a dword table at **`0x241084`**.

**Directly demonstrated:** a loop with arithmetic over byte-sized
values, early/loop-back branches, and a table lookup; most likely a
generic processing helper. There is **no evidence in this snippet**
of a periodic timer or a game-state update, despite the word
`periodic` in the sampling label.

### Updated research direction

We now have **instruction-level evidence** separating the wait-for-
nonzero loop, VGA-synchronized DAC writes, unrolled buffer processing,
and an unidentified table/byte routine. The strongest lead toward
measuring true simulation cadence is the **producer** of
`DS:[uint32[DS:0x27177C] + 0x24]`; the currently observed consumer is not
automatically the game tick.

Next controlled test should:

1. Read and record **DS selector/base** and the **runtime pointer value**
   `uint32[DS:0x27177C]`. The watched **linear** address is
   `DS.base + pointer + 0x24` (subject to segment validity/limit rules).
   **CS base was observed as zero, but DS base has not yet been measured.**
   The pointer itself could vary between game states.
2. Capture **actual writes** to that address, including the writer
   `CS:EIP`, value before/after and PIC emulated timestamp, using a
   carefully scoped **normal-core** watchpoint or equivalent debugger
   instrumentation. Avoid an instruction hook in the default dynamic
   core for normal gameplay.
3. Correlate writes with IRQ0 events, VSYNC/DAC updates and the
   busy-wait read/reset. Separate a producer's work-ready signal
   from an actual confirmed combat-simulation update.
4. Obtain small private code contexts around the writer and around
   the target of `0x2054B9 -> 0x2054FE`, rather than dumping the game.
5. Only after disassembling and confirming a routine that advances
   **game state** should a precise logic-tick counter be built.

**No PIT/VGA frequency, guest binaries or simulation timing were
changed by this analysis.**

## New controlled experiment: watch the producer of MK2's busy-wait signal

**Implementation date:** 2026-11-10, branch `mk2-timing-research`.
**Current status:** committed, integrated, **Windows compilation and runtime
validation still pending**. No MK2 executable or copyrighted game bytes
were changed or committed.

### Why this experiment exists

The user supplied six locally captured, 64-byte MK2 code snippets.
Disassembly confirmed that `0180:00205489` loops reading a
**DS-relative** DWORD at `[ESI+0x24]`, where ESI itself comes
from `DS:[0x27177C]`. When nonzero, the consumer saves its value,
updates `DS:[0x275B10]`, resets the watched DWORD and branches to
`0x2054FE`. **We have not identified who writes the nonzero value.**
That writer is a promising lead, but a producer event is not by itself
proof that fighter animation or combat simulation advanced.

### Technical implementation

- `src/hardware/mk2_signal_probe.h`: new header-only,
  **default-disabled** signal writer observer. It is constructed from
  the pre-IRQ0 hook and becomes active only if
  `DOSBOX_MK2_TRACE` is set and
  `DOSBOX_MK2_SIGNAL_WATCH=1`. Requires the normal interpreter
  `cpudecoder == CPU_Core_Normal_Run`.
- `src/hardware/pic.cpp`: immediately before delivering IRQ0,
  when interrupted CS:EIP lies in the confirmed loop
  `0180:00205489–00205494`, capture the actual **DS selector/base**,
  read `DS.base+0x27177C`, then calculate the runtime linear watch
  address `DS.base + pointer + 0x24`. Also drains the event buffer
  every emulated second.
- `src/cpu/paging.h`: a cheap, guarded normal-core memory-write
  hook on the standard byte/word/dword/qword inline write helpers.
  Only stores overlapping the watched DWORD or its pointer-source
  DWORD invoke the expensive callback; aligned stores are captured
  once and cross-page stores via their byte components. Capture actual
  before/after DWORD values, store address and width, guest writer
  CS:EIP, writer DS selector/base and PIC emulated timestamp.
  **Guest writes themselves are unchanged.**
- `scripts/analyze_mk2_signal_trace.py`: standalone Python stdlib
  tool to report writes, changes from zero to nonzero, clears, writer
  CS:EIP concentration, pointer retargets and event spacing.

The output `*_signal.csv` distinguishes `resolve`,
`pointer_write`, `pointer_same`, `signal_write`,
and `signal_same`. The sample interval is emulated 30–150 seconds.
Note that the watch **starts after observing the consumer loop**,
not from the moment MK2 starts. Segment changes, pointer changes
between samples, direct memory access, host-originated operations or
a dynamic CPU core may bypass the observed helper paths. A zero-event
result therefore cannot establish that no signal writer exists.
The write callback still introduces one branch into common guest
memory write helpers, so compare trace-enabled/disabled CPU load.

### Next Windows test

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

$signalCsv = $env:DOSBOX_MK2_TRACE.Substring(0, $env:DOSBOX_MK2_TRACE.Length - 4) + "_signal.csv"
Test-Path "$signalCsv"
python scripts/analyze_mk2_signal_trace.py "$signalCsv"
Remove-Item Env:DOSBOX_MK2_SIGNAL_WATCH
```

Collect gameplay in the 30–150s emulated window, and upload
`*_signal.csv` plus analyzer stdout. The existing `_guest.csv`
may also help if unusual PC contexts are observed. Do not label a
writer as the MK2 simulation tick until its code and the related
actual game-state changes have been verified.

## First producer-watch capture: 6,990 clears, no observed producer (2026-11-10)

The user ran the original `*_signal.csv` analyzer after a
`core=normal` MK2 trace with the separate writer watch enabled.
Raw CSV has **not** been provided in this turn, so this section
is grounded in the user's **analyzer stdout** rather than
independent reanalysis of event timestamps.

| Output item | Observed |
|---|---:|
| Total CSV rows | **6,991** |
| `resolve` events | **1** |
| `signal_write` events | **6,990** |
| Unique writer CS:EIP | **0180:002054AD** (6,990 rows) |
| Runtime pointer | **0x00009B00** |
| Watched linear address | **0x00009B24** |
| Observed `0 -> nonzero` transitions | **0** |
| Observed `nonzero -> 0` clears | **6,990** |
| Reset spacing (median) | **16.652 ms** |
| Reset spacing (min / max) | **3.613 / 172.827 ms** |

**Confirmed:** `0x2054AD` is precisely the `mov dword
[esi+0x24], 0` instruction seen in the previous 64-byte
disassembly. Thus the probe correctly detects MK2 clearing the
nonzero signal after leaving the waiting loop. The approximately
16.65-ms reset interval is consistent with PIT0's ~60.052-Hz
period. It does **not** demonstrate that each clear is a combat
simulation update or that all clears are exactly PIT driven.
The recorded DS-based target happens to be 0x9B24 in this session.

**Not observed:** Any positive store to the watched DWORD. The
first version of the probe restricted write observations to
**protected mode with `cpudecoder == CPU_Core_Normal_Run`**.
That can exclude writes performed by BIOS/interrupt/DOS-extender
code in real mode or under a different decoder. Other possible
explanations include aliased memory or writes through direct
host-backed memory paths. The absence of a producer in the
first log is an **instrumentation coverage gap**, not a
demonstration that the producer does not exist.

### Correction committed on branch: broader watch + bounded opcode snapshots

The following changes are committed but **not yet built/runtime
validated on the user's Windows machine**:

- `src/hardware/mk2_signal_probe.h`: When the address has been
  resolved from the known consumer, the guest-write hook now checks
  stores overlapping the resolved signal/pointer **regardless of
  current CPU mode or decoder**. It still logs actual before/after
  DWORD values and writer CS:EIP.
- `src/cpu/core_normal.cpp`: An opt-in branch at the start of an
  interpreted instruction calls a bounded memory comparison.
  **Approximately every 60th IRQ0** that interrupts the known wait
  loop arms **up to 16,000 interpreted instructions**. It compares
  the watched DWORD at successive instruction boundaries. A change
  yields a new `opcode_transition` event with the preceding
  instruction's CS:EIP and the before/after values. Monitoring
  stops early after detecting `0 -> nonzero`.
- `src/hardware/pic.cpp`: Exposes the opt-in instruction-watch
  callback and boolean flag (false when not armed). The existing
  pre-IRQ0 resolver arms bounded windows.
- `scripts/analyze_mk2_signal_trace.py`: Reports transitions
  **separately by direct store or opcode-boundary observation**.
  A transition can appear in both mechanisms; **do not add
  their counts together**.

These changes **do not modify the original MK2 executable, memory,
PIT IRQ schedule or VGA timings**. However, a per-instruction memory
read incurs significant diagnostic overhead when armed: the next
run must not be used for comparing host FPS or p99 frame pacing.
The instruction-boundary PC is a *candidate* for an ordinary
guest store and must be checked by disassembly; a host callback
could also change memory between boundaries.

### Next validation test

Rebuild the latest `mk2-timing-research` using
`cmake --build --preset=release-windows --parallel 8`.
Run `core=normal` with **both**
`DOSBOX_MK2_TRACE` and `DOSBOX_MK2_SIGNAL_WATCH=1`
for active combat during emulated seconds 30–150.
Collect **just the new `*_signal.csv` and the
`scripts/analyze_mk2_signal_trace.py` stdout**.

If positive transitions appear, examine their writer
`CS:EIP` and inspect a tiny private instruction snippet
around the producer. Only after confirming a routine that
actually advances fighter positions, input state or combat
animation should we implement a true game-logic tick counter.

## Writer identified: MK2's work-ready flag is set at 09B0:08AF (2026-11-10 15:31)

**Evidence:** user-supplied stdout from
`mk2_normal_40000_20261110_153111_signal.csv`, analyzed by the
revised `scripts/analyze_mk2_signal_trace.py`. The raw CSV was NOT
uploaded in this turn; counts below are from the user's console output.

| Measurement | Observed |
|---|---:|
| Total signal rows | **12,329** |
| Direct `signal_write` rows | **12,226** |
| Instruction-boundary `opcode_transition` rows | **102** |
| Pointer `DS:[0x27177C]` | **0x00009B00** |
| Resolved signal linear address | **0x00009B24** |
| Producer writer | **CS 09B0, CS base 0x9B00, EIP 0x000008AF** |
| Producer `0 -> nonzero` writes | **6,108** |
| Consumer clear instruction | **0180:002054AD** |
| Consumer `nonzero -> 0` writes | **6,108** |
| Median spacing, direct positive writes | **16.652 ms** |
| Range, positive spacing | **16.649–166.524 ms** |
| Median spacing, consumer clears | **16.652 ms** |
| Range, clear spacing | **8.951–177.633 ms** |

The 102 `opcode_transition` events independently sampled the **same
producer CS:EIP, 09B0:08AF**. They are **not additional producer
writes**, and their roughly 999.127-ms median spacing is an artifact
of sampling approximately every 60th IRQ0, NOT evidence of a 1-Hz
game event.

**High-confidence conclusion:** We have confirmed both the exact
writer and the exact consumer of this watched DWORD:

- Producer: `09B0:08AF` (linear executing code location
  `0x9B00 + 0x8AF = 0xA3AF`), writing a positive/nonzero value
  to the watched signal at linear `0x9B24`.
- Consumer: `0180:002054AD`, clearing the same DWORD after exiting
  the wait loop.
- Both direct positive and clear events have the **16.652-ms
  median spacing** already observed for the ~60.052-Hz PIT0 timer.

The producer's `CS=09B0`, `CS.base=0x9B00` are compatible with
**segmented interrupt-handler code**, possibly an IRQ0 timer handler.
**However neither the current CPU operating mode nor the code
segment's 16/32-bit operand size has been confirmed by the supplied
stdout**, and an IVT pointer relationship has not yet been checked.
The 6,108 activations/resets do not prove the game's combat simulation
runs at 60 Hz. A timer/work signal is not necessarily a
fighter-animation state update.

### Local producer disassembly capture, added after this result

The user-visible filenames and previously available 64-byte snapshots
only captured protected-mode `0180` code; the producer lies in a
different segment `09B0`. A new **default-off, private, one-time**
snapshot is now committed to `src/hardware/mk2_signal_probe.h`:

- On the **first directly observed 0-to-nonzero producer store**
  with `DOSBOX_MK2_SIGNAL_WATCH=1`, copy **128 bytes at the producer
  instruction `CS.base + EIP`**, another **128 bytes beginning
  32 bytes earlier** for calling/branch context (may start mid-
  instruction), and **128 bytes at `0180:002054FE`**, the
  continuation target after the consumer's wait/clear loop.
- Read the original guest **IRQ0 IVT offset/segment** from linear
  `0x20/0x22` at the same instant and add those to each snapshot
  CSV row. This allows later verification of a direct IRQ0 handler
  association, rather than assuming one from CS and cadence alone.
- Capture `protected_mode` and, critically, `code_bits` from
  the current CPU code segment's default operand size, because
  the real-mode-style producer may be **16-bit**, while the main MK2
  consumer is 32-bit.
- Write a separate **`*_signal_code.csv`**, preserving the first
  seven columns of the previous `*_guest_code.csv` schema,
  and flush once per emulated second. Only three tiny code slices
  are collected; do not commit them, the CSV, or MK2 assets.
- Update `scripts/extract_mk2_guest_code.py` to read optional
  `code_bits`, print suitable `ndisasm -b 16` vs `-b 32`
  (or GNU objdump) commands, and display IRQ0 vector values.
  It remains compatible with the earlier 32-bit snapshots.

**New code capture status: committed but NOT yet built or run in
Windows.** Its initial result should be validated before assuming
the snapshot origin is an instruction boundary or that the IVT
vector points directly to the signal writer.

### Next controlled test

Rebuild latest `mk2-timing-research` and repeat the same
`core=normal`, 40,000 cycles, `DOSBOX_MK2_TRACE` and
`DOSBOX_MK2_SIGNAL_WATCH=1` run used successfully at 15:31.
The existing PowerShell runner clears `DOSBOX_MK2_TRACE` when
finished, so do not attempt to construct a filename from that
environment variable after the run; select the actual output
filename in `Documents/MK2Timing` instead.

The new `*_signal_code.csv` can be processed using:

```powershell
# From the repository root; supply actual filename from the new run.
python scripts/extract_mk2_guest_code.py "C:\Users\Usuario\Documents\MK2Timing\<NEW_RUN>_signal_code.csv"
```

Inspect the three 128-byte snippets and the captured
`ivt_irq0_segment:ivt_irq0_offset` first. Only after confirming
the producer is a timer/interrupt handler should we investigate
which *downstream* code advances fighter physics, animation
and input state. **Do not change PIT or VGA clocks to force
a target FPS.**

## Three producer/consumer code snippets, actual x86 disassembly (2026-11-10)

**Grounding:** user uploaded three **128-byte binary snapshots** from their
private MK2 execution:

- `producer_at_write_000008AF.bin`: guest `09B0:08AF`, decoded
  using **16-bit** x86 instruction mode.
- `producer_preceding_32_0000088F.bin`: guest `09B0:088F`, decoded
  as **16-bit** x86, but the first bytes may begin in the middle
  of an instruction. The 96-byte overlap with the 08AF binary
  **matches exactly**, validating the in-memory snapshot alignment.
- `consumer_after_wait_2054FE_002054FE.bin`: guest
  `0180:002054FE`, decoded as **32-bit** x86.

These three snippets were really inspected with `objdump`
and their byte overlap cross-checked. **Do not commit the actual
guest code bytes or disassembly files to GitHub.**

### Producer `09B0:08AF`: two 32-bit counters built from 16-bit words

The first instruction at `08AF` is `inc word ptr [0x0024]`,
followed by `jne 0x08B9` and `inc word ptr [0x0026]`.
Together these increment the **little-endian DWORD
`DS:[0x0024..0x0027]`**, propagating the carry only when
the low word rolls over to zero.

At `08B9`, the next sequence similarly increments
`DS:[0x0028..0x002B]` via two 16-bit words
(`INC [0028]`, conditional `INC [002A]`).

This directly explains the previous **6,108 producer
zero-to-nonzero observations** at the watched `DS:[0024]`
and **6,108 consumer clears** at `0180:002054AD`.
The first is a **work-ready/time counter** that can accumulate
increments before the protected-mode consumer empties it;
it is not merely a boolean wake-up flag.
The second DWORD is a separate counter, whose consumers
are **not yet identified**.

The preceding-code snippet contains a plausible instruction
boundary at `08A8`: `test byte ptr DS:[003C],0xFF`
then `jne 0x900`. This conditionally bypasses the increment
sequence; exact higher-level semantics are unknown.
Some other nearby instructions perform conditional calls
through pointer `DS:[0014]` and absolute calls to
`0x07CA`, but none of those calls is identified here
as game logic. **The disassembly origin at 088F may be
mid-instruction; early instruction interpretation is uncertain.**

The observed producer `CS=09B0` and `CS.base=0x9B00`
are consistent with 16-bit segmented code. They do
**not alone prove it is the IRQ0 interrupt handler**.
The IRQ0 IVT `segment:offset` values were written to
`*_signal_code.csv` by the new probe, but the user uploaded
the three `.bin` files, **not that CSV metadata**.
The direct association with the vector is still **unverified**.

### Consumer continuation `0180:002054FE`: delayed-work dispatcher

The 32-bit protected-mode sequence is:

```text
0x2054FE: ESI = dword [ESI]         // next list node
0x205500: test ESI, ESI
0x205502: if zero -> 0x205534     // return
0x205504: decrement word [ESI+6]
0x205508: if positive -> 0x2054FE // check next node
0x20550A: EBP = dword [ESI+8]
... copy dwords [ESI+C/10/14] into DS:[2B5A80/7C/78]
0x20552E: EDI = dword [ESI+18]
0x205531: JMP dword [ESI+1C]      // dispatch task callback
```

This is strong instruction-level evidence for a **linked-list
delayed-task / callback dispatcher**: it visits successive nodes,
decrements a 16-bit countdown at node+6, skips tasks whose
countdown remains positive, and transfers control to a function
pointer in node+1C on expiry. The list appears to start via
`0x275B04` from the previously decoded consumer entry,
but we have not traced all list-management functions.
The indirect transfer is a **JMP**, not a CALL.

Additional code beginning `0x205535` searches/relinks
nodes, and code at `0x205565` begins setting up
a collection with `ECX=0xA8`, although the 128-byte
snapshot ends partway through that setup. The exact structure
layout and function identities are not yet established.

**Critical interpretation:** This is much better evidence for
a *timer-driven scheduler* than for a directly measured
**60-Hz fighter simulation update**. We have NOT confirmed
the event timer's exact relationship to game logic,
sprite animation, controls, collision or physics.

### Next experiment should observe the actual indirect dispatches

Collect the original **`*_signal_code.csv`** metadata to
check `protected_mode`, `code_bits` and
`ivt_irq0_segment:ivt_irq0_offset` against the producer.

Then instrument the exact interpreted instruction
`0180:00205531` as a **default-off diagnostic** during combat:

- `PIC emulated time`, task node `ESI`, DS selector/base
- `WORD DS:[ESI+6]` after decrement
- `DWORD DS:[ESI+0x1C]` — actual callback destination
- optional `EBP` / `EDI` task context, entry source and repetition

Group callbacks by target EIP, count their recurrences and
compare across active combat, menus and intermissions.
Disassemble the callback sites **before** calling any one
of them a simulation tick. The callback destination is
DS-relative data; the resulting near jump uses the caller's CS
and guest code-mode context.

**Never modify the original executable, timer IRQ, PIT/VGA
frequency, or original game assets while researching this.**

## 2026-11-10 17:48: actual eight-stream scheduler and VGA capture analyzed

**Source:** eight USER-UPLOADED CSVs sharing basename
`mk2_normal_40000_20261110_174841`. The raw CSVs are local
user data and are NOT in the GitHub repository.

### Verified dispatch counts and cadence

- `_dispatch.csv`: **17,403** rows in emulated
  **30,003.305–94,946.505 ms**, **zero overflow**,
  **20 unique task nodes** and **126 unique callback targets**.
  Every observed post-decrement countdown `remaining` is **0**.
- Two persistent timer tasks at `ESI=0x276A8C` and
  `0x276774`: 3,727 and 3,721 dispatches overall,
  median each about **16.6522 ms**, typically only
  **0.0026 emulated ms** apart.
- During a representative 40–90 emulated-second window:
  **3,002** work-counter clears at `0180:002054AD`,
  **3,001** dispatches for `ESI=0x276A8C` and **2,998**
  for `ESI=0x276774`. Thus both primary tasks are
  usually scheduled **once per ~60-Hz work-counter period**.
  This does NOT prove they are game-combat simulation ticks.
- Main dynamic callbacks by node:
  - `ESI=0x276A8C` invoked `0x223DFD` (385 times,
    early phase) and then `0x2133B5` (3,332 times).
  - `ESI=0x276774` invoked `0x2239B2` (385 times,
    early phase) and then `0x22047D` (3,324 times).
- Other node median periods are integer timer multiples:
  `0x275E2C` ~49.956 ms (~3 ticks);
  `0x276144` ~99.913 ms (~6 ticks);
  `0x276DA4` ~133.216 ms (~8 ticks);
  `0x27645C` ~166.505 ms (~10 ticks).
  The callback `0x221EE0` recurred at ~33.304 ms
  (~2 ticks) during a limited phase. Some destinations
  are shared between nodes and produce misleading
  near-zero mixed medians; group by **node+time phase**.
- Around **39.4 s** the primary callback targets switch.
  Several additional task nodes begin around **56.7 s**.
  These are apparent state/scene changes in dispatch
  topology, not proven character-logic identities.

### Timer backlog and graphics

- `_signal.csv` has 3,901 direct producer writes
  at `09B0:08AF` (including accumulated increments),
  3,891 clears at `0180:002054AD`, and 65
  separate bounded opcode confirmations.
- A notable delayed-consumption event:
  from roughly **39,278.811 to 39,428.325 ms**, the
  producer increments the pending DWORD from **1 to 10**;
  at **39,438.852 ms** consumer writes **10 -> 0** in
  one store. This demonstrates an accumulating time/work
  counter, but **not that 10 distinct game simulation
  updates were recovered**.
  Immediately after the 39,438.852-ms clear the recorded
  scheduler made individual task dispatches at approximately
  39,440.035–39,440.041 ms, **not a visible tenfold burst**
  of the two primary task callbacks. This does not rule out
  other downstream catch-up or elapsed-time accounting. Visual output was mostly
  static around that phase; do not presume cause.
- `_frames.csv`: **6,652 scanouts**, **2,578 changed**
  visible images, 4,074 repeats; VGA roughly **70.09 Hz**.
  During 40–90 s **3,504** VGA scanouts resulted in
  **1,934** changed images (55.19%). Image changes
  are NOT a simulation clock.
- `_scanmem.csv` hashes match `_frames.csv`
  on **6,652/6,652** scanouts. `_lines.csv` reports
  **42,000/42,000** samples with
  `output_equals_cache=1`.
  No mismatch of sampled VGA result to renderer cache.

### Next experiment, implemented but NOT yet Windows-build-tested

The active opt-in writer/dispatcher observer in
`src/hardware/mk2_signal_probe.h` now additionally
captures a **private `*_dispatch_code.csv`**:
up to **14 selected callbacks**, once each, **256 bytes**
from each actual first-dispatched entry. Priority
targets `0x2133B5`, `0x22047D`, early-phase
`0x223DFD`, `0x2239B2`, and secondary-rate
`0x202D95`, `0x213213`, `0x21FF5D`, and other
common scene-dependent callbacks.

The first 7 columns follow the existing guest snapshot
format and include optional `code_bits=32`, so
`scripts/extract_mk2_guest_code.py` can extract
private `.bin` slices for local disassembly.
Activation is unchanged: both
`DOSBOX_MK2_TRACE=<unique>.csv` and
`DOSBOX_MK2_SIGNAL_WATCH=1` with
`core=normal`, protected cycles 40,000. The
probe neither modifies guest data nor stores copyrighted
game bytes in GitHub. Rebuild before the next run,
then inspect the callback code to determine which
tasks actually update fighter state; do NOT equate
60-Hz task dispatches with 60-Hz combat simulation.

## 2026-11-10 18:21: all 12 CSVs + 14 callback instruction snapshots analyzed

**Source basis:** User uploaded all twelve CSV files with
`mk2_normal_40000_20261110_182156` prefix,
including `_dispatch_code.csv`, `_signal_code.csv`,
`_dispatch.csv`, `_signal.csv`, `_frames.csv`,
`_scanmem.csv`, `_lines.csv`, `_guest.csv`,
`_guest_code.csv`, `_regs.csv`, `_vram.csv`,
and the main timing CSV. We actually parsed their contents.
The 14 callback snapshots were all 256 bytes of actual
guest 32-bit x86, disassembled locally with GNU objdump.
**No binary bytes or disassembly containing game code
were committed to GitHub**; the local private disassembly
and the analysis report were generated separately for
the user.

### Timer IRQ0 chain — vector now definitively grounded

The user finally supplied `_signal_code.csv` metadata
(not just earlier .bin files). All three rows report the
**real-mode IRQ0 IVT vector `09B0:0000`** (segment
2480 decimal, offset 0), while the producer executes at
**CS `09B0`, EIP `08AF`, CS base `0x9B00`,
16-bit code**. This establishes the producer is in
the same segment as the guest's IRQ0 handler entry;
the actual call/branch chain from the IVT's offset zero
to offset 0x8AF has **not** yet been decoded.

The producer incremented the 32-bit pending timer counter
at `DS:0x24..27` in previously decoded snippets.
The protected-mode consumer at `0180:2054AD` clears
the same linear `0x9B24` location.

### This run's counts

| Source | Observed |
|---|---:|
| `_dispatch.csv` normal-core scheduled callback events | **19,652** |
| Emulated dispatcher window | **30.015–92.993 s** |
| Distinct ESI task nodes | **29** |
| Distinct destination EIPs | **127** |
| `_dispatch.csv` overflow events | **0** |
| Direct positive timer producer events | **3,783** |
| Direct work-counter clears | **3,773** |
| Independent bounded opcode transition observations | **63** |
| VGA scanouts `_frames.csv` | **6,515** |
| Visible image changes `_frames.csv` | **2,808** |
| Repeated visible images | **3,707** |
| `_frames` / `_scanmem` hash matches | **6,515 / 6,515** |
| VGA output/cache line sample matches | **42,000 / 42,000** |

Different callback target totals versus the previous
17:48 capture (**126 targets, 20 nodes**) reflect
the *different gameplay phases* in each run. They
do not imply that MK2's native logic clock changed.
The main task nodes `0x276A8C` and `0x276774`
still have median dispatcher intervals **16.652 ms**.

New phase-specific observations:

- At emulated **30.015–32.213 s**, the two main
  nodes run `0x223DFD` and `0x2239B2`
  respectively, **133 dispatches each**.
- At emulated **35.260 s**, `0x276A8C` begins
  the persistent callback `0x2133B5` (**3,466**
  runs overall). At **35.327 s**, `0x276774`
  begins `0x22047D` (**3,458** runs).
- Multiple other entities then appear. In
  50–60 s there are **29 distinct task nodes**.
  Unlike the two main nodes, some of these run
  identical callback EIPs on different objects,
  explaining why a combined per-callback interval
  can be misleading.
- In the signal stream a pending-value accumulation
  **1→2→...→10** occurs around emulated
  **35.111–35.244 s**, ending with a single
  **10→0** clear. As with the earlier capture,
  this is *not* proof of replaying ten individual
  game-physics updates.

### Direct 32-bit x86 evidence about callback roles

`0x2133B5` — **priority logic candidate**:
the routine conditionally decrements DWORD
`DS:[0x2B62D2]` and loads two object pointers
from `DS:[0x2B5FC2]` and `DS:[0x2B6146]`.
It calls **`0x21374A` twice**, first with EAX
holding one object pointer, then with EAX holding
the other. It also calls `0x2136F6` and
performs multiple conditions on state and
positions. These two callee bodies **were not
contained in the 256-byte snapshot** and must
be examined before assigning actual game simulation
or fighter-update semantics. The repeated
calls make `0x21374A` an especially valuable
candidate for a per-object update routine.

`0x22047D` — **position/viewport candidate**:
reads signed WORD `[EDI+0x1A]` from each
of those same two global objects, orders the
values, computes their difference and midpoint
(`SHR distance,1; ADD lower_position`),
and checks a global `DS:[0x2C432E]` with
several positional bounds (`0x50`, `0x3C`,
`0x117`). This is consistent with midpoint/
camera tracking or positional framing.
It does **not** directly prove fighter physics
integration; do not label it a combat tick.

`0x2239B2` — **strong early-phase input/bitfield
candidate**: reads `DWORD [DWORD DS:[0x27177C]+0x20]`,
i.e. a value from the same pointer structure
used by the 16-bit IRQ producer; selects one
16-bit half depending on `WORD [ESI+4]`,
masks `0x1F0`, stores successive states
at `[ESI+0x6C]` and `[ESI+0x70]`,
then branches based on button/flag bits.
This is strong evidence of input/state-edge
processing, without enough context to identify
individual button meanings.

`0x223DFD` — tests/updates flags
`[EDI+0x34]` including bit `0x40` and
compares `WORD [EDI+0x40]` and state value;
probably stage/entity-state work but role
is not yet verified.

`0x22FB73` — branches on bitfield
masks `8`, `4`, `1`, and `2`
from helper `0x201739`; the callbacks
appear on multiple task nodes in this run.
`0x22FABF` — reads `WORD [EDI+0x1E]`
and compares against an object/global
threshold; its **2,812** dispatches were
spread over many task nodes, suggesting
per-entity work rather than a sole global
60-Hz simulation callback. Neither routine's
exact gameplay meaning is proven.

The 14 `_dispatch_code.csv` rows had full
256-byte payloads and valid **code_bits=32**.
Some end mid-instruction or branch outside
their 256-byte region. Do not treat
misaligned trailing disassembly as code facts.

### Next experiment already staged in fork, not yet Windows-tested

The current branch's opt-in `OnTaskDispatch()`
will now read **two dynamic candidate-object
pointers** `DS:[0x2B5FC2]` and
`DS:[0x2B6146]` and sample their signed
WORD fields `+0x1A` and `+0x1E` just
**before each scheduled callback**. The extra
8 CSV columns are added to the end of
`*_dispatch.csv`, leaving legacy analysis
columns unchanged. New standard-library
`scripts/analyze_mk2_object_trace.py`
reports per-object change cadence, pointer
changes, and the *preceding dispatched callback*
as a correlation candidate — **not proof of
which CPU instruction wrote the field**.

The private `*_dispatch_code.csv` will
also include **8 extra `helper_candidate_...`
snapshots**, including `0x21374A`,
`0x2136F6`, `0x201739`, `0x202706`,
`0x203285`, `0x231EC8`, `0x2054BB`,
and `0x205673`. These are actual code
addresses referenced by known MK2 callback
fragments; capturing code bytes alone
does not prove the helpers were executed
during that particular snapshot.

**Next:** Windows compile, repeat the same
normal core, 40,000 protected cycles,
`DOSBOX_MK2_TRACE=<unique file>` and
`DOSBOX_MK2_SIGNAL_WATCH=1` run.
Inspect `*_dispatch_code.csv` via
`scripts/extract_mk2_guest_code.py`
and use `scripts/analyze_mk2_object_trace.py`
on the new `*_dispatch.csv`.
To positively attribute coordinate changes
to a writer, a direct guest memory write
watchpoint may still be needed. No PIT,
IRQ, VGA or executable modification is
part of this experiment.

## Context to restore in a new chat

> MK2 DOS timing project `WildGer/dosbox-staging` branch `mk2-timing-research`. Read **2026-11-10 18:21: all 12 CSVs + 14 callback instruction snapshots analyzed** in this findings doc. User supplied actual 12 CSVs, including full `_dispatch_code.csv` (14 x 256-byte private 32-bit code snapshots) and `_signal_code.csv`. IRQ0 IVT entry now **confirmed 09B0:0000**, producer **09B0:08AF** in same 16-bit real-mode segment increments work DWORD; protected consumer 0180:2054AD resets. Run 18:21: 19,652 scheduler dispatches across 29 task nodes, 127 callback EIPs, 0 overflow; two main nodes ~60Hz; 6515 VGA scanouts and 2808 visible changes with 6515/6515 frame/scanmem and 42000/42000 line/cache agreement. Pending signal accumulates 10 ticks near 35.11–35.24s during phase change. Real 32-bit disassembly: **0x2133B5** handles both global object pointers 0x2B5FC2 and 0x2B6146 via **two CALLs to 0x21374A**, making 0x21374A priority to decode for true object update; **0x22047D** compares WORD fields [obj+1A] from both objects and calculates midpoint/bounds, more likely camera/framing than physics; early **0x2239B2** reads masked bitfield from shared IRQ pointer+0x20, probably input/state processing; **0x22FABF**, **0x22FB73** on many task nodes. None establishes a 60Hz game combat tick. NEW UNTESTED PATCH on fork: extends `*_dispatch.csv` with two candidate object pointers and signed WORD [+1A,+1E] sampled at each dispatch; `scripts/analyze_mk2_object_trace.py` groups observed value changes vs preceding task (correlation only, not proof writer); `*_dispatch_code.csv` additionally captures 8 helper candidates including 0x21374A and 0x2136F6 in private user RAM. NEXT compile, run normal core 40k with `DOSBOX_MK2_TRACE` + `DOSBOX_MK2_SIGNAL_WATCH=1`, upload new `_dispatch.csv` and `_dispatch_code.csv`, inspect helpers and potential actor field change intervals; proceed to direct writer watch if needed. No MK2 code bytes in GitHub; no changes to game memory or PIT/VGA timings.

**Raw CSVs are not in the repository.** Continue from the user-provided capture files in this conversation if available.
