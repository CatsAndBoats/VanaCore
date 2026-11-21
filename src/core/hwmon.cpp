#define PSAPI_VERSION 2
#include "hwmon.hpp"

#include <windows.h>

#include <pdh.h>
#include <pdhmsg.h>
#include <powrprof.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "../dlss/hw_ipc.h"
#include "../gfx/vanacore_gfx.h"
#include "chat.hpp"
#include "log.hpp"

#pragma comment(lib, "powrprof.lib") // CallNtPowerInformation

namespace vanacore::hwmon {
namespace {

// What CallNtPowerInformation(ProcessorInformation) gives for each logical processor. Its clocks are the rated ones:
// CurrentMhz stays MaxMhz (the base clock) on a CPU that boosts past it, and a CPU with two kinds of core (Intel's P-cores
// and E-cores) has a rated clock for each
struct ProcessorPower {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

// Windows' performance counters (pdh.dll, loaded by the poll thread rather than imported, so the game's start doesn't load
// it): each core's clock while it runs as a % of its rated one (over 100 while it boosts), and how busy it is. Windows 8
// on: Windows 7 hasn't the first, and the clocks are the rated ones there.
struct Counters {
    decltype(&PdhOpenQueryW) open = nullptr;
    decltype(&PdhAddEnglishCounterW) add = nullptr;
    decltype(&PdhCollectQueryData) collect = nullptr;
    decltype(&PdhGetFormattedCounterArrayW) array = nullptr;
    decltype(&PdhCloseQuery) close = nullptr;
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER perf = nullptr, busy = nullptr;
    bool tried = false;
};
Counters g_counters; // the poll thread's

std::wstring g_install;
std::wstring g_logs;
std::mutex g_lock;
Reading g_reading;
std::atomic<int> g_users{0};
HANDLE g_thread = nullptr;
std::atomic<bool> g_run{false};
HANDLE g_helper = nullptr; // VanaCore.Dlss.exe --hw
HANDLE g_map = nullptr;
VanaHwShared* g_shared = nullptr;
std::string g_helper_why;
std::atomic<ULONGLONG> g_touch_until{0}; // GetTickCount64() when touch()'s hold lapses
std::atomic<bool> g_touch_held{false};   // touch() holds a user of its own (acquire once, release when it lapses)
constexpr ULONGLONG kTouchHold = 5000;   // ms a touch holds the readings
// this game's video memory and Windows' budget for it (DXGI, any card), read on the render thread in on_frame once a
// second while someone wants the readings (the device is made and lost on that thread), for latest()
std::atomic<unsigned long long> g_vram_game{0}, g_vram_budget{0};
ULONGLONG g_vram_tick = 0;

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// NVML's reasons the clocks are held down, in words; `limit` set for a reason that's a limit (not the card idling)
std::string throttle_words(unsigned long long bits, bool& limit)
{
    struct Reason {
        unsigned long long bit;
        const char* text;
        bool limit;
    };
    static const Reason kReasons[] = {
        {0x04, "power cap (software)", true},
        {0x08, "hardware slowdown", true},
        {0x20, "thermal slowdown (software)", true},
        {0x40, "thermal slowdown (hardware)", true},
        {0x80, "power brake (hardware)", true},
        {0x10, "sync boost", false},
        {0x02, "application clocks setting", false},
        {0x100, "display clock setting", false},
        {0x01, "idle", false},
    };
    std::string out;
    limit = false;
    unsigned long long known = 0;
    for (const Reason& r : kReasons) {
        known |= r.bit;
        if ((bits & r.bit) == 0)
            continue;
        out += (out.empty() ? "" : ", ") + std::string(r.text);
        limit = limit || r.limit;
    }
    if ((bits & ~known) != 0) { // a reason newer than this list: its bits, so it can be looked up
        char other[40];
        std::snprintf(other, sizeof other, "other 0x%llX", bits & ~known);
        out += (out.empty() ? "" : ", ") + std::string(other);
    }
    return out.empty() ? "none" : out;
}

// Plugged in or not, the power plan (Control Panel > Power Options) and the power mode Windows is in (Settings > System >
// Power): a laptop on the battery or in its efficiency mode holds the CPU and the graphics card back
void poll_power(Reading& r)
{
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        r.power = true;
        r.ac = status.ACLineStatus == 1;
        r.battery = status.BatteryLifePercent <= 100 ? status.BatteryLifePercent : -1;
    }
    // the plan by the name Windows shows for it, in its language: a plan made from another (Ultimate Performance is one,
    // made when it's turned on) has a GUID of its own
    GUID* plan = nullptr;
    if (PowerGetActiveScheme(nullptr, &plan) == ERROR_SUCCESS && plan != nullptr) {
        wchar_t name[256] = L"";
        DWORD size = sizeof name - sizeof(wchar_t);
        if (PowerReadFriendlyName(nullptr, plan, nullptr, nullptr, reinterpret_cast<UCHAR*>(name), &size) == ERROR_SUCCESS)
            r.power_plan = narrow(name);
        // the plan's own caps on the CPU, for the power source in use: Maximum processor state (%) and Maximum processor
        // frequency (MHz, 0 none). Without one, a clock held below its fastest is the PC's doing (heat, its firmware)
        static const GUID kProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
        static const GUID kStateMax = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
        static const GUID kFreqMax = {0x75b0ae3f, 0xbce0, 0x45a7, {0x8c, 0x89, 0xc9, 0x61, 0x1c, 0x25, 0xe1, 0x00}};
        const auto read = r.power && !r.ac ? PowerReadDCValueIndex : PowerReadACValueIndex;
        DWORD value = 0;
        if (read(nullptr, plan, &kProcessor, &kStateMax, &value) == ERROR_SUCCESS)
            r.cpu_plan_state = value;
        if (read(nullptr, plan, &kProcessor, &kFreqMax, &value) == ERROR_SUCCESS)
            r.cpu_plan_mhz = value;
        // Processor performance boost mode (hidden in Power Options): 0 is off, the cores kept to their rated clock
        static const GUID kBoost = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
        if (read(nullptr, plan, &kProcessor, &kBoost, &value) == ERROR_SUCCESS)
            r.cpu_plan_boost = value;
        LocalFree(plan);
    }
    using GetOverlay = DWORD(WINAPI*)(GUID*);
    static const auto get = reinterpret_cast<GetOverlay>(GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerGetEffectiveOverlayScheme"));
    GUID mode{};
    if (get == nullptr || get(&mode) != 0)
        return;
    // The mode is all zeros both for Balanced and under a plan other than Balanced (Windows hides the mode then): only
    // the others are told, beside the plan
    static const GUID kEfficiency = {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
    static const GUID kBetter = {0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}};
    static const GUID kBest = {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};
    static const GUID kNone = {};
    r.power_mode = IsEqualGUID(mode, kBest)        ? "best performance"
                   : IsEqualGUID(mode, kBetter)    ? "better performance"
                   : IsEqualGUID(mode, kEfficiency) ? "best power efficiency"
                   : IsEqualGUID(mode, kNone)      ? ""
                                                   : "other";
}

void open_counters(Counters& c)
{
    c.tried = true;
    static const HMODULE pdh = LoadLibraryW(L"pdh.dll");
    if (pdh == nullptr)
        return;
    c.open = reinterpret_cast<decltype(c.open)>(GetProcAddress(pdh, "PdhOpenQueryW"));
    c.add = reinterpret_cast<decltype(c.add)>(GetProcAddress(pdh, "PdhAddEnglishCounterW"));
    c.collect = reinterpret_cast<decltype(c.collect)>(GetProcAddress(pdh, "PdhCollectQueryData"));
    c.array = reinterpret_cast<decltype(c.array)>(GetProcAddress(pdh, "PdhGetFormattedCounterArrayW"));
    c.close = reinterpret_cast<decltype(c.close)>(GetProcAddress(pdh, "PdhCloseQuery"));
    if (c.open == nullptr || c.add == nullptr || c.collect == nullptr || c.array == nullptr || c.close == nullptr ||
        c.open(nullptr, 0, &c.query) != ERROR_SUCCESS) {
        c.query = nullptr;
        return;
    }
    if (c.add(c.query, L"\\Processor Information(*)\\% Processor Performance", 0, &c.perf) != ERROR_SUCCESS ||
        c.add(c.query, L"\\Processor Information(*)\\% Processor Time", 0, &c.busy) != ERROR_SUCCESS) {
        c.close(c.query);
        c.query = nullptr;
        return;
    }
    c.collect(c.query); // the first of the two samples a rate needs: the next poll has the values
}

void close_counters(Counters& c)
{
    if (c.query != nullptr)
        c.close(c.query);
    c.query = nullptr;
    c.tried = false;
}

// A counter's value for each logical processor, by its number (instance "0,5"); the totals, and groups past the first
// (over 64 processors), left out
std::vector<double> per_processor(const Counters& c, PDH_HCOUNTER counter)
{
    std::vector<double> out;
    DWORD size = 0, count = 0;
    if (c.array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA)
        return out;
    std::vector<BYTE> buffer(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (c.array(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0; i < count; ++i) {
        unsigned group = 0, number = 0;
        if (swscanf_s(items[i].szName, L"%u,%u", &group, &number) != 2 || group != 0 || number >= 64 ||
            (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA))
            continue;
        if (out.size() <= number)
            out.resize(number + 1, 0.0);
        out[number] = items[i].FmtValue.doubleValue;
    }
    return out;
}

void poll_cpu(Reading& r)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    std::vector<ProcessorPower> power(info.dwNumberOfProcessors);
    if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(), static_cast<ULONG>(power.size() * sizeof(ProcessorPower))) != 0)
        return;
    Counters& c = g_counters;
    std::vector<double> perf, busy;
    if (!c.tried)
        open_counters(c); // its first sample: the clocks come from the next poll's, a second on
    else if (c.query != nullptr && c.collect(c.query) == ERROR_SUCCESS)
        perf = per_processor(c, c.perf), busy = per_processor(c, c.busy);
    // Each core's clock: its rated one times how far past it the core runs while it runs, else (no counters) the rated one.
    // The CPU's is the cores' with the busy ones counting the most. Held: a core Windows limits below its own rated clock
    // (each kind of core against its own), as a share of it.
    double weighed = 0, weight = 0, all = 0, peak = 0, peak_busy = 0, held = 1.0;
    unsigned max = 0, cores = 0;
    bool live = false;
    for (const ProcessorPower& p : power) {
        if (p.MaxMhz == 0)
            continue;
        const bool counted = p.Number < perf.size() && perf[p.Number] > 0;
        const double mhz = counted ? p.MaxMhz * perf[p.Number] / 100.0 : p.CurrentMhz;
        const double load = p.Number < busy.size() ? busy[p.Number] : 0.0;
        live = live || counted;
        weighed += mhz * load, weight += load, all += mhz, ++cores;
        peak = std::max(peak, mhz);
        if (load >= 5.0)
            peak_busy = std::max(peak_busy, mhz);
        max = std::max<unsigned>(max, p.MaxMhz);
        if (p.MhzLimit != 0)
            held = std::min(held, static_cast<double>(p.MhzLimit) / p.MaxMhz);
    }
    if (cores == 0)
        return;
    r.cpu = true;
    r.cpu_live = live;
    r.cpu_mhz = static_cast<unsigned>((weight > 0 ? weighed / weight : all / cores) + 0.5);
    r.cpu_peak_mhz = static_cast<unsigned>((peak_busy > 0 ? peak_busy : peak) + 0.5);
    r.cpu_max_mhz = max;
    r.cpu_limit_mhz = static_cast<unsigned>(max * held + 0.5);
}

ULONGLONG filetime(const FILETIME& t)
{
    ULARGE_INTEGER v;
    v.LowPart = t.dwLowDateTime, v.HighPart = t.dwHighDateTime;
    return v.QuadPart;
}

ULONGLONG process_cpu()
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return 0;
    return filetime(kernel) + filetime(user); // 100 ns
}

// The times Windows counts for every core added up (100 ns): idle, all of it, and this game's
struct Load {
    ULONGLONG idle = 0, all = 0, game = 0;
};
Load g_load; // the poll thread's: the last poll's

// How busy the CPU has been since the last poll and this game's share of it, as Task Manager has them (% of every core's
// time); this game's memory, and the PC's
void poll_load(Reading& r)
{
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const Load now{filetime(idle), filetime(kernel) + filetime(user), process_cpu()}; // the kernel's time has the idle in it
        const Load was = g_load;
        g_load = now;
        if (was.all != 0 && now.all > was.all && now.idle >= was.idle && now.game >= was.game) {
            const double all = static_cast<double>(now.all - was.all);
            r.cpu_load = true;
            r.cpu_busy = std::clamp(100.0 * (1.0 - static_cast<double>(now.idle - was.idle) / all), 0.0, 100.0);
            r.cpu_game = std::clamp(100.0 * static_cast<double>(now.game - was.game) / all, 0.0, 100.0);
        }
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof memory;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof memory))
        r.ram_game_mb = static_cast<double>(memory.PrivateUsage) / (1024.0 * 1024.0);
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status)) {
        r.ram_total_mb = static_cast<double>(status.ullTotalPhys) / (1024.0 * 1024.0);
        r.ram_used_mb = static_cast<double>(status.ullTotalPhys - status.ullAvailPhys) / (1024.0 * 1024.0);
    }
}

// VanaCore.Dlss.exe --hw: started for the game's pid, reads NVML (64-bit only) and shares it (hw_ipc.h)
void start_helper()
{
    if (g_helper != nullptr)
        return;
    const std::wstring exe = g_install + L"dlss\\VanaCore.Dlss.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_helper_why = "dlss\\VanaCore.Dlss.exe isn't there: the graphics card's readings come from it";
        return;
    }
    std::wstring command = L"\"" + exe + L"\" --hw " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, (g_install + L"dlss").c_str(), &startup,
                        &process)) {
        g_helper_why = "couldn't start dlss\\VanaCore.Dlss.exe";
        return;
    }
    CloseHandle(process.hThread);
    g_helper = process.hProcess;
    g_helper_why.clear();
}

void stop_helper()
{
    if (g_shared != nullptr)
        UnmapViewOfFile(g_shared);
    g_shared = nullptr;
    if (g_map != nullptr)
        CloseHandle(g_map);
    g_map = nullptr;
    if (g_helper != nullptr) {
        TerminateProcess(g_helper, 0);
        CloseHandle(g_helper);
    }
    g_helper = nullptr;
}

void poll_gpu(Reading& r)
{
    if (g_shared == nullptr) {
        const std::wstring name = std::wstring(VANA_HW_MAP) + std::to_wstring(GetCurrentProcessId());
        g_map = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        if (g_map != nullptr)
            g_shared = static_cast<VanaHwShared*>(MapViewOfFile(g_map, FILE_MAP_READ, 0, 0, sizeof(VanaHwShared)));
    }
    if (g_shared == nullptr || g_shared->version != VANA_HW_VERSION) {
        r.gpu_why = g_helper_why.empty() ? "starting..." : g_helper_why;
        return;
    }
    if (g_shared->state == VANA_HW_UNAVAILABLE) {
        r.gpu_why = g_shared->message;
        return;
    }
    if (g_shared->state != VANA_HW_RUNNING || g_shared->tick == 0) {
        r.gpu_why = "starting...";
        return;
    }
    r.gpu = true;
    r.gpu_mhz = g_shared->gpu_mhz, r.gpu_max_mhz = g_shared->gpu_max_mhz;
    r.gpu_temp = g_shared->temp;
    r.gpu_watts = (g_shared->power_mw + 500) / 1000, r.gpu_limit_watts = (g_shared->limit_mw + 500) / 1000;
    r.gpu_busy = g_shared->busy;
    r.vram_used_mb = static_cast<double>(g_shared->vram_used) / (1024.0 * 1024.0);
    r.vram_total_mb = static_cast<double>(g_shared->vram_total) / (1024.0 * 1024.0);
    r.throttle = g_shared->throttle;
    r.throttle_text = throttle_words(r.throttle, r.throttled);
}

DWORD WINAPI poll_thread(LPVOID)
{
    g_load = {}; // a rate from this run's polls, not over the time nothing asked
    while (g_run.load()) {
        Reading r;
        poll_power(r);
        poll_cpu(r);
        poll_load(r);
        poll_gpu(r);
        {
            const std::lock_guard lock(g_lock);
            g_reading = std::move(r);
        }
        for (int i = 0; i < 10 && g_run.load(); ++i)
            Sleep(100);
    }
    close_counters(g_counters);
    return 0;
}

// ---- the telemetry -------------------------------------------------------------------------------------------------

constexpr double kInterval = 5.0; // s between lines

struct Telemetry {
    bool on = false;
    FILE* file = nullptr;
    LARGE_INTEGER started{}, window{}, last_frame{};
    unsigned frames = 0;
    double frame_sum = 0, worst = 0;
    ULONGLONG cpu_last = 0;
} g_telemetry;
LARGE_INTEGER g_frequency{};

void telemetry_line(double seconds, const LARGE_INTEGER& now)
{
    Telemetry& t = g_telemetry;
    const Reading r = latest();
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof memory;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof memory);
    const ULONGLONG cpu = process_cpu();
    const double cores = t.cpu_last != 0 && cpu >= t.cpu_last ? static_cast<double>(cpu - t.cpu_last) / 1.0e7 / seconds : 0.0;
    t.cpu_last = cpu;
    SYSTEMTIME time;
    GetLocalTime(&time);
    const double since = static_cast<double>(now.QuadPart - t.started.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    std::string plan = r.power_plan; // a column of its own: no commas
    std::replace(plan.begin(), plan.end(), ',', ';');
    std::fprintf(t.file, "%02u:%02u:%02u,%.0f,%.1f,%.2f,%.2f,%u,%u,%u,%u,%d,%u,%u,%u,%.0f,%.0f,%.0f,%.2f,%s,%d,%d,%s,%s\n", time.wHour, time.wMinute,
                 time.wSecond, since, t.frames / seconds, t.frames ? t.frame_sum / t.frames : 0.0, t.worst, r.cpu_mhz, r.cpu_max_mhz, r.cpu_limit_mhz,
                 r.gpu_mhz, r.gpu_temp, r.gpu_watts, r.gpu_limit_watts, r.gpu_busy, r.vram_used_mb, r.vram_total_mb,
                 static_cast<double>(memory.PrivateUsage) / (1024.0 * 1024.0), cores,
                 r.gpu ? r.throttle_text.c_str() : (r.gpu_why.empty() ? "" : "no GPU reading"), r.power ? (r.ac ? 1 : 0) : -1, r.battery,
                 plan.c_str(), r.power_mode.c_str());
    std::fflush(t.file);
    t.frames = 0, t.frame_sum = 0, t.worst = 0;
    t.window = now;
}

void telemetry_stop()
{
    Telemetry& t = g_telemetry;
    if (!t.on)
        return;
    t.on = false;
    if (t.file != nullptr)
        std::fclose(t.file);
    t.file = nullptr;
    release();
}

bool telemetry_start()
{
    Telemetry& t = g_telemetry;
    if (t.on)
        return true;
    CreateDirectoryW(g_logs.c_str(), nullptr);
    SYSTEMTIME time;
    GetLocalTime(&time);
    wchar_t name[64];
    swprintf_s(name, L"telemetry-%04u-%02u-%02u-%02u%02u%02u.csv", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    if (_wfopen_s(&t.file, (g_logs + name).c_str(), L"wb") != 0 || t.file == nullptr)
        return false;
    std::fputs("\xEF\xBB\xBF", t.file);
    std::fputs("time,seconds,fps,frame_ms,worst_frame_ms,cpu_mhz,cpu_max_mhz,cpu_limit_mhz,gpu_mhz,gpu_temp_c,gpu_watts,gpu_limit_watts,gpu_busy_pct,"
               "vram_used_mb,vram_total_mb,game_private_mb,game_cpu_cores,gpu_throttle,plugged_in,battery_pct,windows_power_plan,windows_power_mode\n",
               t.file);
    t.on = true;
    t.frames = 0, t.frame_sum = 0, t.worst = 0, t.cpu_last = 0;
    QueryPerformanceCounter(&t.started);
    t.window = t.started;
    t.last_frame = {};
    acquire();
    chat::echo("telemetry: a line every 5 s to logs\\" + narrow(name));
    return true;
}

} // namespace

void install(const wchar_t* install_dir)
{
    g_install = install_dir != nullptr ? install_dir : L"";
    if (!g_install.empty() && g_install.back() != L'\\')
        g_install += L'\\';
    g_logs = g_install + L"logs\\";
    QueryPerformanceFrequency(&g_frequency);
}

void acquire()
{
    const std::lock_guard lock(g_lock);
    if (g_users++ > 0)
        return;
    start_helper();
    g_run = true;
    g_thread = CreateThread(nullptr, 0, &poll_thread, nullptr, 0, nullptr);
}

void release()
{
    HANDLE thread = nullptr;
    {
        const std::lock_guard lock(g_lock);
        if (g_users == 0 || --g_users > 0)
            return;
        g_run = false;
        thread = g_thread;
        g_thread = nullptr;
    }
    if (thread != nullptr) {
        WaitForSingleObject(thread, 2000);
        CloseHandle(thread);
    }
    stop_helper();
}

void touch()
{
    g_touch_until = GetTickCount64() + kTouchHold;
    if (!g_touch_held.exchange(true))
        acquire();
}


Reading latest()
{
    Reading r;
    {
        const std::lock_guard lock(g_lock);
        r = g_reading;
    }
    r.vram_game_mb = static_cast<double>(g_vram_game.load()) / (1024.0 * 1024.0);
    r.vram_budget_mb = static_cast<double>(g_vram_budget.load()) / (1024.0 * 1024.0);
    return r;
}

void on_frame()
{
    // this game's video memory, from DXGI, once a second while the readings are wanted
    if (g_users.load() > 0) {
        const ULONGLONG tick = GetTickCount64();
        if (tick - g_vram_tick >= 1000) {
            g_vram_tick = tick;
            unsigned long long used = 0, budget = 0;
            if (!vanacore_gfx_video_memory(&used, &budget))
                used = 0, budget = 0;
            g_vram_game = used, g_vram_budget = budget;
        }
    }
    // touch()'s hold lapsed (the page showing the readings was left): let the poll thread and the helper go
    if (g_touch_held.load() && GetTickCount64() > g_touch_until.load() && g_touch_held.exchange(false))
        release();
    Telemetry& t = g_telemetry;
    if (!t.on)
        return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (t.last_frame.QuadPart != 0) {
        const double ms = static_cast<double>(now.QuadPart - t.last_frame.QuadPart) * 1000.0 / static_cast<double>(g_frequency.QuadPart);
        ++t.frames;
        t.frame_sum += ms;
        t.worst = std::max(t.worst, ms);
    }
    t.last_frame = now;
    const double seconds = static_cast<double>(now.QuadPart - t.window.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    if (seconds >= kInterval)
        telemetry_line(seconds, now);
}

bool command(const std::vector<std::string>& parts)
{
    const std::string option = parts.size() > 1 ? parts[1] : "";
    if (option != "" && option != "on" && option != "off") {
        chat::echo("/vana telemetry [on|off]: a line every 5 s (frame rate, CPU and GPU clocks, temperature, power, memory) in logs\\telemetry-*.csv");
        return true;
    }
    const bool want = option == "on" ? true : option == "off" ? false : !g_telemetry.on;
    if (want) {
        if (!telemetry_start())
            chat::echo("telemetry: the file couldn't be made in logs\\");
    } else {
        telemetry_stop();
        chat::echo("telemetry: stopped");
    }
    return true;
}

} // namespace vanacore::hwmon
