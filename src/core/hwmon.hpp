#pragma once

#include <string>
#include <vector>

// What the hardware is doing while the game runs, for chasing a slowdown that comes with time and stays: whether the CPU
// and the graphics card have been slowed down to stay cool or within their power (a laptop does it for the two of them
// together), and whether the card's memory is filling.
//
//   the CPU        its clock now, boost included (Windows' performance counters), against its rated one and the limit Windows
//                  has put on it (the power plan, the battery, heat); how busy it is, and this game's share of it
//   the memory     this game's, and the PC's in use against all of it
//   the GPU        NVIDIA's NVML (nvml.dll, which the driver puts in System32): clock, temperature, power and the limit,
//                  how busy it is, its memory, and the reasons it gives for running slower (a power cap, a thermal slowdown)
//
// /vana stats shows them under its bars, and /vana telemetry writes a line every 5 seconds to
// logs\telemetry-<date>-<time>.csv with the frame rate beside them, to leave on through a long session and read after: when
// the frame rate fell, was the card hot, was the memory full, had the clocks dropped?
//
//   /vana telemetry            starts it, or stops it
//   /vana telemetry on|off
namespace vanacore::hwmon {

// From VanaCoreInstall: VanaCore's folder (the CSV goes in its logs folder).
void install(const wchar_t* install_dir);

// Something wants the readings (the stats window, the telemetry): they're polled once a second while any does.
void acquire();
void release();

// Something wants the readings for the next few seconds (an addon reading vana.graphics.hardware() while a page shows
// them): holds them as acquire() does, and lets go on its own 5 s after the last touch (on_frame checks).
void touch();

struct Reading {
    bool power = false;         // Windows' power state is known
    bool ac = false;            // plugged in
    int battery = -1;           // % charged, -1 unknown or no battery
    std::string power_plan;     // Windows' power plan (Control Panel > Power Options), by the name it shows
    std::string power_mode;     // Windows' power mode on top of it: best performance... ("" balanced, or not in use)
    bool cpu = false;
    bool cpu_live = false;      // cpu_mhz and cpu_peak_mhz are the clocks the cores run at (Windows 8 on), not their rated ones
    unsigned cpu_mhz = 0;       // the cores' clock now while they run, boost included, the busy ones counting the most
    unsigned cpu_peak_mhz = 0;  // the fastest busy core's
    unsigned cpu_max_mhz = 0;   // the rated (base) clock of the fastest kind of core: boost takes it past this
    unsigned cpu_limit_mhz = 0; // what Windows lets that kind run at, at the most-held core's share of its own rated clock:
                                // below cpu_max_mhz, held there (the power plan, or the PC itself: heat, its firmware)
    unsigned cpu_plan_state = 100; // the power plan's Maximum processor state, %
    unsigned cpu_plan_mhz = 0;  // the power plan's Maximum processor frequency, 0 none
    unsigned cpu_plan_boost = 1; // the power plan's Processor performance boost mode, 0 off (no boost past the rated clock)
    bool cpu_load = false;      // cpu_busy and cpu_game are there (from the second poll on: they're over the second between)
    double cpu_busy = 0;        // % of every core's time something ran (Task Manager's Performance > CPU)
    double cpu_game = 0;        // % of every core's time this game ran (Task Manager's CPU column: 100 is all the cores)
    double ram_game_mb = 0;     // this game's private memory (Task Manager's Commit size; /vana graph's Memory)
    double ram_used_mb = 0, ram_total_mb = 0; // the PC's memory in use (every program's) and in all, 0 unknown
    bool gpu = false;
    unsigned gpu_mhz = 0, gpu_max_mhz = 0;
    int gpu_temp = 0;           // C
    unsigned gpu_watts = 0, gpu_limit_watts = 0;
    unsigned gpu_busy = 0;      // % of the time something ran on it
    double vram_used_mb = 0, vram_total_mb = 0; // all programs'
    double vram_game_mb = 0, vram_budget_mb = 0; // this game's own, and Windows' budget for it (DXGI: any card; 0 unknown)
    unsigned long long throttle = 0; // NVML's reasons the clocks are held down (bits), 0 none
    bool throttled = false;     // a reason that's a limit (power, thermal), not idling
    std::string throttle_text;  // them in words
    std::string gpu_why;        // why there's no GPU reading, when there isn't
};
Reading latest();

// Each frame, from the Present hook, on the render thread: counts it, for the telemetry's frame rate.
void on_frame();

// "telemetry ..." (its words after /vana): true, it's handled.
bool command(const std::vector<std::string>& parts);

} // namespace vanacore::hwmon
