// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental, opt-in scanline timing / source-vs-render-cache telemetry.
// Never alters the guest, VGA state, timing parameters, or pixel buffers.
#ifndef DOSBOX_MK2_SCANLINE_PROBE_H
#define DOSBOX_MK2_SCANLINE_PROBE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "hardware/pic.h"
#include "hardware/video/vga.h"

class Mk2ScanlineProbe final {
public:
    static Mk2ScanlineProbe& Get()
    {
        static Mk2ScanlineProbe probe = {};
        return probe;
    }

    // Called after VGA has selected the actual draw source and scheduled its
    // line events. Sampling is sparse: one emulated second near 40, 60, 80,
    // 100 and 120 s. This keeps normal gameplay largely uninstrumented.
    void OnFrameBegin(const double timestamp_ms)
    {
        active = false;
        if (!enabled || vga.mode != M_VGA ||
            vga.draw.image_info.width != 320 ||
            vga.draw.image_info.height != 200 ||
            vga.draw.line_length != 320u * 4u ||
            !vga.draw.linear_base || vga.draw.linear_mask == 0) {
            return;
        }
        const auto second = static_cast<uint32_t>(timestamp_ms / 1000.0);
        if (!(second == 40 || second == 60 || second == 80 ||
              second == 100 || second == 120)) {
            return;
        }
        // On first frame in a new window, forget comparisons across gaps.
        if (second != last_sample_second) {
            previous_line_valid.fill(false);
            last_sample_second = second;
        }

        frame_begin_ms = timestamp_ms;
        active = true;
        pending = false;
        for (auto& row : rows) {
            row.valid = false;
        }
    }

    // Runs immediately before the source line is converted to BGRX32.
    // In MK2's observed M_VGA configuration the lookup reads one byte per
    // pixel from vga.draw.linear_base; fastmem can be this source.
    void OnBeforeLine(const uint32_t line_number, const uint32_t address)
    {
        pending = false;
        if (!active || line_number >= rows.size()) {
            return;
        }
        auto& row = rows[line_number];
        row.valid = true;
        row.line = line_number;
        row.address = address;
        row.masked_offset = address & vga.draw.linear_mask;
        row.source_kind = vga.draw.linear_base == vga.fastmem ? 2 :
                          vga.draw.linear_base == vga.mem.linear ? 1 : 3;
        row.sample_ms = PIC_FullIndex();
        row.raw_source_hash = FnvOffset;
        for (uint32_t x = 0; x < 320; ++x) {
            const auto byte = vga.draw.linear_base[
                    (address + x) & vga.draw.linear_mask];
            row.raw_source_hash = (row.raw_source_hash ^ byte) * FnvPrime;
        }

        // Palette state as used by the VGA RGB line conversion. The hash
        // covers all 256 entries; it does not assert all are visible.
        row.palette_hash = FnvOffset;
        for (const auto& color : vga.dac.palette_map) {
            const auto rgb = static_cast<uint32_t>(color) & 0x00ffffffu;
            for (unsigned int shift = 0; shift < 24; shift += 8) {
                row.palette_hash =
                        (row.palette_hash ^ ((rgb >> shift) & 0xffu)) * FnvPrime;
            }
        }
        pending_line = line_number;
        pending = true;
    }

    // Runs after VGA_DrawLine() produces RGB output, before the scaler
    // consumes that line. No per-line disk I/O.
    void OnAfterLine(const uint8_t* rgb_line)
    {
        if (!active || !pending || !rgb_line) {
            return;
        }
        auto& row = rows[pending_line];
        row.output_hash = HashBytes(rgb_line, 320u * 4u);
        row.output_ready = true;
        pending = false;
    }

    // The full frame has been latched into the renderer's source cache.
    // Compare each recorded RGB scanline with the row actually retained
    // for this frame. Index and timestamps join to *_frames.csv.
    void OnComplete(const uint64_t scanout_index,
                    const double completion_ms,
                    const uint8_t* cached_image,
                    const int width,
                    const int height,
                    const int pitch)
    {
        if (!active) {
            return;
        }
        active = false;
        pending = false;
        if (!cached_image || width != 320 || height != 200 ||
            pitch != 320 * 4) {
            return;
        }
        for (uint32_t line = 0; line < rows.size(); ++line) {
            auto& row = rows[line];
            if (!row.valid || !row.output_ready) {
                continue;
            }
            const auto cache_hash = HashBytes(
                    cached_image + static_cast<std::size_t>(line) * pitch,
                    320u * 4u);
            const bool known_prev = previous_line_valid[line];
            const bool source_changed = known_prev &&
                    row.raw_source_hash != prev_source_hash[line];
            const bool palette_changed = known_prev &&
                    row.palette_hash != prev_palette_hash[line];
            const bool output_changed = known_prev &&
                    row.output_hash != prev_output_hash[line];
            const bool cache_changed = known_prev &&
                    cache_hash != prev_cache_hash[line];
            output << scanout_index << ',' << line << ','
                   << std::fixed << std::setprecision(6)
                   << frame_begin_ms << ',' << row.sample_ms << ','
                   << completion_ms << ',' << row.address << ','
                   << row.masked_offset << ',' << row.source_kind << ','
                   << static_cast<int>(known_prev) << ','
                   << static_cast<int>(source_changed) << ','
                   << static_cast<int>(palette_changed) << ','
                   << static_cast<int>(output_changed) << ','
                   << static_cast<int>(cache_changed) << ','
                   << static_cast<int>(row.output_hash == cache_hash) << ','
                   << std::hex << std::setfill('0')
                   << std::setw(16) << row.raw_source_hash << ','
                   << std::setw(16) << row.palette_hash << ','
                   << std::setw(16) << row.output_hash << ','
                   << std::setw(16) << cache_hash
                   << std::dec << std::setfill(' ') << '\n';
            prev_source_hash[line] = row.raw_source_hash;
            prev_palette_hash[line] = row.palette_hash;
            prev_output_hash[line] = row.output_hash;
            prev_cache_hash[line] = cache_hash;
            previous_line_valid[line] = true;
            ++written_lines;
        }
        if ((++sampled_frames & 15u) == 0) {
            output.flush();
        }
    }

    void OnAbort()
    {
        active = false;
        pending = false;
        previous_line_valid.fill(false);
    }

private:
    static constexpr uint64_t FnvOffset = 14695981039346656037ULL;
    static constexpr uint64_t FnvPrime = 1099511628211ULL;

    struct LineSample {
        uint32_t line = 0;
        uint32_t address = 0;
        uint32_t masked_offset = 0;
        int source_kind = 0;
        double sample_ms = 0.0;
        uint64_t raw_source_hash = 0;
        uint64_t palette_hash = 0;
        uint64_t output_hash = 0;
        bool valid = false;
        bool output_ready = false;
    };

    static uint64_t HashBytes(const uint8_t* ptr, const std::size_t count)
    {
        auto hash = FnvOffset;
        for (std::size_t i = 0; i < count; ++i) {
            hash = (hash ^ ptr[i]) * FnvPrime;
        }
        return hash;
    }

    Mk2ScanlineProbe()
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
        const auto cut = (dot != std::string::npos &&
                          (slash == std::string::npos || dot > slash))
                                 ? dot
                                 : trace.size();
        const auto path = trace.substr(0, cut) + "_lines" +
                          (cut < trace.size() ? trace.substr(cut) : ".csv");
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "scanout_index,line,frame_begin_ms,line_sample_ms,"
                  "frame_complete_ms,draw_address,masked_offset,"
                  "source_kind,previous_valid,source_changed,"
                  "palette_changed,output_changed,cache_changed,"
                  "output_equals_cache,source_hash,palette_hash,"
                  "vga_output_hash,renderer_cache_hash\n";
        enabled = true;
    }

    bool enabled = false;
    bool active = false;
    bool pending = false;
    uint32_t pending_line = 0;
    uint32_t last_sample_second = UINT32_MAX;
    double frame_begin_ms = 0.0;
    uint64_t sampled_frames = 0;
    uint64_t written_lines = 0;
    std::ofstream output = {};
    std::array<LineSample, 200> rows = {};
    std::array<bool, 200> previous_line_valid = {};
    std::array<uint64_t, 200> prev_source_hash = {};
    std::array<uint64_t, 200> prev_palette_hash = {};
    std::array<uint64_t, 200> prev_output_hash = {};
    std::array<uint64_t, 200> prev_cache_hash = {};
};

#endif // DOSBOX_MK2_SCANLINE_PROBE_H
