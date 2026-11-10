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

## Context to restore in a new chat

> Continue MK2 DOS timing study in GitHub repo `WildGer/dosbox-staging`, branch `mk2-timing-research`, documents `docs/mk2-timing-research.md` and `docs/mk2-timing-research-findings-2026-11-09.md`. Six VGA-oriented CSV probes were Windows-validated; the sixth line trace showed 70,000/70,000 VGA output-to-renderer-cache hash agreement, and explained 837 formerly mysterious changes as compatible with prior-scanout guest VRAM writes. Seventh opt-in `_guest.csv` probe was built and supplied as analyzer stdout `mk2_40000_20261110_133415_guest.csv`: 30,385 context samples, ~120 s. 7,097/7,207 pre-IRQ0 samples at `0180:00205489`; ALL 1,473 status-rising and ALL 5,029 status-sampling events at `0180:002196D5`, median VSYNC-rising separation 99.876 ms; 4,676 VRAM first-write context samples concentrated in `0180:00204200–002049FF`. Other periodic PCs include `0180:0020C800`, `0180:00208700`. These are candidate CPU locations **NOT simulation ticks**. DOSBox Staging `core=auto` uses dynamic core in protected mode; callback EIPs may be stale/block-level. Next: short `core=normal` diagnostic capture to validate location precision, then disassemble guest code with correct DOS extender mapping; identify a real game-state update routine before counting simulation ticks. Do not alter PIT (60.052 Hz), VGA (70.09 Hz), MK2 binaries or gameplay timing. All game assets remain private; raw CSVs are not committed to GitHub.

**Do not assume the source CSVs are in the repository.** If necessary, upload all seven CSVs from the next run. Maintain this document when new evidence changes a hypothesis.
