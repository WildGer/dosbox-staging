# Mortal Kombat II (DOS) — timing investigation log and handoff

**Project:** DOSBox Staging research fork, `WildGer/dosbox-staging`
**Branch:** `mk2-timing-research`
**Working platform:** Windows 11, Visual Studio 2026 (CMake `release-windows` preset), ClangCL, vcpkg at `C:\vcpkg`
**Research date:** 2026-11-09
**Game:** GOG installation of *Mortal Kombat II*, launched as an external DOS .EXE.
**Status:** All five CSV instrumentation streams now run on Windows, including the new boundary memory-state (`_scanmem.csv`) trace. The 2026-11-10 five-CSV capture has been analyzed below. Next open question: 696 changed source images with no preceding observed VRAM write/DAC commit and no sampled backing-memory or palette boundary change.

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
| **`_scanmem.csv`** | **New, not yet runtime validated.** At beginning/end of a scanout, hash raw backing VGA memory, its fastmem cache and the effective RGB palette; cross-reference the existing finished source-image hash | `hardware/mk2_scanmem_probe.h`, `hardware/video/vga_draw.cpp`, `gui/render/render.cpp` |

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

## Context to restore in a new chat

> We are investigating DOSBox Staging's MK2 DOS visual frame pacing (private use, no game assets). Repo: `WildGer/dosbox-staging`, branch `mk2-timing-research`. Read this findings log and `docs/mk2-timing-research.md`. PIT0 ~60.052 Hz (divisor 19869), VGA ~70.09 Hz; the 3-1-1-2 sequence of source-image-change intervals recurs at 20k, 40k and 80k CPU cycles. We installed five opt-in CSV probes: PIT/IRQ/VGA, visual frames + 8x5 regions, VGA handler write bursts, CRTC/panning/DAC events, and scanout-start/end hashes of raw VGA memory, fastmem and the effective palette. All five ran on Windows in the 2026-11-10 40k-cycle capture `mk2_40000_20261110_124505`: 10,505 scanouts / 3,678 changed images total; in 320x200 mode, 9,975 scanouts / 3,642 changed. The boundary probe validated exact image-hash and scanout-index agreement. It classified 1,519 changed graphics frames with VRAM mutation during scanout, 308 with VRAM mutation between scanouts only, 1,060 with palette-only change between scanouts, and 755 without any detected net boundary change. Of those 755, 696 also lacked observed VRAM writes or DAC RGB commits in the preceding completion interval. These 696 are NOT established emulation bugs: transient writes, scanline sampling, other VGA state or renderer cache could explain them. No timing changes have been made. Next: focused per-scanline source/address/output hashes and timestamps around recurring region patterns; compare `*_frames.csv` and register/write traces. The source CSVs are NOT committed to GitHub; ask for them if needed.

**Do not assume the source CSVs are in the repository.** If necessary, upload the five CSVs again. Maintain this document when new evidence changes a hypothesis.
