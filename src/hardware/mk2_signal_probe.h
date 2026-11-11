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
        bool touches_object = false;
        for (const auto& watch : object_watches) {
            if (watch.valid && Overlaps(address, size, watch.field_linear)) {
                touches_object = true;
                break;
            }
        }
        if ((!touches_target && !touches_pointer && !touches_object) ||
            !InWindow(PIC_FullIndex())) {
            return false;
        }

        const auto at_ms = PIC_FullIndex();
        const auto writer_cs = SegValue(cs);
        const auto writer_cs_base = SegPhys(cs);
        const auto writer_eip = reg_eip;
        const auto writer_ds = SegValue(ds);
        const auto writer_ds_base = SegPhys(ds);
        if (touches_target || touches_pointer) {
            pending.active = true;
            pending.pointer_write = touches_pointer;
            pending.addr = address;
            pending.size = size;
            pending.before = mem_readd<MemOpMode::SkipBreakpoints>(
                    touches_pointer ? pointer_location : target_linear);
            pending.emulated_ms = at_ms;
            pending.cs = writer_cs;
            pending.cs_base = writer_cs_base;
            pending.eip = writer_eip;
            pending.ds = writer_ds;
            pending.ds_base = writer_ds_base;
        }
        if (touches_object &&
            (object_write_output.is_open() || object_steps_output.is_open())) {
            for (std::size_t i = 0; i < object_watches.size(); ++i) {
                const auto& watch = object_watches[i];
                if (!watch.valid ||
                    !Overlaps(address, size, watch.field_linear)) {
                    continue;
                }
                auto& entry = object_pending[i];
                entry.active = true;
                entry.watch = watch;
                entry.before = mem_readw<MemOpMode::SkipBreakpoints>(
                        watch.field_linear);
                entry.emulated_ms = at_ms;
                entry.store_linear = address;
                entry.store_size = size;
                entry.cs = writer_cs;
                entry.cs_base = writer_cs_base;
                entry.eip = writer_eip;
                entry.ds = writer_ds;
                entry.ds_base = writer_ds_base;
                entry.protected_mode = cpu.pmode;
                // Disassembled x86 0x2315B1/0x2315B7 use
                // ADD DWORD PTR [EBP+0x18/+0x1C], EBX.
                // The prior object watch observes only the upper WORD
                // at +0x1A/+0x1E; it misses fractional-only updates.
                // Capture the ENTIRE DWORD for every recognized ADD,
                // even when the upper WORD remains unchanged.
                const uint32_t position_offset =
                        (watch.field_offset == 0x1au) ? 0x18u : 0x1cu;
                const uint32_t writer_for_field =
                        (position_offset == 0x18u) ? 0x002315b1u
                                                    : 0x002315b7u;
                const uint64_t position_linear =
                        static_cast<uint64_t>(watch.object_linear) +
                        position_offset;
                entry.is_position_add =
                        cpu.pmode && writer_cs == 0x0180u &&
                        writer_cs_base == 0 &&
                        writer_eip == writer_for_field &&
                        size == 4 &&
                        address == position_linear;
                if (entry.is_position_add) {
                    entry.position_before =
                            mem_readd<MemOpMode::SkipBreakpoints>(
                                    static_cast<PhysPt>(position_linear));
                    entry.expected_delta = reg_ebx;
                    entry.writer_ebp = reg_ebp;
                }
            }
        }
        return true;
    }

    // AFTER the original store: record the actual 32-bit target value
    // instead of assuming a byte/word/quad write had the expected effect.
    void AfterGuestStore()
    {
        // Observe *actual stores* to candidate object fields, rather than
        // inferring the writer from an earlier scheduler callback. Each
        // entry is made BEFORE the store and compared AFTER the store.
        for (auto& entry : object_pending) {
            if (!entry.active) {
                continue;
            }
            const auto copy = entry;
            entry.active = false;
            const auto after = mem_readw<MemOpMode::SkipBreakpoints>(
                    copy.watch.field_linear);
            if (copy.is_position_add && object_steps_output.is_open()) {
                const auto position_offset =
                        (copy.watch.field_offset == 0x1au) ? 0x18u : 0x1cu;
                const auto position_linear = static_cast<PhysPt>(
                        copy.watch.object_linear + position_offset);
                const uint32_t position_after =
                        mem_readd<MemOpMode::SkipBreakpoints>(position_linear);
                const uint32_t actual_delta =
                        position_after - copy.position_before;
                const int16_t integer_before = static_cast<int16_t>(
                        copy.position_before >> 16u);
                const int16_t integer_after = static_cast<int16_t>(
                        position_after >> 16u);
                object_steps_output << ++object_step_count << ','
                                    << std::fixed << std::setprecision(6)
                                    << copy.emulated_ms << ','
                                    << (copy.watch.slot < 2 ? 'A' : 'B')
                                    << ',' << position_offset
                                    << ',' << copy.watch.object_pointer
                                    << ',' << copy.watch.object_linear
                                    << ',' << copy.eip
                                    << ',' << copy.position_before
                                    << ',' << position_after
                                    << ',' << actual_delta
                                    << ',' << copy.expected_delta
                                    << ',' << (actual_delta ==
                                              copy.expected_delta ? 1 : 0)
                                    << ',' << integer_before
                                    << ',' << integer_after
                                    << ',' << (integer_before !=
                                              integer_after ? 1 : 0)
                                    << ',' << copy.writer_ebp
                                    << ',' << copy.expected_delta
                                    << ',' << copy.cs
                                    << ',' << copy.ds << '\n';
            }
            if (after != copy.before) {
                CaptureDominantObjectWriterCode(copy);
            }
            if (after != copy.before && object_write_output.is_open()) {
                object_write_output << ++object_write_count << ','
                                    << std::fixed << std::setprecision(6)
                                    << copy.emulated_ms << ','
                                    << (copy.watch.slot < 2 ? 'A' : 'B')
                                    << ',' << copy.watch.field_offset
                                    << ',' << copy.watch.object_pointer
                                    << ',' << copy.watch.object_linear
                                    << ',' << copy.watch.field_linear
                                    << ',' << copy.store_linear
                                    << ',' << static_cast<unsigned>(
                                               copy.store_size)
                                    << ',' << static_cast<int16_t>(copy.before)
                                    << ',' << static_cast<int16_t>(after)
                                    << ',' << copy.cs << ',' << copy.cs_base
                                    << ',' << copy.eip << ',' << copy.ds
                                    << ',' << copy.ds_base << ','
                                    << static_cast<unsigned>(
                                               copy.protected_mode) << '\n';
            }
        }
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
        // Two global object pointers are read by the callback code:
        // 0x2133B5 and 0x22047D. The latter compares WORD +0x1A
        // and the former passes both objects to 0x21374A.
        // Sample at the exact dispatcher boundary so that future
        // analysis can distinguish object-state changes from the
        // 60-Hz scheduler and 70-Hz VGA refresh.
        row.object_a = ReadObjectFields(row.ds_base, 0x002b5fc2u);
        row.object_b = ReadObjectFields(row.ds_base, 0x002b6146u);
        RefreshObjectWatches(row.object_a, 0, row.ds_base);
        RefreshObjectWatches(row.object_b, 2, row.ds_base);
        CaptureDispatchCallbackCode(row.handler_eip, row.emulated_ms);
        CaptureHelperCandidateCode(row.handler_eip, row.emulated_ms);
    }

    // Entry of a candidate shared movement routine. This is observed at
    // the real 32-bit instruction boundary (not inferred from a static
    // CALL target). Capture its timing, register arguments and stack top.
    // The stack DWORD is a RETURN-ADDRESS CANDIDATE only: a JMP into this
    // address need not leave an actual return address there.
    void OnMovementFunctionEntry()
    {
        if (!enabled || !IsNormalCore() || !cpu.pmode ||
            !cpu.code.big || SegValue(cs) != 0x0180u ||
            SegPhys(cs) != 0u || reg_eip != 0x0023156cu ||
            !InWindow(PIC_FullIndex()) || !movement_entry_output.is_open()) {
            return;
        }
        if (movement_entry_count == MovementEntryBufferSize) {
            ++movement_entry_dropped;
            return;
        }
        auto& entry = movement_entry_buffer[movement_entry_count++];
        entry.emulated_ms = PIC_FullIndex();
        entry.eax = reg_eax;
        entry.ebx = reg_ebx;
        entry.ecx = reg_ecx;
        entry.edx = reg_edx;
        entry.esi = reg_esi;
        entry.edi = reg_edi;
        entry.ebp = reg_ebp;
        entry.esp = reg_esp;
        entry.ds = SegValue(ds);
        entry.ds_base = SegPhys(ds);
        entry.ss = SegValue(ss);
        entry.ss_base = SegPhys(ss);
        const uint64_t stack_top = static_cast<uint64_t>(entry.ss_base) +
                                   static_cast<uint64_t>(entry.esp);
        entry.stack_dword = stack_top <=
                           static_cast<uint64_t>(
                                   std::numeric_limits<PhysPt>::max()) - 4u
                                   ? mem_readd<MemOpMode::SkipBreakpoints>(
                                             static_cast<PhysPt>(stack_top))
                                   : 0u;
        const auto a = ReadObjectFields(entry.ds_base, 0x002b5fc2u);
        const auto b = ReadObjectFields(entry.ds_base, 0x002b6146u);
        entry.object_a_ptr = a.valid ? a.pointer : 0u;
        entry.object_b_ptr = b.valid ? b.pointer : 0u;
    }

    void OnClockTick(const uint32_t ms)
    {
        if (enabled && ms % 1000u == 0) {
            Flush();
            FlushCode();
            FlushDispatch();
            FlushDispatchCode();
            FlushMovementEntry();
            if (object_write_output.is_open()) {
                object_write_output.flush();
            }
            FlushObjectWriterCode();
            if (object_steps_output.is_open()) {
                object_steps_output.flush();
            }
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
    static constexpr std::size_t DispatchCodeBytes = 256;
    static constexpr std::size_t DispatchCodeSites = 14;
    static constexpr std::size_t HelperCodeSites = 9;
    static constexpr std::size_t MovementEntryBufferSize = 4096;
    static constexpr std::size_t ObjectWatchSites = 4;
    static constexpr std::size_t ObjectWriterCodeSites = 4;
    static constexpr std::size_t ObjectWriterCodeBytes = 256;
    static constexpr uint32_t ObjectWriterContextBytes = 48;

    struct MovementEntryRow {
        double emulated_ms = 0;
        uint32_t eax = 0;
        uint32_t ebx = 0;
        uint32_t ecx = 0;
        uint32_t edx = 0;
        uint32_t esi = 0;
        uint32_t edi = 0;
        uint32_t ebp = 0;
        uint32_t esp = 0;
        uint32_t ds_base = 0;
        uint32_t ss_base = 0;
        uint32_t stack_dword = 0;
        uint32_t object_a_ptr = 0;
        uint32_t object_b_ptr = 0;
        uint16_t ds = 0;
        uint16_t ss = 0;
    };

    struct ObjectWriterSnapshot {
        bool captured = false;
        bool written = false;
        double emulated_ms = 0;
        uint16_t cs = 0;
        uint32_t cs_base = 0;
        uint32_t observed_eip = 0;
        uint32_t start_eip = 0;
        std::array<uint8_t, ObjectWriterCodeBytes> bytes = {};
    };

    struct ObjectWatch {
        bool valid = false;
        uint8_t slot = 0;
        uint16_t field_offset = 0;
        uint32_t object_pointer = 0;
        uint32_t object_linear = 0;
        PhysPt field_linear = 0;
    };

    struct ObjectPending {
        bool active = false;
        ObjectWatch watch = {};
        uint16_t before = 0;
        double emulated_ms = 0;
        PhysPt store_linear = 0;
        uint8_t store_size = 0;
        uint16_t cs = 0;
        uint32_t cs_base = 0;
        uint32_t eip = 0;
        uint16_t ds = 0;
        uint32_t ds_base = 0;
        bool protected_mode = false;
        bool is_position_add = false;
        uint32_t position_before = 0;
        uint32_t expected_delta = 0;
        uint32_t writer_ebp = 0;
    };

    struct DispatchCodeSnapshot {
        bool captured = false;
        bool written = false;
        uint32_t eip = 0;
        double emulated_ms = 0;
        std::array<uint8_t, DispatchCodeBytes> bytes = {};
    };

    struct ObjectFields {
        uint32_t pointer = 0;
        int16_t field_1a = 0;
        int16_t field_1e = 0;
        bool valid = false;
    };

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
        ObjectFields object_a = {};
        ObjectFields object_b = {};
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
                               "ebp,edi,dropped_count,"
                               "object_a_valid,object_a_pointer,"
                               "object_a_field_1a,object_a_field_1e,"
                               "object_b_valid,object_b_pointer,"
                               "object_b_field_1a,object_b_field_1e\n";
        }

        // Only a small, selected set of callback ENTRY instructions is
        // captured from the user's running MK2 memory. This output is
        // private and contains no game bytes in the fork source.
        const auto dispatch_code_path = trace.substr(0, cut) +
                                        "_dispatch_code" +
                                        (cut < trace.size()
                                                 ? trace.substr(cut)
                                                 : ".csv");
        dispatch_code_output.open(dispatch_code_path,
                                  std::ios::out | std::ios::trunc);
        if (dispatch_code_output.is_open()) {
            dispatch_code_output << "site,emulated_ms,cs,cs_base,"
                                    "observed_eip,snapshot_start,"
                                    "bytes_hex,code_bits\n";
        }

        // Store-level attribution of the two observed object fields:
        // DS:[DS:[global-pointer]+0x1A/0x1E] for each object.
        // This is separate from the pre-dispatch change snapshots.
        const auto object_write_path = trace.substr(0, cut) +
                                       "_object_writes" +
                                       (cut < trace.size()
                                                ? trace.substr(cut)
                                                : ".csv");
        object_write_output.open(object_write_path,
                                 std::ios::out | std::ios::trunc);
        if (object_write_output.is_open()) {
            object_write_output << "event_index,emulated_ms,object,"
                                   "field_offset,object_pointer,"
                                   "object_linear,field_linear,"
                                   "store_linear,store_size,before,after,"
                                   "writer_cs,writer_cs_base,writer_eip,"
                                   "writer_ds,writer_ds_base,"
                                   "protected_mode\n";
        }

        const auto object_writer_code_path =
                trace.substr(0, cut) + "_object_writer_code" +
                (cut < trace.size() ? trace.substr(cut) : ".csv");
        object_writer_code_output.open(object_writer_code_path,
                                       std::ios::out | std::ios::trunc);
        if (object_writer_code_output.is_open()) {
            object_writer_code_output <<
                    "site,emulated_ms,cs,cs_base,observed_eip,"
                    "snapshot_start,bytes_hex,code_bits\n";
        }

        const auto object_steps_path = trace.substr(0, cut) +
                                       "_object_steps" +
                                       (cut < trace.size()
                                                ? trace.substr(cut)
                                                : ".csv");
        object_steps_output.open(object_steps_path,
                                 std::ios::out | std::ios::trunc);
        if (object_steps_output.is_open()) {
            object_steps_output <<
                    "event_index,emulated_ms,object,position_offset,"
                    "object_pointer,object_linear,writer_eip,"
                    "old_raw32,new_raw32,actual_delta32,expected_delta32,"
                    "delta_matches,old_integer,new_integer,"
                    "integer_changed,writer_ebp,writer_ebx,"
                    "writer_cs,writer_ds\n";
        }

        const auto movement_path = trace.substr(0, cut) +
                                   "_movement_entry" +
                                   (cut < trace.size()
                                            ? trace.substr(cut)
                                            : ".csv");
        movement_entry_output.open(movement_path,
                                   std::ios::out | std::ios::trunc);
        if (movement_entry_output.is_open()) {
            movement_entry_output <<
                "event_index,emulated_ms,entry_eip,entry_eax,"
                "entry_ebx,entry_ecx,entry_edx,entry_esi,"
                "entry_edi,entry_ebp,entry_esp,ds,ds_base,"
                "ss,ss_base,stack_dword,object_a_pointer,"
                "object_b_pointer,dropped_count\n";
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
        FlushDispatchCode();
        FlushMovementEntry();
        if (object_write_output.is_open()) {
            object_write_output.flush();
        }
        FlushObjectWriterCode();
        if (object_steps_output.is_open()) {
            object_steps_output.flush();
        }
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

    // Prioritize the stable 60-Hz work tasks, their phase-dependent
    // replacements, and several independently timed callbacks from
    // the actual 17,403-row MK2 dispatch capture. No speculative
    // guest code or asset bytes are embedded in this source.
    void CaptureDispatchCallbackCode(const uint32_t handler_eip,
                                     const double emulated_ms)
    {
        if (!dispatch_code_output.is_open() ||
            handler_eip > std::numeric_limits<PhysPt>::max() -
                                  DispatchCodeBytes) {
            return;
        }

        static constexpr std::array<uint32_t, DispatchCodeSites> targets = {
                0x002133b5u, 0x0022047du, 0x00223dfdu, 0x002239b2u,
                0x00202d95u, 0x00213213u, 0x0021ff5du, 0x0022fb73u,
                0x00201d8au, 0x0022fabfu, 0x00230101u, 0x0021455au,
                0x00221ee0u, 0x00224beau};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (targets[i] != handler_eip || dispatch_code[i].captured) {
                continue;
            }
            auto& sample = dispatch_code[i];
            sample.eip = handler_eip;
            sample.emulated_ms = emulated_ms;
            // Here CS=0180 and CS.base=0, confirmed by the caller's
            // guard. All selected callbacks use 32-bit code.
            for (std::size_t j = 0; j < DispatchCodeBytes; ++j) {
                sample.bytes[j] =
                        mem_readb<MemOpMode::SkipBreakpoints>(
                                static_cast<PhysPt>(handler_eip + j));
            }
            sample.captured = true;
            return;
        }
    }

    // Helper addresses are referenced by actual decoded MK2 callbacks.
    // Capturing their code does NOT prove they execute in the sampled
    // dispatcher pass; they are further disassembly candidates.
    void CaptureHelperCandidateCode(const uint32_t handler_eip,
                                    const double emulated_ms)
    {
        if (!dispatch_code_output.is_open() ||
            (handler_eip != 0x002133b5u &&
             handler_eip != 0x0022047du &&
             handler_eip != 0x002239b2u)) {
            return;
        }
        static constexpr std::array<uint32_t, HelperCodeSites> targets = {
                0x0021374au, 0x002136f6u, 0x00202706u, 0x00201739u,
                0x00203285u, 0x00231ec8u, 0x002054bbu, 0x00205673u,
                0x0023156cu};
        for (std::size_t i = 0; i < targets.size(); ++i) {
            auto& sample = helper_code[i];
            if (sample.captured) {
                continue;
            }
            const uint32_t eip = targets[i];
            sample.eip = eip;
            sample.emulated_ms = emulated_ms;
            for (std::size_t j = 0; j < DispatchCodeBytes; ++j) {
                sample.bytes[j] =
                        mem_readb<MemOpMode::SkipBreakpoints>(
                                static_cast<PhysPt>(eip + j));
            }
            sample.captured = true;
        }
    }

    void FlushDispatchCode()
    {
        if (!dispatch_code_output.is_open()) {
            return;
        }
        for (auto& sample : dispatch_code) {
            if (!sample.captured || sample.written) {
                continue;
            }
            dispatch_code_output << "callback_" << std::hex
                                 << std::uppercase << std::setfill('0')
                                 << std::setw(8) << sample.eip
                                 << std::dec << std::nouppercase
                                 << std::setfill(' ') << ','
                                 << std::fixed << std::setprecision(6)
                                 << sample.emulated_ms
                                 << ",384,0," << sample.eip << ','
                                 << sample.eip << ','
                                 << std::hex << std::setfill('0');
            for (const auto byte : sample.bytes) {
                dispatch_code_output << std::setw(2)
                                     << static_cast<unsigned>(byte);
            }
            dispatch_code_output << std::dec << std::setfill(' ')
                                 << ",32\n";
            sample.written = true;
        }
        for (auto& sample : helper_code) {
            if (!sample.captured || sample.written) {
                continue;
            }
            dispatch_code_output << "helper_candidate_" << std::hex
                                 << std::uppercase << std::setfill('0')
                                 << std::setw(8) << sample.eip
                                 << std::dec << std::nouppercase
                                 << std::setfill(' ') << ','
                                 << std::fixed << std::setprecision(6)
                                 << sample.emulated_ms
                                 << ",384,0," << sample.eip << ','
                                 << sample.eip << ','
                                 << std::hex << std::setfill('0');
            for (const auto byte : sample.bytes) {
                dispatch_code_output << std::setw(2)
                                     << static_cast<unsigned>(byte);
            }
            dispatch_code_output << std::dec << std::setfill(' ')
                                 << ",32\n";
            sample.written = true;
        }
        dispatch_code_output.flush();
    }

    void RefreshObjectWatches(const ObjectFields& object,
                              const std::size_t first_slot,
                              const uint32_t ds_base)
    {
        for (std::size_t j = 0; j < 2; ++j) {
            auto& watch = object_watches[first_slot + j];
            watch.valid = false;
            if (!object.valid) {
                continue;
            }
            const uint16_t offset = (j == 0) ? 0x1au : 0x1eu;
            const uint64_t linear =
                    static_cast<uint64_t>(ds_base) +
                    static_cast<uint64_t>(object.pointer);
            if (linear > std::numeric_limits<PhysPt>::max() -
                                 static_cast<uint64_t>(offset) - 2u) {
                continue;
            }
            watch.slot = static_cast<uint8_t>(first_slot + j);
            watch.field_offset = offset;
            watch.object_pointer = object.pointer;
            watch.object_linear = static_cast<uint32_t>(linear);
            watch.field_linear = static_cast<PhysPt>(linear + offset);
            watch.valid = true;
        }
    }

    static ObjectFields ReadObjectFields(const uint32_t ds_base,
                                         const uint32_t global_offset)
    {
        ObjectFields result = {};
        const uint64_t pointer_location =
                static_cast<uint64_t>(ds_base) + global_offset;
        constexpr uint64_t max_address =
                std::numeric_limits<PhysPt>::max();
        if (pointer_location > max_address - 4u) {
            return result;
        }
        const uint32_t pointer =
                mem_readd<MemOpMode::SkipBreakpoints>(
                        static_cast<PhysPt>(pointer_location));
        if (!pointer) {
            return result;
        }
        const uint64_t object_linear =
                static_cast<uint64_t>(ds_base) + pointer;
        if (object_linear > max_address - 0x20u) {
            return result;
        }

        result.pointer = pointer;
        result.field_1a = static_cast<int16_t>(
                mem_readw<MemOpMode::SkipBreakpoints>(
                        static_cast<PhysPt>(object_linear + 0x1au)));
        result.field_1e = static_cast<int16_t>(
                mem_readw<MemOpMode::SkipBreakpoints>(
                        static_cast<PhysPt>(object_linear + 0x1eu)));
        result.valid = true;
        return result;
    }

    // Two hot instruction sites account for 4,889 of the 4,958
    // changed field stores in the 21:xx research run. Capture only
    // one tiny, read-only code slice around each of those sites.
    // The 48-byte preceding context MAY start mid-instruction;
    // re-align from known EIP when interpreting the disassembly.
    void CaptureDominantObjectWriterCode(const ObjectPending& change)
    {
        if (!object_writer_code_output.is_open() ||
            !change.protected_mode || change.cs != 0x0180u ||
            change.cs_base != 0) {
            return;
        }
        static constexpr std::array<uint32_t, ObjectWriterCodeSites> sites = {
                0x002315b1u, 0x002315b1u,
                0x002315b7u, 0x002315b7u};
        for (std::size_t i = 0; i < sites.size(); ++i) {
            auto& snapshot = object_writer_code[i];
            if (snapshot.captured || change.eip != sites[i]) {
                continue;
            }
            // Capture both an ALIGNED instruction-start excerpt
            // and a separate preceding-context excerpt, because the
            // latter can begin in the middle of an x86 instruction.
            const bool preceding = (i % 2u) != 0u;
            if (preceding && change.eip < ObjectWriterContextBytes) {
                continue;
            }
            const uint32_t start_eip = preceding
                    ? change.eip - ObjectWriterContextBytes
                    : change.eip;
            const uint64_t last_exclusive =
                    static_cast<uint64_t>(change.cs_base) +
                    start_eip + ObjectWriterCodeBytes;
            if (last_exclusive >
                static_cast<uint64_t>(std::numeric_limits<PhysPt>::max())) {
                continue;
            }
            snapshot.cs = change.cs;
            snapshot.cs_base = change.cs_base;
            snapshot.observed_eip = change.eip;
            snapshot.start_eip = start_eip;
            snapshot.emulated_ms = change.emulated_ms;
            for (std::size_t b = 0; b < ObjectWriterCodeBytes; ++b) {
                const auto linear = static_cast<PhysPt>(
                        change.cs_base + snapshot.start_eip + b);
                snapshot.bytes[b] =
                        mem_readb<MemOpMode::SkipBreakpoints>(linear);
            }
            snapshot.captured = true;
        }
    }

    void FlushObjectWriterCode()
    {
        if (!object_writer_code_output.is_open()) {
            return;
        }
        for (auto& snapshot : object_writer_code) {
            if (!snapshot.captured || snapshot.written) {
                continue;
            }
            const bool preceding =
                    snapshot.start_eip != snapshot.observed_eip;
            object_writer_code_output
                    << (preceding ? "writer_preceding_" : "writer_at_")
                    << std::hex << std::uppercase << std::setfill('0')
                                      << std::setw(8) << snapshot.observed_eip
                                      << std::dec << std::nouppercase
                                      << std::setfill(' ') << ','
                                      << std::fixed << std::setprecision(6)
                                      << snapshot.emulated_ms << ','
                                      << snapshot.cs << ','
                                      << snapshot.cs_base << ','
                                      << snapshot.observed_eip << ','
                                      << snapshot.start_eip << ','
                                      << std::hex << std::setfill('0');
            for (const auto byte : snapshot.bytes) {
                object_writer_code_output << std::setw(2)
                                          << static_cast<unsigned>(byte);
            }
            object_writer_code_output << std::dec << std::setfill(' ')
                                      << ",32\n";
            snapshot.written = true;
        }
        object_writer_code_output.flush();
    }

    void FlushMovementEntry()
    {
        if (!movement_entry_output.is_open()) {
            return;
        }
        for (std::size_t i = 0; i < movement_entry_count; ++i) {
            const auto& e = movement_entry_buffer[i];
            movement_entry_output << ++movement_entry_written << ','
                                  << std::fixed << std::setprecision(6)
                                  << e.emulated_ms
                                  << ',' << 0x0023156cu << ','
                                  << e.eax << ',' << e.ebx << ','
                                  << e.ecx << ',' << e.edx << ','
                                  << e.esi << ',' << e.edi << ','
                                  << e.ebp << ',' << e.esp << ','
                                  << e.ds << ',' << e.ds_base << ','
                                  << e.ss << ',' << e.ss_base << ','
                                  << e.stack_dword << ','
                                  << e.object_a_ptr << ','
                                  << e.object_b_ptr << ",0\n";
        }
        if (movement_entry_dropped) {
            movement_entry_output << ++movement_entry_written << ','
                                  << std::fixed << std::setprecision(6)
                                  << PIC_FullIndex()
                                  << ",0,0,0,0,0,0,0,0,0,0,0,0,0,"
                                  << "0,0,0,0," << movement_entry_dropped
                                  << '\n';
            movement_entry_dropped = 0;
        }
        movement_entry_count = 0;
        movement_entry_output.flush();
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
                            << ",0,"
                            << static_cast<unsigned>(row.object_a.valid)
                            << ',' << row.object_a.pointer
                            << ',' << row.object_a.field_1a
                            << ',' << row.object_a.field_1e
                            << ',' << static_cast<unsigned>(row.object_b.valid)
                            << ',' << row.object_b.pointer
                            << ',' << row.object_b.field_1a
                            << ',' << row.object_b.field_1e << '\n';
        }
        if (dispatch_dropped) {
            dispatch_output << ++dispatch_written << ','
                            << std::fixed << std::setprecision(6)
                            << PIC_FullIndex() << ",overflow,"
                            << "0,0,0,0,0,0,0,0,0,0,"
                            << dispatch_dropped
                            << ",0,0,0,0,0,0,0,0\n";
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
    std::ofstream dispatch_code_output = {};
    std::ofstream object_write_output = {};
    std::ofstream object_writer_code_output = {};
    std::ofstream object_steps_output = {};
    std::ofstream movement_entry_output = {};
    std::array<Entry, BufferCount> buffer = {};
    std::array<CodeSnapshot, CodeSiteCount> code_snapshots = {};
    std::array<DispatchRow, DispatchBufferSize> dispatch_buffer = {};
    std::array<DispatchCodeSnapshot, DispatchCodeSites> dispatch_code = {};
    std::array<DispatchCodeSnapshot, HelperCodeSites> helper_code = {};
    std::array<ObjectWatch, ObjectWatchSites> object_watches = {};
    std::array<ObjectPending, ObjectWatchSites> object_pending = {};
    std::array<ObjectWriterSnapshot, ObjectWriterCodeSites> object_writer_code = {};
    std::array<MovementEntryRow, MovementEntryBufferSize> movement_entry_buffer = {};
    uint64_t object_write_count = 0;
    uint64_t object_step_count = 0;
    uint64_t movement_entry_written = 0;
    uint64_t movement_entry_dropped = 0;
    std::size_t movement_entry_count = 0;
    std::size_t dispatch_count = 0;
    uint64_t dispatch_written = 0;
    uint64_t dispatch_dropped = 0;
};

#endif // DOSBOX_MK2_SIGNAL_PROBE_H
