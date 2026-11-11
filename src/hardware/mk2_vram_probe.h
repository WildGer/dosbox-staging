// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental read-only telemetry for CPU writes to VGA page handlers.
// All counters are opt-in via DOSBOX_MK2_TRACE; the guest is not modified.
#ifndef DOSBOX_MK2_VRAM_PROBE_H
#define DOSBOX_MK2_VRAM_PROBE_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "hardware/pic.h"
#include "hardware/mk2_guest_probe.h"

// Collects one lightweight counter update per guest VGA write operation,
// then emits at most one CSV row per emulated millisecond. Importantly,
// direct-mapped VGA memory bypasses the page-handler write methods; the CSV
// reports that limitation instead of silently claiming complete coverage.
class Mk2VramProbe final {
public:
    static Mk2VramProbe& Get()
    {
        static Mk2VramProbe probe = {};
        return probe;
    }

    void OnWrite(const unsigned int guest_bytes)
    {
        if (!enabled) { return; }

        if (!write_ops) {
            first_write_ms = PIC_FullIndex();
            last_write_sample_ms = first_write_ms;
            Mk2GuestProbe::Get().OnVramBurst(first_write_ms);
        }
        ++write_ops;
        guest_write_bytes += guest_bytes;
        if (guest_bytes == 1) {
            ++write8;
        } else if (guest_bytes == 2) {
            ++write16;
        } else if (guest_bytes == 4) {
            ++write32;
        }

        // Sample once per 32 writes, rather than divide CPU cycle counts
        // on every VGA byte store. Timing here is observational only.
        if ((write_ops & 31u) == 0) {
            last_write_sample_ms = PIC_FullIndex();
        }
    }

    void OnIrq0(const double emulated_ms)
    {
        if (!enabled) { return; }
        ++irq0_dispatches;
        last_irq0_ms = emulated_ms;
    }

    void OnVgaRefresh(const double emulated_ms)
    {
        if (!enabled) { return; }
        ++vga_refreshes;
        last_vga_ms = emulated_ms;
    }

    void OnCompletedScanout(const double emulated_ms)
    {
        if (!enabled) { return; }
        ++scanouts;
        last_scanout_ms = emulated_ms;
    }

    // Set from VGA_SetupHandlers. 0 signals direct-mapped VRAM may bypass
    // OnWrite; 1 means the currently selected conventional VGA page handler
    // uses intercepted write callbacks. Does not alter the memory mapping.
    void OnVgaMapping(const int mode, const bool intercepts_writes)
    {
        if (!enabled) { return; }
        if (vga_mode != mode || page_handler_covered != intercepts_writes) {
            ++mapping_changes;
        }
        vga_mode = mode;
        page_handler_covered = intercepts_writes;
    }

    // PIC_Ticks was incremented just before this callback, so all counters
    // belong to the completed simulated millisecond ending at end_ms.
    // Nothing is written to disk during individual VGA stores.
    void OnClockTick(const uint32_t end_ms)
    {
        if (!enabled) { return; }

        if (write_ops || irq0_dispatches || vga_refreshes || scanouts ||
            mapping_changes) {
            output << end_ms << ','
                   << write_ops << ',' << guest_write_bytes << ','
                   << write8 << ',' << write16 << ',' << write32 << ','
                   << std::fixed << std::setprecision(6)
                   << first_write_ms << ',' << last_write_sample_ms << ','
                   << irq0_dispatches << ',' << last_irq0_ms << ','
                   << vga_refreshes << ',' << last_vga_ms << ','
                   << scanouts << ',' << last_scanout_ms << ','
                   << vga_mode << ',' << static_cast<int>(page_handler_covered)
                   << ',' << mapping_changes << '\n';
            ++output_rows;
            if ((output_rows & 127u) == 0) {
                output.flush();
            }
        }

        write_ops = 0;
        guest_write_bytes = 0;
        write8 = 0;
        write16 = 0;
        write32 = 0;
        first_write_ms = 0;
        last_write_sample_ms = 0;
        irq0_dispatches = 0;
        last_irq0_ms = 0;
        vga_refreshes = 0;
        last_vga_ms = 0;
        scanouts = 0;
        last_scanout_ms = 0;
        mapping_changes = 0;
    }

private:
    Mk2VramProbe()
    {
        const char* setting = std::getenv("DOSBOX_MK2_TRACE");
        if (!setting || !*setting || std::strcmp(setting, "0") == 0) {
            return;
        }
        const std::string filename = (std::strcmp(setting, "1") == 0)
                                             ? "mk2_timing.csv"
                                             : setting;
        const auto dot = filename.find_last_of('.');
        const auto sep = filename.find_last_of("/\\");
        const auto cut = (dot != std::string::npos &&
                          (sep == std::string::npos || dot > sep))
                                 ? dot
                                 : filename.size();
        const std::string vram_path =
                filename.substr(0, cut) + "_vram" +
                (cut < filename.size() ? filename.substr(cut) : ".csv");

        output.open(vram_path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "emulated_ms_end,write_ops,guest_write_bytes,"
                  "write8,write16,write32,first_write_ms,"
                  "last_write_sample_ms,irq0_dispatches,last_irq0_ms,"
                  "vga_refreshes,last_vga_ms,scanouts,last_scanout_ms,"
                  "vga_mode,page_handler_covered,mapping_changes\n";
        enabled = true;
    }

    bool enabled = false;
    bool page_handler_covered = false;
    int vga_mode = -1;

    std::ofstream output = {};
    uint64_t output_rows = 0;
    uint64_t write_ops = 0;
    uint64_t guest_write_bytes = 0;
    uint64_t write8 = 0;
    uint64_t write16 = 0;
    uint64_t write32 = 0;
    uint64_t irq0_dispatches = 0;
    uint64_t vga_refreshes = 0;
    uint64_t scanouts = 0;
    uint64_t mapping_changes = 0;
    double first_write_ms = 0.0;
    double last_write_sample_ms = 0.0;
    double last_irq0_ms = 0.0;
    double last_vga_ms = 0.0;
    double last_scanout_ms = 0.0;
};

#endif // DOSBOX_MK2_VRAM_PROBE_H
