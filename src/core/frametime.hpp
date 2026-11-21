#pragma once

// [logging] frametimes=1: a line in the log every 10 seconds on where the game's frames go. Frame times
// (fastest, average, slowest), and in the average frame: the game's own work, its Sleep calls on the
// render thread (by the module calling, what it asked for and what it got), VanaCore's work,
// VanaCore's frame rate limit, and the device's Present; plus Windows' timer resolution, which
// decides how long a Sleep really lasts.
namespace vanacore::frametime {

// From VanaCoreInstall: reads the setting; when it's on, watches Sleep in every module that loads.
void install(const wchar_t* ini);

// The game's Direct3D 8 device is made: its methods (and vertex and index buffers' and textures'
// locks) are timed, so the log can say how much of the game's frame is spent in Direct3D.
void on_device(void* device);

// From the Present hook, on the render thread: its start, the frame rate limit's wait around it,
// and the device's Present around it.
void frame_start();
void limit_start();
void limit_end();
void present_start();
void present_end();

// A finished frame's parts, in ms (for /vana debug's recording): the frame, the game's Direct3D calls (outermost),
// its sleeps on the render thread, VanaCore's own work in Present, its frame rate limit, the device's Present; and
// the slowest single Direct3D call in it, by name.
struct Parts {
    double frame = 0, direct3d = 0, sleeps = 0, vanacore = 0, limit = 0, present = 0;
    double slowest = 0;
    const char* slowest_name = "";
};

// While a recording runs: the timing on (as [logging] frametimes has it, without its log lines unless that's on), each
// finished frame's parts to `sink`; nullptr: back as it was. On the render thread.
void record(void (*sink)(const Parts& parts));

// More watchers of the same, for /vana stats and /vana trace's timed trace: as record(), and they all may run at once
// (up to four). unwatch(): that one stops.
void watch(void (*sink)(const Parts& parts));
void unwatch(void (*sink)(const Parts& parts));

// The timed Direct3D methods' time and calls since the last call, which starts the next (only those called, up to
// `max`; their time includes any call inside one): for /vana stats, which sorts them into what they're about.
struct Method {
    const char* name;
    double ms;
    unsigned calls;
};
int take_methods(Method* out, int max);

// Direct3D 12's frame profile (GPU time, draws, uploads; nothing on Direct3D 11): on for each that asks, and back as it
// was when the last one stops. A recording and /vana stats both ask.
void profile(bool on);

} // namespace vanacore::frametime
