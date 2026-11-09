// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in source-frame pacing telemetry for MK2 timing research.
// Does not change VGA timing, guest memory or rendered pixel data.
#ifndef DOSBOX_MK2_FRAME_PROBE_H
#define DOSBOX_MK2_FRAME_PROBE_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "utils/rgb888.h"

// Observes completed source frames from RENDER_EndUpdate(false).
// Identical images are recorded as repeats, even when the VGA scanout
// continues at ~70 Hz. Indexed-colour palette changes are considered only
// for palette entries actually used by the currently scanned-out pixels.
class Mk2FrameProbe final {
public:
    static Mk2FrameProbe& Get()
    {
        static Mk2FrameProbe probe = {};
        return probe;
    }

    void OnCompletedScanout(const double emulated_ms,
                            const uint8_t* pixels,
                            const int width,
                            const int height,
                            const int pitch,
                            const int pixel_format,
                            const bool indexed8,
                            const Rgb888* palette)
    {
        if (!enabled || !pixels || width <= 0 || height <= 0 || pitch <= 0) {
            return;
        }

        // Capture the timestamp BEFORE doing the hashing work.
        const auto host_now = Clock::now();
        const auto size = static_cast<std::size_t>(pitch) *
                          static_cast<std::size_t>(height);

        uint64_t pixel_hash = FnvOffset;
        uint64_t visible_hash = FnvOffset;
        std::array<bool, 256> used_palette_entries = {};

        for (std::size_t i = 0; i < size; ++i) {
            const auto value = pixels[i];
            pixel_hash = (pixel_hash ^ value) * FnvPrime;
            if (indexed8 && palette) {
                used_palette_entries[value] = true;
                // Hash the *actual RGB value* of each indexed pixel. Two
                // different palette indices can map to the same visible RGB
                // colour, so raw index changes alone are not sufficient.
                const auto& rgb = palette[value];
                visible_hash = (visible_hash ^ rgb.red) * FnvPrime;
                visible_hash = (visible_hash ^ rgb.green) * FnvPrime;
                visible_hash = (visible_hash ^ rgb.blue) * FnvPrime;
            }
        }
        if (!indexed8 || !palette) {
            visible_hash = pixel_hash;
        }

        uint64_t visible_palette_hash = 0;
        if (indexed8 && palette) {
            visible_palette_hash = FnvOffset;
            for (std::size_t i = 0; i < used_palette_entries.size(); ++i) {
                if (!used_palette_entries[i]) {
                    continue;
                }
                const auto& rgb = palette[i];
                visible_palette_hash =
                        (visible_palette_hash ^ static_cast<uint8_t>(i)) * FnvPrime;
                visible_palette_hash =
                        (visible_palette_hash ^ rgb.red) * FnvPrime;
                visible_palette_hash =
                        (visible_palette_hash ^ rgb.green) * FnvPrime;
                visible_palette_hash =
                        (visible_palette_hash ^ rgb.blue) * FnvPrime;
            }
        }

        const bool geometry_changed = !have_previous ||
                                      width != previous_width ||
                                      height != previous_height ||
                                      pitch != previous_pitch ||
                                      pixel_format != previous_pixel_format;
        const bool pixels_changed = !have_previous ||
                                    pixel_hash != previous_pixel_hash;
        const bool visible_palette_changed = !have_previous ||
                                             visible_palette_hash != previous_palette_hash;
        // Count changes in *visible image data*, not merely changes in raw
        // palette indices or unused palette entries.
        const bool changed = geometry_changed || !have_previous ||
                             visible_hash != previous_visible_hash;

        const auto host_elapsed_ms =
                std::chrono::duration<double, std::milli>(host_now - start_host).count();
        const auto scanout_interval_emulated_ms =
                have_previous ? emulated_ms - previous_emulated_ms : 0.0;
        const auto scanout_interval_host_ms =
                have_previous
                        ? std::chrono::duration<double, std::milli>(
                                  host_now - previous_host).count()
                        : 0.0;

        double change_interval_emulated_ms = 0.0;
        double change_interval_host_ms = 0.0;

        if (changed) {
            if (have_last_change) {
                change_interval_emulated_ms =
                        emulated_ms - last_change_emulated_ms;
                change_interval_host_ms =
                        std::chrono::duration<double, std::milli>(
                                host_now - last_change_host).count();
            }
            last_change_emulated_ms = emulated_ms;
            last_change_host = host_now;
            have_last_change = true;
            consecutive_repeats = 0;
        } else {
            ++consecutive_repeats;
        }

        output << std::fixed << std::setprecision(6)
               << ++scanout_count << ','
               << emulated_ms << ',' << host_elapsed_ms << ','
               << scanout_interval_emulated_ms << ','
               << scanout_interval_host_ms << ','
               << static_cast<int>(changed) << ','
               << static_cast<int>(pixels_changed) << ','
               << static_cast<int>(visible_palette_changed) << ','
               << static_cast<int>(geometry_changed) << ','
               << consecutive_repeats << ','
               << change_interval_emulated_ms << ','
               << change_interval_host_ms << ','
               << width << ',' << height << ',' << pixel_format << ','
               << std::hex << std::setfill('0') << std::setw(16) << pixel_hash
               << ',' << std::setw(16) << visible_palette_hash
               << ',' << std::setw(16) << visible_hash
               << std::dec << std::setfill(' ') << '\n';

        // Keep the file observable during gameplay without flushing on every
        // 70 Hz scanout. Normal stream destruction flushes remaining rows.
        if (scanout_count % 32 == 0) {
            output.flush();
        }

        previous_emulated_ms = emulated_ms;
        previous_host = host_now;
        previous_width = width;
        previous_height = height;
        previous_pitch = pitch;
        previous_pixel_format = pixel_format;
        previous_pixel_hash = pixel_hash;
        previous_palette_hash = visible_palette_hash;
        previous_visible_hash = visible_hash;
        have_previous = true;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr uint64_t FnvOffset = 14695981039346656037ULL;
    static constexpr uint64_t FnvPrime = 1099511628211ULL;

    Mk2FrameProbe()
    {
        const char* setting = std::getenv("DOSBOX_MK2_TRACE");
        if (!setting || !*setting || std::strcmp(setting, "0") == 0) {
            return;
        }
        const std::string trace_path = (std::strcmp(setting, "1") == 0)
                                               ? "mk2_timing.csv"
                                               : setting;
        const auto dot = trace_path.find_last_of('.');
        const auto sep = trace_path.find_last_of("/\\");
        const auto insert_pos = (dot != std::string::npos &&
                                 (sep == std::string::npos || dot > sep))
                                        ? dot
                                        : trace_path.size();
        const std::string frame_path = trace_path.substr(0, insert_pos) +
                                       "_frames" +
                                       (insert_pos < trace_path.size()
                                                ? trace_path.substr(insert_pos)
                                                : ".csv");

        output.open(frame_path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "scanout_index,emulated_ms,host_elapsed_ms,"
                  "scanout_interval_emulated_ms,scanout_interval_host_ms,"
                  "changed,pixels_changed,visible_palette_changed,"
                  "geometry_changed,consecutive_repeats,"
                  "change_interval_emulated_ms,change_interval_host_ms,"
                  "width,height,pixel_format,pixel_hash,visible_palette_hash,visible_hash\n";
        enabled = true;
        start_host = Clock::now();
    }

    bool enabled = false;
    bool have_previous = false;
    bool have_last_change = false;
    std::ofstream output = {};
    Clock::time_point start_host = {};
    Clock::time_point previous_host = {};
    Clock::time_point last_change_host = {};
    double previous_emulated_ms = 0.0;
    double last_change_emulated_ms = 0.0;
    uint64_t scanout_count = 0;
    uint64_t consecutive_repeats = 0;
    uint64_t previous_pixel_hash = 0;
    uint64_t previous_palette_hash = 0;
    uint64_t previous_visible_hash = 0;
    int previous_width = 0;
    int previous_height = 0;
    int previous_pitch = 0;
    int previous_pixel_format = 0;
};

#endif // DOSBOX_MK2_FRAME_PROBE_H
