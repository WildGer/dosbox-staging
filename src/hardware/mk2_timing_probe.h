// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in experimental PIT/VGA telemetry. Never changes emulated timing.
#ifndef DOSBOX_MK2_TIMING_PROBE_H
#define DOSBOX_MK2_TIMING_PROBE_H

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>

// The emulator's PIT, video refresh and VGA status handlers run on the
// emulation thread. Keep the hot paths to counter increments; write only once
// per emulated second. This intentionally does NOT count gameplay ticks or
// unique framebuffer images.
class Mk2TimingProbe final {
public:
	static Mk2TimingProbe& Get()
	{
		static Mk2TimingProbe probe = {};
		return probe;
	}

	void OnPit0Event()
	{
		if (enabled) { ++pit0_events; }
	}

	void OnPit0Program(const uint32_t divisor, const double frequency_hz)
	{
		if (!enabled) { return; }
		pit0_divisor = divisor;
		pit0_programmed_hz = frequency_hz;
		++pit0_programs;
	}

	void OnVgaStatusRead(const uint8_t status)
	{
		if (!enabled) { return; }
		++status_reads;
		if (status & 0x08) { ++status_vsync_high; }
	}

	// PIC_FullIndex() returns emulated milliseconds.
	void OnVgaFrame(const double emulated_ms)
	{
		if (!enabled) { return; }
		const auto host_now = Clock::now();
		if (!started) {
			started = true;
			window_start_emulated_ms = emulated_ms;
			window_start_host = host_now;
			start_host = host_now;
		}
		++vga_frames;
		const auto elapsed_ms = emulated_ms - window_start_emulated_ms;
		if (elapsed_ms < 1000.0) { return; }
		const auto host_window_ms =
		        std::chrono::duration<double, std::milli>(host_now - window_start_host).count();
		const auto host_total_ms =
		        std::chrono::duration<double, std::milli>(host_now - start_host).count();
		const auto pit_hz = 1000.0 * static_cast<double>(pit0_events) / elapsed_ms;
		const auto vga_hz = 1000.0 * static_cast<double>(vga_frames) / elapsed_ms;

		output << std::fixed << std::setprecision(3) << emulated_ms << ','
		       << host_total_ms << ',' << elapsed_ms << ',' << host_window_ms << ','
		       << pit0_events << ',' << pit_hz << ',' << vga_frames << ','
		       << vga_hz << ',' << status_reads << ',' << status_vsync_high << ','
		       << pit0_divisor << ',' << pit0_programmed_hz << ','
		       << pit0_programs << '\n';
		output.flush();

		pit0_events = 0;
		vga_frames = 0;
		status_reads = 0;
		status_vsync_high = 0;
		pit0_programs = 0;
		window_start_emulated_ms = emulated_ms;
		window_start_host = host_now;
	}

private:
	using Clock = std::chrono::steady_clock;
	Mk2TimingProbe()
	{
		const char* setting = std::getenv("DOSBOX_MK2_TRACE");
		if (!setting || !*setting || std::strcmp(setting, "0") == 0) {
			return;
		}
		const std::string filename = (std::strcmp(setting, "1") == 0)
		                                   ? "mk2_timing.csv"
		                                   : setting;
		output.open(filename, std::ios::out | std::ios::trunc);
		if (!output.is_open()) { return; }
		output << "emulated_ms,host_elapsed_ms,window_emulated_ms,window_host_ms,"
		          "pit0_events,pit0_measured_hz,vga_refreshes,vga_measured_hz,"
		          "vga_3da_reads,vga_3da_vsync_high,pit0_divisor,"
		          "pit0_programmed_hz,pit0_programs\n";
		enabled = true;
	}

	bool enabled = false;
	bool started = false;
	std::ofstream output = {};
	Clock::time_point start_host = {};
	Clock::time_point window_start_host = {};
	double window_start_emulated_ms = 0.0;
	uint64_t pit0_events = 0;
	uint64_t vga_frames = 0;
	uint64_t status_reads = 0;
	uint64_t status_vsync_high = 0;
	uint64_t pit0_programs = 0;
	uint32_t pit0_divisor = 0;
	double pit0_programmed_hz = 0.0;
};

#endif // DOSBOX_MK2_TIMING_PROBE_H
