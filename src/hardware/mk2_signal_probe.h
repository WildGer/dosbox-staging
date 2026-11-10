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
extern bool Mk2SignalInstructionActive;

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
            // Never compare instruction snapshots across a pointer change.
            instruction_previous_valid = false;
        }

        // A short per-instruction data check on roughly every 60th IRQ0
        // (about one diagnostic window per second) catches producers that
        // bypass ordinary guest memory-store helpers. The normal core
        // executes the next opcode *after* IRQ dispatch, so this includes
        // the beginning of the IRQ handler without tracing the entire run.
        if ((++irq0_observed % 60u) == 1u &&
            !Mk2SignalInstructionActive) {
            instruction_budget = 16000;
            instruction_previous_valid = false;
            Mk2SignalInstructionActive = true;
        }
    }

    // Called BEFORE an actual guest memory write in the normal core.
    // Return true only when the write overlaps the 4-byte pointer or
    // watched target. Cross-page writes will be observed as byte writes.
    bool BeforeGuestStore(const PhysPt address, const uint8_t size)
    {
        // The IRQ0 signal producer may run in real mode, under a
        // different decoder, or inside a DOS extender callback. The old
        // protected-mode/normal-decoder restriction missed every observed
        // positive transition. Resolve the target under the known consumer
        // first, then watch stores to that *linear address* regardless of
        // the current CPU mode. CS:EIP attribution still needs validation.
        if (!resolved || size == 0) {
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
        // Keep one small, read-only snapshot of the real producer and
        // consumer continuation after the FIRST observed activation.
        if (!copy.pointer_write && copy.before == 0 && after != 0) {
            CaptureProducerCode(copy);
        }

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
                instruction_previous_valid = false;
            } else {
                resolved = false;
                Mk2SignalInstructionActive = false;
                instruction_budget = 0;
            }
        }
    }

    // Called at the start of an interpreted instruction. Compare the
    // watched DWORD to the value seen at the previous instruction
    // boundary, attributing a transition to the preceding guest EIP.
    // This will also catch writes through direct host-backed memory,
    // although a callback outside guest execution can change the DWORD.
    void OnInstructionBoundary()
    {
        if (!resolved || instruction_budget == 0) {
            Mk2SignalInstructionActive = false;
            return;
        }
        --instruction_budget;
        const auto value =
                mem_readd<MemOpMode::SkipBreakpoints>(target_linear);
        if (instruction_previous_valid && value != instruction_last_value) {
            Append("opcode_transition",
                   PIC_FullIndex(),
                   target_linear,
                   4,
                   instruction_last_value,
                   value,
                   instruction_last_cs,
                   instruction_last_cs_base,
                   instruction_last_eip,
                   instruction_last_ds,
                   instruction_last_ds_base);
            // We only need one positive producer instruction per short
            // window; disarm immediately after observing it.
            if (instruction_last_value == 0 && value != 0) {
                instruction_budget = 0;
            }
        }
        instruction_last_value = value;
        instruction_last_cs = SegValue(cs);
        instruction_last_cs_base = SegPhys(cs);
        instruction_last_eip = reg_eip;
        instruction_last_ds = SegValue(ds);
        instruction_last_ds_base = SegPhys(ds);
        instruction_previous_valid = true;

        if (instruction_budget == 0) {
            Mk2SignalInstructionActive = false;
        }
    }

    // Exact normal-core instruction boundary, validated by disassembly:
    // 0180:00205531 is JMP DWORD PTR DS:[ESI+0x1C].
    // The EIP is sampled BEFORE the jump executes, while ESI points
    // to the task node and its destination has not yet been loaded.
    void OnTaskDispatch()
    {
        if (!enabled || !IsNormalCore() || !cpu.pmode ||
            SegValue(cs) != 0x0180u || SegPhys(cs) != 0 ||
            reg_eip != 0x00205531u ||
            !InWindow(PIC_FullIndex())) {
            return;
        }
        const uint64_t node = static_cast<uint64_t>(SegPhys(ds)) +
                              static_cast<uint64_t>(reg_esi);
        if (node > static_cast<uint64_t>(
                           std::numeric_limits<PhysPt>::max()) - 0x20u) {
            return;
        }
        if (dispatch_count == DispatchBufferSize) {
            ++dispatch_dropped;
            return;
        }
        auto& row = dispatch_buffer[dispatch_count++];
        row.emulated_ms = PIC_FullIndex();
        row.task_offset = reg_esi;
        row.task_linear = static_cast<uint32_t>(node);
        row.ds = SegValue(ds);
        row.ds_base = SegPhys(ds);
        row.remaining = static_cast<int16_t>(
                mem_readw<MemOpMode::SkipBreakpoints>(
                        static_cast<PhysPt>(node + 0x06u)));
        row.next_offset = mem_readd<MemOpMode::SkipBreakpoints>(
                static_cast<PhysPt>(node));
        row.handler_eip = mem_readd<MemOpMode::SkipBreakpoints>(
                static_cast<PhysPt>(node + 0x1cu));
        row.ebp = reg_ebp;
        row.edi = reg_edi;
        row.cs = SegValue(cs);
    }

    void OnClockTick(const uint32_t ms)
    {
        if (enabled && ms % 1000u == 0) {
            Flush();
            FlushCode();
            FlushDispatch();
        }
    }

private:
    static constexpr uint32_t PointerOffset = 0x0027177cu;
    static constexpr uint32_t TargetOffset = 0x24u;
    static constexpr double StartMs = 30000.0;
    static constexpr double EndMs = 150000.0;
    static constexpr std::size_t BufferCount = 512;
    static constexpr std::size_t CodeBytes = 128;
    static constexpr std::size_t CodeSiteCount = 3;
    static constexpr std::size_t DispatchBufferSize = 4096;

    struct DispatchRow {
        double emulated_ms = 0;
        uint32_t task_offset = 0;
        uint32_t task_linear = 0;
        uint32_t next_offset = 0;
        uint32_t handler_eip = 0;
        uint32_t ebp = 0;
        uint32_t edi = 0;
        uint32_t ds_base = 0;
        int16_t remaining = 0;
        uint16_t ds = 0;
        uint16_t cs = 0;
    };

    struct CodeSnapshot {
        const char* site = "";
        bool captured = false;
        bool written = false;
        double emulated_ms = 0.0;
        uint16_t cs = 0;
        uint32_t cs_base = 0;
        uint32_t observed_eip = 0;
        uint32_t start_eip = 0;
        uint16_t irq0_vector_segment = 0;
        uint16_t irq0_vector_offset = 0;
        uint8_t protected_mode = 0;
        uint8_t code_bits = 32;
        std::array<uint8_t, CodeBytes> bytes = {};
    };

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
        const auto code_path = trace.substr(0, cut) + "_signal_code" +
                               (cut < trace.size() ? trace.substr(cut)
                                                  : ".csv");
        code_output.open(code_path, std::ios::out | std::ios::trunc);
        if (code_output.is_open()) {
            // First seven columns intentionally match the existing
            // scripts/extract_mk2_guest_code.py format.
            code_output << "site,emulated_ms,cs,cs_base,observed_eip,"
                           "snapshot_start,bytes_hex,protected_mode,"
                           "code_bits,ivt_irq0_segment,ivt_irq0_offset\n";
        }

        const auto dispatch_path = trace.substr(0, cut) + "_dispatch" +
                                   (cut < trace.size()
                                            ? trace.substr(cut)
                                            : ".csv");
        dispatch_output.open(dispatch_path,
                             std::ios::out | std::ios::trunc);
        if (dispatch_output.is_open()) {
            dispatch_output << "event_index,emulated_ms,event,cs,ds,"
                               "ds_base,task_offset,task_linear,"
                               "next_offset,remaining,handler_eip,"
                               "ebp,edi,dropped_count\n";
        }

        enabled = true;
        Mk2SignalWatchActive = true;
    }

    ~Mk2SignalProbe()
    {
        Mk2SignalInstructionActive = false;
        Mk2SignalWatchActive = false;
        Flush();
        FlushCode();
        FlushDispatch();
    }

    // The source at 09B0:08AF has now been observed through both
    // normal guest write helpers and independent opcode-boundary
    // sampling. Copy tiny snippets from the running user's guest RAM
    // once; never include game code bytes in this repository.
    void CaptureSite(const std::size_t index, const char* label,
                     const Pending& producer, const uint16_t selector,
                     const uint32_t base, const uint32_t observed_eip,
                     const uint32_t start_eip)
    {
        auto& snapshot = code_snapshots[index];
        const uint64_t start = static_cast<uint64_t>(base) + start_eip;
        if (snapshot.captured || start >
                static_cast<uint64_t>(std::numeric_limits<PhysPt>::max()) -
                        CodeBytes) {
            return;
        }
        snapshot.site = label;
        snapshot.emulated_ms = producer.emulated_ms;
        snapshot.cs = selector;
        snapshot.cs_base = base;
        snapshot.observed_eip = observed_eip;
        snapshot.start_eip = start_eip;
        const bool is_current_code_segment =
                selector == producer.cs && base == producer.cs_base;
        // The producer is the current CPU code segment; its D/B bit
        // selects 16/32-bit decoding independent of real/protected
        // mode. The independently sampled 0180:2054FE consumer is
        // known to use 32-bit instructions.
        snapshot.protected_mode = static_cast<uint8_t>(
                is_current_code_segment ? cpu.pmode : 1);
        snapshot.code_bits = static_cast<uint8_t>(
                is_current_code_segment ? (cpu.code.big ? 32 : 16) : 32);
        snapshot.irq0_vector_offset =
                mem_readw<MemOpMode::SkipBreakpoints>(0x20u);
        snapshot.irq0_vector_segment =
                mem_readw<MemOpMode::SkipBreakpoints>(0x22u);
        for (std::size_t i = 0; i < CodeBytes; ++i) {
            snapshot.bytes[i] = mem_readb<MemOpMode::SkipBreakpoints>(
                    static_cast<PhysPt>(start + i));
        }
        snapshot.captured = true;
    }

    void CaptureProducerCode(const Pending& producer)
    {
        if (!code_output.is_open() || code_snapshots[0].captured) {
            return;
        }
        CaptureSite(0, "producer_at_write",
                    producer, producer.cs, producer.cs_base,
                    producer.eip, producer.eip);
        // The preceding 32 bytes provide branch/call context. The
        // first byte may NOT be an instruction boundary; do not
        // blindly trust a disassembly starting from this offset.
        if (producer.eip >= 32u) {
            CaptureSite(1, "producer_preceding_32",
                        producer, producer.cs, producer.cs_base,
                        producer.eip, producer.eip - 32u);
        }
        // Separate protected-mode continuation after the consumer
        // clears its work-ready flag. Independent of the producer CS.
        CaptureSite(2, "consumer_after_wait_2054FE",
                    producer, 0x0180u, 0u,
                    0x002054feu, 0x002054feu);
    }

    void FlushDispatch()
    {
        if (!dispatch_output.is_open()) {
            return;
        }
        for (std::size_t i = 0; i < dispatch_count; ++i) {
            const auto& row = dispatch_buffer[i];
            dispatch_output << ++dispatch_written << ','
                            << std::fixed << std::setprecision(6)
                            << row.emulated_ms << ",dispatch,"
                            << row.cs << ',' << row.ds << ','
                            << row.ds_base << ',' << row.task_offset
                            << ',' << row.task_linear << ','
                            << row.next_offset << ','
                            << row.remaining << ','
                            << row.handler_eip << ','
                            << row.ebp << ',' << row.edi
                            << ",0\n";
        }
        if (dispatch_dropped) {
            dispatch_output << ++dispatch_written << ','
                            << std::fixed << std::setprecision(6)
                            << PIC_FullIndex() << ",overflow,"
                            << "0,0,0,0,0,0,0,0,0,0,"
                            << dispatch_dropped << '\n';
            dispatch_dropped = 0;
        }
        dispatch_count = 0;
        dispatch_output.flush();
    }

    void FlushCode()
    {
        if (!code_output.is_open()) {
            return;
        }
        for (auto& snapshot : code_snapshots) {
            if (!snapshot.captured || snapshot.written) {
                continue;
            }
            code_output << snapshot.site << ','
                        << std::fixed << std::setprecision(6)
                        << snapshot.emulated_ms << ','
                        << snapshot.cs << ','
                        << snapshot.cs_base << ','
                        << snapshot.observed_eip << ','
                        << snapshot.start_eip << ','
                        << std::hex << std::setfill('0');
            for (const auto byte : snapshot.bytes) {
                code_output << std::setw(2)
                            << static_cast<unsigned>(byte);
            }
            code_output << std::dec << std::setfill(' ')
                        << ',' << static_cast<unsigned>(snapshot.protected_mode)
                        << ',' << static_cast<unsigned>(snapshot.code_bits)
                        << ',' << snapshot.irq0_vector_segment
                        << ',' << snapshot.irq0_vector_offset << '\n';
            snapshot.written = true;
        }
        code_output.flush();
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
    uint32_t irq0_observed = 0;
    uint32_t instruction_budget = 0;
    bool instruction_previous_valid = false;
    uint32_t instruction_last_value = 0;
    uint16_t instruction_last_cs = 0;
    uint16_t instruction_last_ds = 0;
    uint32_t instruction_last_cs_base = 0;
    uint32_t instruction_last_eip = 0;
    uint32_t instruction_last_ds_base = 0;
    uint16_t ds_selector = 0;
    uint32_t data_segment_base = 0;
    uint32_t pointer_location = 0;
    uint32_t pointer_value = 0;
    uint32_t target_linear = 0;
    std::size_t buffer_size = 0;
    uint64_t event_count = 0;
    Pending pending = {};
    std::ofstream output = {};
    std::ofstream code_output = {};
    std::ofstream dispatch_output = {};
    std::array<Entry, BufferCount> buffer = {};
    std::array<CodeSnapshot, CodeSiteCount> code_snapshots = {};
    std::array<DispatchRow, DispatchBufferSize> dispatch_buffer = {};
    std::size_t dispatch_count = 0;
    uint64_t dispatch_written = 0;
    uint64_t dispatch_dropped = 0;
};

#endif // DOSBOX_MK2_SIGNAL_PROBE_H
