// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental source scanout memory-state telemetry for MK2 timing research.
// Reads VGA backing buffers and effective palette; never modifies guest state.
#ifndef DOSBOX_MK2_SCANMEM_PROBE_H
#define DOSBOX_MK2_SCANMEM_PROBE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "hardware/video/vga.h"

// One row per complete VGA scanout. This is NOT a snapshot of each individual
// pixel at its exact beam position: beginning/end VRAM hashes only bracket
// the emulator's scanline drawing. The *_frames.csv image is the actual
// sequentially scanned-out source, hence can contain a mix of old/new lines.
//
// Tracing is enabled only if DOSBOX_MK2_TRACE names a writable CSV file.
// Limit the memory sampled per buffer so tracing remains useful on fast PCs.
// Any unsampled video pages are explicitly outside this probe's coverage.
class Mk2ScanmemProbe final {
public:
    static Mk2ScanmemProbe& Get()
    {
        static Mk2ScanmemProbe instance = {};
        return instance;
    }

    void OnBegin(const double emulated_ms)
    {
        if (!enabled) {
            return;
        }
        start_state = Capture();
        start_ms = emulated_ms;
        have_start = true;
    }

    void OnComplete(const double emulated_ms,
                    const uint64_t frame_index,
                    const uint64_t source_image_hash,
                    const bool source_image_changed,
                    const int width,
                    const int height,
                    const int pixel_format)
    {
        if (!enabled || !have_start) {
            return;
        }

        const auto end = Capture();
        const auto& begin = start_state;

        const bool raw_during = begin.raw_valid && end.raw_valid &&
                                begin.raw_hash != end.raw_hash;
        const bool fast_during = begin.fast_valid && end.fast_valid &&
                                 begin.fast_hash != end.fast_hash;
        const bool palette_during =
                begin.palette_hash != end.palette_hash;

        const bool have_previous = have_previous_end;
        const bool raw_between =
                have_previous && previous_end.raw_valid && begin.raw_valid &&
                previous_end.raw_hash != begin.raw_hash;
        const bool fast_between =
                have_previous && previous_end.fast_valid && begin.fast_valid &&
                previous_end.fast_hash != begin.fast_hash;
        const bool palette_between =
                have_previous && previous_end.palette_hash != begin.palette_hash;

        output << std::fixed << std::setprecision(6)
               << frame_index << ',' << start_ms << ',' << emulated_ms
               << ',' << (emulated_ms - start_ms)
               << ',' << static_cast<int>(source_image_changed)
               << ',' << width << ',' << height << ',' << pixel_format
               << ',' << static_cast<int>(begin.vga_mode)
               << ',' << static_cast<int>(end.vga_mode)
               << ',' << begin.source_kind << ',' << end.source_kind
               << ',' << begin.raw_bytes << ',' << end.raw_bytes
               << ',' << begin.fast_bytes << ',' << end.fast_bytes
               << ',' << static_cast<int>(begin.raw_valid)
               << ',' << static_cast<int>(end.raw_valid)
               << ',' << static_cast<int>(begin.fast_valid)
               << ',' << static_cast<int>(end.fast_valid)
               << ',' << static_cast<int>(raw_during)
               << ',' << static_cast<int>(fast_during)
               << ',' << static_cast<int>(palette_during)
               << ',' << static_cast<int>(raw_between)
               << ',' << static_cast<int>(fast_between)
               << ',' << static_cast<int>(palette_between)
               << ',' << begin.display_start << ',' << end.display_start
               << ',' << begin.panning << ',' << end.panning
               << ',' << std::hex << std::setfill('0')
               << std::setw(16) << begin.raw_hash
               << ',' << std::setw(16) << end.raw_hash
               << ',' << std::setw(16) << begin.fast_hash
               << ',' << std::setw(16) << end.fast_hash
               << ',' << std::setw(16) << begin.palette_hash
               << ',' << std::setw(16) << end.palette_hash
               << ',' << std::setw(16) << source_image_hash
               << std::dec << std::setfill(' ') << '\n';

        ++completed_count;
        if ((completed_count & 63u) == 0) {
            output.flush();
        }

        previous_end = end;
        have_previous_end = true;
        have_start = false;
    }

    void OnAbort()
    {
        if (!enabled) {
            return;
        }
        have_start = false;
        // A future completed scanout must not compare with a skipped scanout.
        have_previous_end = false;
    }

private:
    // For MK2's conventional Mode 13h this includes the 64 KiB pixel area,
    // plus nearby buffers. Does not claim to cover every bank of large SVGA
    // configurations.
    static constexpr std::size_t RawLimitBytes = 256u * 1024u;
    static constexpr std::size_t FastLimitBytes = 128u * 1024u;
    static constexpr uint64_t FnvOffset = 14695981039346656037ULL;
    static constexpr uint64_t FnvPrime = 1099511628211ULL;

    struct Snapshot {
        uint64_t raw_hash = 0;
        uint64_t fast_hash = 0;
        uint64_t palette_hash = 0;
        std::size_t raw_bytes = 0;
        std::size_t fast_bytes = 0;
        uint32_t display_start = 0;
        uint32_t panning = 0;
        int vga_mode = 0;
        int source_kind = 0; // 1 = raw backing, 2 = fastmem, 3 = other
        bool raw_valid = false;
        bool fast_valid = false;
    };

    static uint64_t HashBytes(const uint8_t* data, const std::size_t count)
    {
        auto hash = FnvOffset;
        for (std::size_t i = 0; i < count; ++i) {
            hash = (hash ^ data[i]) * FnvPrime;
        }
        return hash;
    }

    static Snapshot Capture()
    {
        Snapshot state = {};
        state.vga_mode = static_cast<int>(vga.mode);
        state.display_start = vga.config.real_start;
        state.panning = vga.draw.panning;

        const auto memory_size = static_cast<std::size_t>(vga.vmemsize);
        if (vga.mem.linear && memory_size) {
            state.raw_bytes = std::min(memory_size, RawLimitBytes);
            state.raw_hash = HashBytes(vga.mem.linear, state.raw_bytes);
            state.raw_valid = true;
        }
        if (vga.fastmem && memory_size) {
            state.fast_bytes = std::min(memory_size * 2u, FastLimitBytes);
            state.fast_hash = HashBytes(vga.fastmem, state.fast_bytes);
            state.fast_valid = true;
        }

        if (vga.draw.linear_base == vga.mem.linear) {
            state.source_kind = 1;
        } else if (vga.draw.linear_base == vga.fastmem) {
            state.source_kind = 2;
        } else {
            state.source_kind = 3;
        }

        // vga.dac.palette_map is the currently effective 32-bit RGB lookup
        // used by VGA's indexed-colour drawing paths. Hash the RGB bytes
        // explicitly instead of relying on host struct layout.
        auto hash = FnvOffset;
        for (const auto& color : vga.dac.palette_map) {
            const auto rgb = static_cast<uint32_t>(color) & 0x00ffffffu;
            for (unsigned int shift = 0; shift < 24; shift += 8) {
                hash = (hash ^ ((rgb >> shift) & 0xffu)) * FnvPrime;
            }
        }
        state.palette_hash = hash;
        return state;
    }

    Mk2ScanmemProbe()
    {
        const char* setting = std::getenv("DOSBOX_MK2_TRACE");
        if (!setting || !*setting || std::strcmp(setting, "0") == 0) {
            return;
        }
        const std::string trace = std::strcmp(setting, "1") == 0
                                          ? "mk2_timing.csv"
                                          : setting;
        const auto slash = trace.find_last_of("/\\");
        const auto dot = trace.find_last_of('.');
        const auto cut = dot != std::string::npos &&
                                 (slash == std::string::npos || dot > slash)
                                 ? dot
                                 : trace.size();
        const auto path = trace.substr(0, cut) + "_scanmem" +
                          (cut < trace.size() ? trace.substr(cut) : ".csv");
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "scanout_index,begin_ms,end_ms,span_ms,"
                  "visible_changed,width,height,pixel_format,"
                  "begin_vga_mode,end_vga_mode,begin_source_kind,end_source_kind,"
                  "begin_raw_bytes,end_raw_bytes,begin_fast_bytes,end_fast_bytes,"
                  "begin_raw_valid,end_raw_valid,begin_fast_valid,end_fast_valid,"
                  "raw_changed_during,fast_changed_during,palette_changed_during,"
                  "raw_changed_between,fast_changed_between,palette_changed_between,"
                  "begin_display_start,end_display_start,begin_panning,end_panning,"
                  "raw_begin_hash,raw_end_hash,fast_begin_hash,fast_end_hash,"
                  "palette_begin_hash,palette_end_hash,visible_image_hash\n";
        enabled = true;
    }

    bool enabled = false;
    bool have_start = false;
    bool have_previous_end = false;
    std::ofstream output = {};
    uint64_t completed_count = 0;
    double start_ms = 0.0;
    Snapshot start_state = {};
    Snapshot previous_end = {};
};
#endif // DOSBOX_MK2_SCANMEM_PROBE_H
