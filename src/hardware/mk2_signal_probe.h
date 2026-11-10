// SPDX-License-Identifier: GPL-2.0-or-later
// MK2 private research: trace the writer of the work-ready DWORD consumed
// at 0180:00205489. Does not modify guest memory or simulation timing.
// This is an observational watchpoint, NOT a game simulation tick counter.
#ifndef DOSBOX_MK2_SIGNAL_PROBE_H
#define DOSBOX_MK2_SIGNAL_PROBE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <string>

#include "cpu/cpu.h"
#include "hardware/memory.h"
#include "hardware/pic.h"

// This global is defined in pic.cpp, false unless the probe is constructed
// with DOSBOX_MK2_SIGNAL_WATCH=1 and DOSBOX_MK2_TRACE set. The normal CPU
// memory-write helpers check it before calling this probe. No overhead from
// hashing, file I/O or emulated-memory reads on the uninstrumented path.
extern bool Mk2SignalWatchActive;

class Mk2SignalProbe final {
public:
    static Mk2SignalProbe& Get()
    {
        static Mk2SignalProbe instance = {};
        return instance;
    }

    // Called before IRQ0 dispatch, while the interrupted program's CS:EIP
    // and DS still identify the busy-wait consumer. In normal core the
    // EIP values 205489/8F/92/94 are inside the actual wait loop.
    void OnBeforeIrq0(const double emulated_ms)
    {
        if (!InWindow(emulated_ms) || !IsNormalCore() ||
            !cpu.pmode || SegValue(cs) != 0x0180 ||
            SegPhys(cs) != 0 || reg_eip < 0x00205489u ||
            reg_eip > 0x00205494u) {
            return;
        }

        const auto ds_base = static_cast<uint64_t>(SegPhys(ds));
        const auto pointer_addr = ds_base + PointerOffset;
        if (pointer_addr > std::numeric_limits<PhysPt>::max()) {
            return;
        }

        // This is a DS-relative pointer read, not CS-based addressing.
        // A valid DOS extender mapping must exist for the consumer.
        const auto pointer = mem_readd<MemOpMode::SkipBreakpoints>(
                static_cast<PhysPt>(pointer_addr));
        const auto target = ds_base + pointer + TargetOffset;
        if (!pointer || target > std::numeric_limits<PhysPt>::max()) {
            return;
        }
        const bool retargeted = !resolved ||
                                target_linear != static_cast<PhysPt>(target) ||
                                pointer_location != static_cast<PhysPt>(pointer_addr) ||
                                ds_selector != SegValue(ds);
        pointer_location = static_cast<PhysPt>(pointer_addr);
        pointer_value = pointer;
        target_linear = static_cast<PhysPt>(target);
        ds_selector = SegValue(ds);
        data_segment_base = static_cast<uint32_t>(ds_base);
        resolved = true;

        if (retargeted) {
            const auto observed = mem_readd<MemOpMode::SkipBreakpoints>(
                    target_linear);
            Append("resolve", emulated_ms, 0, 0, observed, observed);
        }
    }

    // Called BEFORE an actual guest memory write in the normal core.
    // Return true only when the write overlaps the 4-byte pointer or
    // watched target. Cross-page writes will be observed as byte writes.
    bool BeforeGuestStore(const PhysPt address, const uint8_t size)
    {
        if (!resolved || !IsNormalCore() || !cpu.pmode || size == 0) {
            return false;
        }
        // Extremely cheap address comparison first; do not calculate
        // PIC_FullIndex() for the millions of unrelated guest writes.
        const bool touches_target = Overlaps(address, size, target_linear);
        const bool touches_pointer = Overlaps(address, size, pointer_location);
        if ((!touches_target && !touches_pointer) ||
            !InWindow(PIC_FullIndex())) {
            return false;
        }

        pending.active = true;
        pending.pointer_write = touches_pointer;
        pending.addr = address;
        pending.size = size;
        pending.before = mem_readd<MemOpMode::SkipBreakpoints>(
                touches_pointer ? pointer_location : target_linear);
        pending.emulated_ms = PIC_FullIndex();
        pending.cs = SegValue(cs);
        pending.cs_base = SegPhys(cs);
        pending.eip = reg_eip; // interpreter's instruction-start EIP
        pending.ds = SegValue(ds);
        pending.ds_base = SegPhys(ds);
        return true;
    }

    // AFTER the original store: record the actual 32-bit target value
    // instead of assuming a byte/word/quad write had the expected effect.
    void AfterGuestStore()
    {
        if (!pending.active) {
            return;
        }
        const auto copy = pending;
        pending.active = false;
        const auto location = copy.pointer_write ? pointer_location
                                                 : target_linear;
        const auto after = mem_readd<MemOpMode::SkipBreakpoints>(location);
        if (after != copy.before) {
            Append(copy.pointer_write ? "pointer_write" : "signal_write",
                   copy.emulated_ms,
                   copy.addr,
                   copy.size,
                   copy.before,
                   after,
                   copy.cs,
                   copy.cs_base,
                   copy.eip,
                   copy.ds,
                   copy.ds_base);
        } else {
            // Retain equal-value writes, including explicit consumer clears,
            // for interpreting writer frequency and missed transitions.
            Append(copy.pointer_write ? "pointer_same" : "signal_same",
                   copy.emulated_ms,
                   copy.addr,
                   copy.size,
                   copy.before,
                   after,
                   copy.cs,
                   copy.cs_base,
                   copy.eip,
                   copy.ds,
                   copy.ds_base);
        }

        if (copy.pointer_write) {
            // The pointer can change without IRQ0. Resolve the new
            // target using the DS base captured from the consumer.
            const uint64_t next_target =
                    static_cast<uint64_t>(data_segment_base) +
                    after + TargetOffset;
            if (after && next_target <= std::numeric_limits<PhysPt>::max()) {
                pointer_value = after;
                target_linear = static_cast<PhysPt>(next_target);
            } else {
                resolved = false;
            }
        }
    }

    void OnClockTick(const uint32_t ms)
    {
        if (enabled && ms % 1000u == 0) {
            Flush();
        }
    }

private:
    static constexpr uint32_t PointerOffset = 0x0027177cu;
    static constexpr uint32_t TargetOffset = 0x24u;
    static constexpr double StartMs = 30000.0;
    static constexpr double EndMs = 150000.0;
    static constexpr std::size_t BufferCount = 512;

    struct Entry {
        const char* event = "";
        double emulated_ms = 0;
        uint32_t pointer_location = 0;
        uint32_t pointer_value = 0;
        uint32_t target_linear = 0;
        uint32_t ds_base_consumer = 0;
        uint32_t store_linear = 0;
        uint32_t before = 0;
        uint32_t after = 0;
        uint32_t cs_base = 0;
        uint32_t eip = 0;
        uint32_t writer_ds_base = 0;
        uint16_t consumer_ds = 0;
        uint16_t cs = 0;
        uint16_t writer_ds = 0;
        uint8_t size = 0;
    };

    struct Pending {
        bool active = false;
        bool pointer_write = false;
        PhysPt addr = 0;
        uint8_t size = 0;
        uint32_t before = 0;
        double emulated_ms = 0;
        uint16_t cs = 0;
        uint32_t cs_base = 0;
        uint32_t eip = 0;
        uint16_t ds = 0;
        uint32_t ds_base = 0;
    };

    Mk2SignalProbe()
    {
        const char* setting = std::getenv("DOSBOX_MK2_SIGNAL_WATCH");
        const char* trace_setting = std::getenv("DOSBOX_MK2_TRACE");
        if (!setting || std::strcmp(setting, "1") != 0 ||
            !trace_setting || !*trace_setting ||
            std::strcmp(trace_setting, "0") == 0) {
            return;
        }
        const std::string trace = std::strcmp(trace_setting, "1") == 0
                                          ? "mk2_timing.csv"
                                          : trace_setting;
        const auto slash = trace.find_last_of("/\\");
        const auto dot = trace.find_last_of('.');
        const auto cut = dot != std::string::npos &&
                                 (slash == std::string::npos || dot > slash)
                                 ? dot : trace.size();
        const auto path = trace.substr(0, cut) + "_signal" +
                          (cut < trace.size() ? trace.substr(cut) : ".csv");
        output.open(path, std::ios::out | std::ios::trunc);
        if (!output.is_open()) {
            return;
        }
        output << "event_index,emulated_ms,event,consumer_ds,"
                  "consumer_ds_base,pointer_location,pointer_value,"
                  "target_linear,store_linear,store_size,before,after,"
                  "writer_cs,writer_cs_base,writer_eip,writer_ds,"
                  "writer_ds_base\n";
        enabled = true;
        Mk2SignalWatchActive = true;
    }

    ~Mk2SignalProbe()
    {
        Mk2SignalWatchActive = false;
        Flush();
    }

    bool IsNormalCore() const
    {
        return cpudecoder == &CPU_Core_Normal_Run;
    }

    bool InWindow(const double ms) const
    {
        return enabled && ms >= StartMs && ms < EndMs;
    }

    static bool Overlaps(const uint32_t address, const uint8_t size,
                         const uint32_t watched)
    {
        const uint64_t write_end = static_cast<uint64_t>(address) + size;
        const uint64_t watch_end = static_cast<uint64_t>(watched) + 4u;
        return address < watch_end && watched < write_end;
    }

    void Append(const char* event, const double at_ms,
                const uint32_t addr, const uint8_t size,
                const uint32_t before, const uint32_t after,
                const uint16_t writer_cs = 0,
                const uint32_t writer_cs_base = 0,
                const uint32_t writer_eip = 0,
                const uint16_t writer_ds = 0,
                const uint32_t writer_ds_base = 0)
    {
        if (buffer_size == BufferCount) {
            Flush();
        }
        auto& entry = buffer[buffer_size++];
        entry.event = event;
        entry.emulated_ms = at_ms;
        entry.consumer_ds = ds_selector;
        entry.ds_base_consumer = data_segment_base;
        entry.pointer_location = pointer_location;
        entry.pointer_value = pointer_value;
        entry.target_linear = target_linear;
        entry.store_linear = addr;
        entry.size = size;
        entry.before = before;
        entry.after = after;
        entry.cs = writer_cs;
        entry.cs_base = writer_cs_base;
        entry.eip = writer_eip;
        entry.writer_ds = writer_ds;
        entry.writer_ds_base = writer_ds_base;
    }

    void Flush()
    {
        if (!enabled || buffer_size == 0) {
            return;
        }
        for (std::size_t i = 0; i < buffer_size; ++i) {
            const auto& entry = buffer[i];
            output << ++event_count << ','
                   << std::fixed << std::setprecision(6)
                   << entry.emulated_ms << ',' << entry.event
                   << ',' << entry.consumer_ds
                   << ',' << entry.ds_base_consumer
                   << ',' << entry.pointer_location
                   << ',' << entry.pointer_value
                   << ',' << entry.target_linear
                   << ',' << entry.store_linear
                   << ',' << static_cast<unsigned>(entry.size)
                   << ',' << entry.before << ',' << entry.after
                   << ',' << entry.cs
                   << ',' << entry.cs_base
                   << ',' << entry.eip
                   << ',' << entry.writer_ds
                   << ',' << entry.writer_ds_base
                   << '\n';
        }
        output.flush();
        buffer_size = 0;
    }

    bool enabled = false;
    bool resolved = false;
    uint16_t ds_selector = 0;
    uint32_t data_segment_base = 0;
    uint32_t pointer_location = 0;
    uint32_t pointer_value = 0;
    uint32_t target_linear = 0;
    std::size_t buffer_size = 0;
    uint64_t event_count = 0;
    Pending pending = {};
    std::ofstream output = {};
    std::array<Entry, BufferCount> buffer = {};
};

#endif // DOSBOX_MK2_SIGNAL_PROBE_H
