// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in VGA register timeline for DOSBox Staging / MK2 timing research.
//
// The probe observes accepted register writes, VGA latch events and the
// resulting source-frame boundaries. It does NOT modify VGA state, CPU cycles,
// IRQ scheduling, memory mappings or the game's original binary.
#ifndef DOSBOX_MK2_VGA_REGS_PROBE_H
#define DOSBOX_MK2_VGA_REGS_PROBE_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "hardware/pic.h"

// CSV timestamp is the DOSBox emulated PIC clock, not host presentation time.
// Scalar numeric fields use decimal notation. Blank fields are represented
// by -1, avoiding ambiguous zero values for registers or palette index 0.
class Mk2VgaRegsProbe final {
public:
    static Mk2VgaRegsProbe& Get()
    {
        static Mk2VgaRegsProbe instance = {};
        return instance;
    }

    void OnCrtcStartWrite(const uint8_t index,
                          const uint8_t value,
                          const uint32_t programmed,
                          const uint32_t latched)
    {
        Record("crtc_start_write",
               index,
               value,
               programmed,
               latched);
    }

    void OnStartLatch(const uint32_t programmed, const uint32_t latched)
    {
        Record("start_latch", -1, -1, programmed, latched);
    }

    void OnPanningWrite(const uint8_t value,
                        const uint32_t programmed,
                        const uint32_t latched)
    {
        Record("pel_pan_write", 0x13, value, -1, -1, programmed, latched);
    }

    void OnPanningLatch(const uint32_t programmed, const uint32_t latched)
    {
        Record("pan_latch", -1, -1, -1, -1, programmed, latched);
    }

    void OnDacRgbCommit(const uint8_t index,
                        const uint8_t red,
                        const uint8_t green,
                        const uint8_t blue)
    {
        Record("dac_rgb_commit", 0x3c9, -1, -1, -1, -1, -1,
               index, red, green, blue);
    }

    void OnDacMaskWrite(const uint8_t value)
    {
        Record("dac_mask_write", 0x3c6, value);
    }

    void OnAttrPaletteWrite(const uint8_t index,
                            const uint8_t raw_value,
                            const uint8_t mapped_dac_index)
    {
        // The palette mapping may change without modifying any DAC RGB entry.
        Record("attr_palette_map", index, raw_value, -1, -1, -1, -1,
               mapped_dac_index);
    }

    void OnVgaRefresh(const double timestamp,
                      const uint32_t programmed,
                      const uint32_t latched,
                      const uint32_t programmed_pan,
                      const uint32_t latched_pan)
    {
        Record("vga_refresh", -1, -1, programmed, latched,
               programmed_pan, latched_pan, -1, -1, -1, -1, timestamp);
    }

    void OnCompletedScanout(const double timestamp,
                            const uint32_t programmed,
                            const uint32_t latched,
                            const uint32_t programmed_pan,
                            const uint32_t latched_pan)
    {
        Record("scanout_complete", -1, -1, programmed, latched,
               programmed_pan, latched_pan, -1, -1, -1, -1, timestamp);
    }

private:
    Mk2VgaRegsProbe()
    {
        const char* setting = std::getenv("DOSBOX_MK2_TRACE");
        if (!setting || !*setting || std::strcmp(setting, "0") == 0) {
            return;
        }

        const std::string trace = std::strcmp(setting, "1") == 0
                                          ? "mk2_timing.csv"
                                          : setting;
        const auto sep = trace.find_last_of("/\\");
        const auto dot = trace.find_last_of('.');
        const auto cut = dot != std::string::npos &&
                                 (sep == std::string::npos || dot > sep)
                                 ? dot
                                 : trace.size();

        const auto path = trace.substr(0, cut) + "_regs" +
                          (cut < trace.size() ? trace.substr(cut) : ".csv");
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }

        output << "event_index,emulated_ms,event,register,value,"
                  "display_start_programmed,display_start_latched,"
                  "panning_programmed,panning_latched,dac_index,"
                  "dac_red6,dac_green6,dac_blue6\n";
        enabled = true;
    }

    void Record(const char* event,
                const int reg = -1,
                const int value = -1,
                const int programmed_start = -1,
                const int latched_start = -1,
                const int programmed_pan = -1,
                const int latched_pan = -1,
                const int dac_index = -1,
                const int red = -1,
                const int green = -1,
                const int blue = -1,
                const double supplied_time = -1.0)
    {
        if (!enabled) {
            return;
        }
        const double emulated_ms =
                supplied_time >= 0.0 ? supplied_time : PIC_FullIndex();

        output << ++event_count << ','
               << std::fixed << std::setprecision(6) << emulated_ms << ','
               << event << ',' << reg << ',' << value << ','
               << programmed_start << ',' << latched_start << ','
               << programmed_pan << ',' << latched_pan << ','
               << dac_index << ',' << red << ',' << green << ',' << blue
               << '\n';
        if ((event_count & 255u) == 0) {
            output.flush();
        }
    }

    bool enabled = false;
    std::ofstream output = {};
    uint64_t event_count = 0;
};

#endif // DOSBOX_MK2_VGA_REGS_PROBE_H
