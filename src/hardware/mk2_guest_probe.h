// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in, event-correlated guest execution context samples for MK2 research.
// Does not hook an MK2 instruction, modify its memory, or alter timers.
// All captured PCs are observational; dynamic cores can expose a stale EIP.
#ifndef DOSBOX_MK2_GUEST_PROBE_H
#define DOSBOX_MK2_GUEST_PROBE_H

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

#include "cpu/cpu.h"
#include "hardware/pic.h"

// Samples guest execution context at selected events. This profiler does
// NOT know where MK2 advances its simulation. Correlate locations with
// later disassembly/debugger work before labelling any location as a tick.
//
// No allocation or file I/O in event hooks: fixed-size buffer is drained
// once per second or when filled. Trace disabled by default.
class Mk2GuestProbe final {
public:
    static Mk2GuestProbe& Get()
    {
        static Mk2GuestProbe probe = {};
        return probe;
    }

    // Called immediately before DOSBox delivers IRQ0 to the guest.
    // Snapshot is of the *interrupted* code, not the IRQ handler entry.
    void OnIrq0(const double emulated_ms)
    {
        if (!InWindow(emulated_ms)) {
            return;
        }
        Store(Event::Irq0Pre, emulated_ms, -1);
    }

    // Once per emulated millisecond bucket at the first observed VGA
    // page-handler write, not on each individual guest store.
    void OnVramBurst(const double emulated_ms)
    {
        if (!InWindow(emulated_ms)) {
            return;
        }
        Store(Event::VramWrite, emulated_ms, -1);
    }

    // Called on each read of 3DAh, but capture one of every 2048 reads,
    // plus a sample on a VSYNC-low -> VSYNC-high transition.
    void OnVgaStatus(const uint8_t status, const double emulated_ms)
    {
        if (!InWindow(emulated_ms)) {
            return;
        }
        ++status_reads;
        const bool high = (status & 0x08u) != 0;
        const bool rising = high && !last_status_high;
        last_status_high = high;
        if (rising || (status_reads % 2048u) == 0) {
            Store(rising ? Event::StatusRising : Event::StatusSample,
                  emulated_ms,
                  status);
        }
    }

    // Sampling by the scheduler clock is distinct from event-triggered
    // sampling (which can be strongly biased toward graphics loops).
    void OnClockTick(const uint32_t tick)
    {
        if (!enabled) {
            return;
        }
        if (tick % 10u == 0 && InWindow(static_cast<double>(tick))) {
            Store(Event::Clock10ms, static_cast<double>(tick), -1);
        }
        if (tick % 1000u == 0) {
            Flush();
        }
    }

private:
    enum class Event : uint8_t {
        Irq0Pre,
        VramWrite,
        StatusRising,
        StatusSample,
        Clock10ms,
    };

    // Fixed, tiny samples of the user's OWN executable as it is mapped
    // into guest memory. They are written only to a local trace alongside
    // the other files, NEVER embedded in this repository.
    struct CodeSite {
        const char* label;
        uint32_t exact_eip;
        uint8_t event_kind;
    };
    static constexpr std::size_t CodeBytes = 64;
    static constexpr std::array<CodeSite, 6> Sites = {{
            {"irq_wait_loop", 0x00205489u,
             static_cast<uint8_t>(Event::Irq0Pre)},
            {"vga_3da_poll", 0x002196d5u,
             static_cast<uint8_t>(Event::StatusSample)},
            {"vram_draw_479a", 0x0020479au,
             static_cast<uint8_t>(Event::VramWrite)},
            {"vram_draw_461a", 0x0020461au,
             static_cast<uint8_t>(Event::VramWrite)},
            {"vram_draw_42fc", 0x002042fcu,
             static_cast<uint8_t>(Event::VramWrite)},
            {"periodic_c81e", 0x0020c81eu,
             static_cast<uint8_t>(Event::Clock10ms)},
    }};
    struct CodeSnapshot {
        bool captured = false;
        double emulated_ms = 0.0;
        uint16_t selector = 0;
        uint32_t cs_base = 0;
        uint32_t observed_eip = 0;
        std::array<uint8_t, CodeBytes> bytes = {};
    };

    struct Sample {
        double emulated_ms = 0;
        uint32_t cs_base = 0;
        uint32_t eip = 0;
        uint32_t eax = 0;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edx = 0;
        uint32_t esi = 0;
        uint32_t edi = 0;
        uint32_t ebp = 0;
        uint32_t esp = 0;
        uint16_t cs = 0;
        uint8_t kind = 0;
        uint8_t protected_mode = 0;
        uint8_t privilege_level = 0;
        int16_t vga_status = -1;
    };

    static constexpr std::size_t BufferSize = 2048;
    static constexpr double WindowStartMs = 30000.0;
    static constexpr double WindowEndMs = 150000.0;

    Mk2GuestProbe()
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
        const auto path = trace.substr(0, cut) + "_guest" +
                          (cut < trace.size() ? trace.substr(cut) : ".csv");
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "sample_index,emulated_ms,event,cs,cs_base,eip,"
                  "linear_cs_eip,protected_mode,cpl,vga_status,"
                  "eax,ebx,ecx,edx,esi,edi,ebp,esp\n";
        // Separate local code snapshot CSV; it remains empty (header only)
        // until a known guest code site is actually encountered.
        const auto code_path = trace.substr(0, cut) + "_guest_code" +
                               (cut < trace.size() ? trace.substr(cut)
                                                  : ".csv");
        code_output.open(code_path, std::ios::out | std::ios::trunc);
        if (code_output.is_open()) {
            code_output << "site,emulated_ms,cs,cs_base,observed_eip,"
                           "snapshot_start,bytes_hex\n";
        }
        enabled = true;
    }

    ~Mk2GuestProbe()
    {
        Flush();
        FlushCode();
    }

    bool InWindow(const double emulated_ms) const
    {
        return enabled && emulated_ms >= WindowStartMs &&
               emulated_ms < WindowEndMs;
    }

    void Store(const Event kind, const double emulated_ms, const int status)
    {
        if (buffer_count == BufferSize) {
            Flush();
        }
        auto& sample = buffer[buffer_count++];
        sample.kind = static_cast<uint8_t>(kind);
        sample.emulated_ms = emulated_ms;
        sample.cs = SegValue(cs);
        sample.cs_base = SegPhys(cs);
        sample.eip = reg_eip;
        sample.protected_mode = static_cast<uint8_t>(cpu.pmode);
        sample.privilege_level = static_cast<uint8_t>(cpu.cpl);
        sample.vga_status = static_cast<int16_t>(status);
        sample.eax = reg_eax;
        sample.ebx = reg_ebx;
        sample.ecx = reg_ecx;
        sample.edx = reg_edx;
        sample.esi = reg_esi;
        sample.edi = reg_edi;
        sample.ebp = reg_ebp;
        sample.esp = reg_esp;
        MaybeCaptureCode(sample);
    }

    // Match locations found independently in both dynamic and normal
    // CPU-core samples. Use only a 64-byte snippet starting at an
    // interpreter-confirmed EIP. No CPU instruction hook or guest writes.
    // Captures are read via regular DOSBox guest linear memory paging.
    void MaybeCaptureCode(const Sample& sample)
    {
        if (!code_output.is_open() || !sample.protected_mode ||
            sample.cs != 0x0180u || sample.cs_base != 0) {
            return;
        }
        for (std::size_t i = 0; i < Sites.size(); ++i) {
            const auto& site = Sites[i];
            auto& snapshot = code_snapshots[i];
            if (snapshot.captured || sample.kind != site.event_kind) {
                continue;
            }

            // Normal core shows four tightly spaced interrupt PCs.
            // A code snapshot near the first one is sufficient.
            const bool match = i == 0
                    ? sample.eip >= site.exact_eip &&
                      sample.eip <= site.exact_eip + 11u
                    : sample.eip == site.exact_eip;
            if (!match) {
                continue;
            }
            snapshot.emulated_ms = sample.emulated_ms;
            snapshot.selector = sample.cs;
            snapshot.cs_base = sample.cs_base;
            snapshot.observed_eip = sample.eip;
            for (std::size_t b = 0; b < CodeBytes; ++b) {
                snapshot.bytes[b] = mem_readb(
                        static_cast<PhysPt>(sample.cs_base +
                                            site.exact_eip + b));
            }
            snapshot.captured = true;
        }
    }

    void FlushCode()
    {
        if (!code_output.is_open()) {
            return;
        }
        for (std::size_t i = 0; i < Sites.size(); ++i) {
            const auto& snapshot = code_snapshots[i];
            if (!snapshot.captured) {
                continue;
            }
            code_output << Sites[i].label << ','
                        << std::fixed << std::setprecision(6)
                        << snapshot.emulated_ms << ','
                        << snapshot.selector << ','
                        << snapshot.cs_base << ','
                        << snapshot.observed_eip << ','
                        << Sites[i].exact_eip << ','
                        << std::hex << std::setfill('0');
            for (const auto byte : snapshot.bytes) {
                code_output << std::setw(2) << static_cast<unsigned>(byte);
            }
            code_output << std::dec << std::setfill(' ') << '\n';
        }
        code_output.flush();
    }

    static const char* Name(const uint8_t event)
    {
        switch (static_cast<Event>(event)) {
        case Event::Irq0Pre: return "irq0_pre";
        case Event::VramWrite: return "vram_first_write_ms";
        case Event::StatusRising: return "vga_status_rising";
        case Event::StatusSample: return "vga_status_sample";
        case Event::Clock10ms: return "clock_10ms";
        }
        return "unknown";
    }

    void Flush()
    {
        if (!enabled || !buffer_count) {
            return;
        }
        for (std::size_t i = 0; i < buffer_count; ++i) {
            const auto& sample = buffer[i];
            output << ++written_count << ','
                   << std::fixed << std::setprecision(6)
                   << sample.emulated_ms << ',' << Name(sample.kind)
                   << ',' << sample.cs
                   << ',' << sample.cs_base
                   << ',' << sample.eip
                   << ',' << static_cast<uint64_t>(sample.cs_base) + sample.eip
                   << ',' << static_cast<int>(sample.protected_mode)
                   << ',' << static_cast<int>(sample.privilege_level)
                   << ',' << sample.vga_status
                   << ',' << sample.eax
                   << ',' << sample.ebx
                   << ',' << sample.ecx
                   << ',' << sample.edx
                   << ',' << sample.esi
                   << ',' << sample.edi
                   << ',' << sample.ebp
                   << ',' << sample.esp << '\n';
        }
        buffer_count = 0;
        output.flush();
    }

    bool enabled = false;
    bool last_status_high = false;
    uint64_t status_reads = 0;
    uint64_t written_count = 0;
    std::size_t buffer_count = 0;
    std::ofstream output = {};
    std::ofstream code_output = {};
    std::array<Sample, BufferSize> buffer = {};
    std::array<CodeSnapshot, Sites.size()> code_snapshots = {};
};

#endif // DOSBOX_MK2_GUEST_PROBE_H
