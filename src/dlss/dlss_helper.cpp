// VanaCore.Dlss.exe: NVIDIA DLSS and AMD FSR for VanaCore. Both come for 64-bit programs only and FFXI is 32-bit, so
// this 64-bit helper runs them on textures the game's process shares with it (dlss_ipc.h), on a device of its own on
// the same GPU. Started by VanaCore with the game's pid, for the one upscaler the game asks for; ends when the game
// does, or when the game lets it go (to start one for the other upscaler).
//
//   VanaCore.Dlss.exe <game pid> [log file]   serve the game
//   VanaCore.Dlss.exe --check                 say whether DLSS (and DLSS 5), FSR and their frame generation run on
//                                             this machine (exit 0 if either upscaler does)
//   VanaCore.Dlss.exe --hw <game pid>         read the graphics card's clock, temperature, power and memory for the game
//                                             once a second (hw_ipc.h), until it ends

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dbghelp.h>
#include <intrin.h>
#include <psapi.h>

#include <atomic>
#include <cctype>
#include <cwctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

// AMD's FSR API (third_party/fsr): its headers only; its DLLs are loaded by name (fsr_start)
#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "upscalers/include/ffx_upscale.h"

#include "dlss_ipc.h"
#include "hw_ipc.h"
#include "../shared/vanacore/log_file.hpp"

namespace {

// NGX's id for a program on no engine it knows: any unique string (a GUID)
constexpr char kProjectId[] = "6f0c1d52-3a8e-4b7d-9e41-2c5a8f9d7b13";
constexpr char kEngineVersion[] = "1.0";

FILE* g_log = nullptr;
bool g_quiet = false; // serving the game: nothing goes to stderr (nobody reads it)
bool g_fsr_mode = false;               // serving FSR (the game's `upscaler`), not DLSS
const char* g_who = "DLSS helper";     // ... what its log lines start with

void say(const char* format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_log) {
        fprintf(g_log, "%04u-%02u-%02u %02u:%02u:%02u.%03u %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, text);
        fflush(g_log);
    }
    if (!g_quiet)
        fprintf(stderr, "%s\n", text);
}

// What the helper is doing ("running DLSS 5, pass 1 of 2"), and on which of the game's frames (its fence value; 0 none): for
// a crash's report (crash::), and in the shared memory (doing, doing_frame), which the game reads once the helper has ended -
// there even when Windows ended it on the spot, without the report (a fail-fast, heap corruption)
VanaDlssShared* g_sh = nullptr;
// The game's ".done" and ".fgdone" events, opened only with sh->cpu_sync (no fence shared: under Wine and Proton, which
// can't open one from another process): set once a frame's work is through, in place of the fence signal the game waits on
HANDLE g_game_done = nullptr, g_game_done_fg = nullptr;
char g_doing[96] = "starting";
uint64_t g_doing_frame = 0;
char g_card[128] = "";     // the graphics card it's on (make_device)
char g_ngx_last[256] = ""; // NGX's last log line (ngx_log): what it said just before a crash

void doing(const char* what, uint64_t frame = 0)
{
    snprintf(g_doing, sizeof g_doing, "%s", what);
    g_doing_frame = frame;
    if (g_sh) {
        g_sh->doing_frame = frame;
        snprintf(g_sh->doing, sizeof g_sh->doing, "%s", what);
    }
}

struct Gpu {
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    // A ring of allocators: a frame takes a list for DLSS and one for each frame generation makes, and the GPU is a
    // frame or two behind (the game's work first) - with two, recording waited on the frame before's GPU work and
    // handed frames over late (the game then showed a frame without DLSS: flicker)
    static constexpr int kAllocs = 16;
    ID3D12CommandAllocator* alloc[kAllocs] = {};
    uint64_t alloc_done[kAllocs] = {}; // the own fence's value when each allocator's last list is through
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* own = nullptr; // this side's progress, for reusing allocators
    uint64_t own_value = 0;
    HANDLE wake = nullptr;
    int turn = 0;
};
Gpu g;

std::string adapter_name(IDXGIAdapter1* a)
{
    DXGI_ADAPTER_DESC1 d;
    if (FAILED(a->GetDesc1(&d)))
        return "?";
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
    return name;
}

// --- a crash's report ------------------------------------------------------------------------------------------------
// The helper crashing (in NVIDIA's or AMD's DLLs, the graphics driver's, or its own): what, where, while doing what, the
// call stack, NGX's last word, the DLLs with their versions (DLSS's, the driver's) and a dump, into dlss.log and the game's
// own vanacore.log, so the game's log says why. As VanaCore.Core's crash handler (core/crash.cpp), for 64 bits: written
// by a thread of its own (the crashed one's stack may be all but gone), each line in one write. Then the helper ends with
// the crash's code, which the game reads (gfx_d3d12.c helper_exit_text), without Windows' "has stopped working" box. A
// crash Windows ends on the spot (a fail-fast, heap corruption) skips all this: the game still has the code, and `doing`.
namespace crash {

HANDLE g_files[2] = {INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE}; // dlss.log, and the game's vanacore.log beside it
std::wstring g_dir;          // the logs folder: the dump goes there
int g_dumps = 1;             // the game's [logging] crashdumps (VANA_CRASHDUMPS): 0 the first line alone, 1 the report and a
                             // dump, 2 a dump of all of the helper's memory
decltype(&MiniDumpWriteDump) g_write_dump = nullptr; // dbghelp.dll's, loaded up front: nothing is loaded once it's crashed
HANDLE g_request = nullptr, g_done = nullptr;
EXCEPTION_POINTERS* g_exception = nullptr;
DWORD g_thread = 0;  // the crashed thread
DWORD g_serving = 0; // the thread that serves the game (serve's)
std::atomic<bool> g_crashing{false};

// "[VanaCore.Dlss.exe] CRASH: ..." into both logs (VanaCore.exe's own lines are marked so in vanacore.log), in one write
void line(const wchar_t* format, ...)
{
    wchar_t text[1536];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t whole[1600];
    swprintf_s(whole, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [VanaCore.Dlss.exe] CRASH: %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, t.wMilliseconds, text);
    char utf8[4800];
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, whole, -1, utf8, sizeof utf8, nullptr, nullptr);
    for (HANDLE file : g_files) {
        DWORD written = 0;
        if (file != INVALID_HANDLE_VALUE && bytes > 1)
            WriteFile(file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
    }
}

// The call stack from the crash's registers, by the modules' unwind data (every x64 function but a leaf has it), into pcs;
// how many. A frame that can't be read ends it.
int walk(const CONTEXT& from, DWORD64* pcs, int most)
{
    CONTEXT c = from;
    int n = 0;
    __try {
        while (n < most && c.Rip != 0) {
            pcs[n++] = c.Rip;
            const DWORD64 rip = c.Rip, rsp = c.Rsp;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (f == nullptr) {
                // a leaf: its return address is on the stack's top
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            } else {
                void* data = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, f, &c, &data, &frame, nullptr);
            }
            if (c.Rip == rip && c.Rsp == rsp)
                break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

bool is_code(DWORD64 address)
{
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != 0 && info.State == MEM_COMMIT &&
           info.Type == MEM_IMAGE &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Code addresses in the stack's top 4 KB: return addresses the walk can miss (code without unwind data), into at (their
// place from RSP) and pc; how many
int scan(DWORD64 rsp, DWORD64* at, DWORD64* pc, int most)
{
    int hits = 0;
    for (DWORD64 a = rsp; hits < most && a < rsp + 4096; a += 8) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(a), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) == 0 ||
            (info.Protect & PAGE_GUARD) != 0)
            break;
        const DWORD64 value = *reinterpret_cast<const DWORD64*>(a);
        if (is_code(value))
            at[hits] = a - rsp, pc[hits] = value, ++hits;
    }
    return hits;
}

// Under Windows' own folder, and not a driver's (DriverStore holds the graphics driver's DLLs, which a report wants)
bool windows_own(const wchar_t* path)
{
    wchar_t windir[MAX_PATH];
    const UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH || _wcsnicmp(path, windir, n) != 0)
        return false;
    std::wstring lower = path;
    for (wchar_t& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    return lower.find(L"\\driverstore\\") == std::wstring::npos;
}

// A loaded DLL's file version from its own version resource ("310.4.0.0"; "" without one)
std::wstring version_of(HMODULE module)
{
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16)); // VS_VERSION_INFO, RT_VERSION
    HGLOBAL loaded = found ? LoadResource(module, found) : nullptr;
    const BYTE* p = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    const DWORD size = found ? SizeofResource(module, found) : 0;
    for (DWORD at = 0; p && at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
        const auto* f = reinterpret_cast<const VS_FIXEDFILEINFO*>(p + at);
        if (f->dwSignature != 0xFEEF04BD)
            continue;
        wchar_t v[48];
        swprintf_s(v, L"%u.%u.%u.%u", HIWORD(f->dwFileVersionMS), LOWORD(f->dwFileVersionMS), HIWORD(f->dwFileVersionLS),
                   LOWORD(f->dwFileVersionLS));
        return v;
    }
    return L"";
}

// The DLLs other than Windows' own: path, version, base address, size and build time
void modules()
{
    HMODULE list[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), list, sizeof(list), &bytes))
        return;
    const DWORD count = bytes / sizeof(HMODULE) < 512 ? bytes / sizeof(HMODULE) : 512;
    int windows = 0;
    for (DWORD i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(list[i], path, MAX_PATH);
        if (windows_own(path)) {
            ++windows;
            continue;
        }
        MODULEINFO info{};
        K32GetModuleInformation(GetCurrentProcess(), list[i], &info, sizeof(info));
        const auto* base = reinterpret_cast<const BYTE*>(list[i]);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const __time64_t built = nt->FileHeader.TimeDateStamp;
        tm time{};
        wchar_t when[32] = L"?";
        if (_gmtime64_s(&time, &built) == 0)
            wcsftime(when, 32, L"%Y-%m-%d %H:%M:%S UTC", &time);
        const std::wstring version = version_of(list[i]);
        line(L"module %ls%ls%ls at 0x%p, %lu bytes, built %ls", path, version.empty() ? L"" : L" ", version.c_str(), list[i],
             info.SizeOfImage, when);
    }
    line(L"plus %d Windows modules", windows);
}

// The helper's device lost (its graphics card stopped: a driver timeout): Windows' reason; S_OK, not lost
HRESULT device_removed()
{
    __try {
        return g.dev != nullptr ? g.dev->GetDeviceRemovedReason() : S_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return S_OK;
    }
}

void dump()
{
    if (g_write_dump == nullptr) {
        line(L"no dump: dbghelp.dll didn't load");
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"dlss-crash-%04u-%02u-%02u_%02u-%02u-%02u-pid%lu.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, GetCurrentProcessId());
    const std::wstring path = g_dir + name;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        line(L"couldn't create %ls (error %lu)", path.c_str(), GetLastError());
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(
        g_dumps >= 2 ? MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                           MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                           MiniDumpIgnoreInaccessibleMemory
                     : MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                           MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo |
                           MiniDumpWithThreadInfo | MiniDumpIgnoreInaccessibleMemory);
    MINIDUMP_EXCEPTION_INFORMATION info{g_thread, g_exception, FALSE};
    const BOOL written = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
    const DWORD error = GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (written)
        line(L"dump written to %ls (%.1f MB)", path.c_str(), static_cast<double>(size.QuadPart) / (1024.0 * 1024.0));
    else
        line(L"writing %ls failed (0x%08lX)", path.c_str(), error);
}

// Everything after the first line
void report()
{
    const CONTEXT& c = *g_exception->ContextRecord;
    line(L"registers RIP=%016llX RSP=%016llX RBP=%016llX RAX=%016llX RBX=%016llX RCX=%016llX RDX=%016llX RSI=%016llX "
         L"RDI=%016llX",
         c.Rip, c.Rsp, c.Rbp, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi);
    line(L"registers R8=%016llX R9=%016llX R10=%016llX R11=%016llX R12=%016llX R13=%016llX R14=%016llX R15=%016llX EFLAGS=%08lX",
         c.R8, c.R9, c.R10, c.R11, c.R12, c.R13, c.R14, c.R15, c.EFlags);
    DWORD64 pcs[48];
    const int n = walk(c, pcs, 48);
    std::wstring text;
    for (int i = 0; i < n; ++i) {
        text += L" #" + std::to_wstring(i) + L" " + vanacore::describe_address(reinterpret_cast<const void*>(pcs[i]));
        if ((i + 1) % 8 == 0)
            line(L"stack%ls", text.c_str()), text.clear();
    }
    if (!text.empty())
        line(L"stack%ls", text.c_str()), text.clear();
    DWORD64 at[24], pc[24];
    const int hits = scan(c.Rsp, at, pc, 24);
    for (int i = 0; i < hits; ++i) {
        text += L" [rsp+" + std::to_wstring(at[i]) + L"] " + vanacore::describe_address(reinterpret_cast<const void*>(pc[i]));
        if ((i + 1) % 8 == 0)
            line(L"code on the stack%ls", text.c_str()), text.clear();
    }
    if (!text.empty())
        line(L"code on the stack%ls", text.c_str());
    const HRESULT removed = device_removed();
    if (removed != S_OK)
        line(L"its graphics device was lost first (0x%08lX: the card stopped, a driver timeout): the crash followed", removed);
    if (g_ngx_last[0] != '\0')
        line(L"NGX's last message: %hs", g_ngx_last);
    if (g_card[0] != '\0')
        line(L"on %hs", g_card);
    modules();
    dump();
}

DWORD WINAPI report_thread(void*)
{
    WaitForSingleObject(g_request, INFINITE);
    report();
    SetEvent(g_done);
    return 0;
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD& r = *info->ExceptionRecord;
    if (!g_crashing.exchange(true)) {
        const wchar_t* name = vanacore::describe_status(r.ExceptionCode);
        const std::wstring where = vanacore::describe_address(r.ExceptionAddress);
        wchar_t access[64] = L"";
        if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
            swprintf_s(access, L" %ls 0x%p",
                       r.ExceptionInformation[0] == 0 ? L"reading" : r.ExceptionInformation[0] == 1 ? L"writing" : L"executing",
                       reinterpret_cast<void*>(r.ExceptionInformation[1]));
        wchar_t frame[48] = L"";
        if (g_doing_frame != 0)
            swprintf_s(frame, L" (the game's frame %llu)", g_doing_frame);
        line(L"the %hs crashed: %ls (0x%08lX) at %ls%ls (thread %lu%ls), while %hs%ls", g_who,
             name != nullptr ? name : L"unhandled exception", r.ExceptionCode, where.c_str(), access, GetCurrentThreadId(),
             GetCurrentThreadId() == g_serving ? L", the one serving the game" : L"", g_doing, frame);
        if (g_dumps > 0) {
            g_exception = info, g_thread = GetCurrentThreadId();
            if (g_request != nullptr) {
                SetEvent(g_request);
                WaitForSingleObject(g_done, g_dumps >= 2 ? 300'000 : 60'000);
            } else {
                report();
            }
        }
    } else if (g_done != nullptr) {
        WaitForSingleObject(g_done, 60'000); // another thread's report under way: the helper ends with it
    }
    TerminateProcess(GetCurrentProcess(), r.ExceptionCode);
    return EXCEPTION_CONTINUE_SEARCH;
}

// From serve(): the logs (log_path, dlss.log, beside the game's vanacore.log; none, the game's logging off: nothing written
// and no dump, as the game's own crash handler), and the handler
void install(const wchar_t* log_path)
{
    g_serving = GetCurrentThreadId();
    wchar_t text[8] = L"";
    if (GetEnvironmentVariableW(L"VANA_CRASHDUMPS", text, 8) != 0)
        g_dumps = _wtoi(text);
    if (log_path == nullptr || log_path[0] == L'\0')
        g_dumps = 0;
    else {
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        g_files[0] = CreateFileW(log_path, FILE_APPEND_DATA, share, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        const std::wstring path = log_path;
        g_dir = path.substr(0, path.find_last_of(L"\\/") + 1);
        // the game's own log, if it's there (VanaCore makes it): never made here
        g_files[1] = CreateFileW((g_dir + L"vanacore.log").c_str(), FILE_APPEND_DATA, share, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (g_dumps > 0) {
        if (HMODULE dbghelp = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
            g_write_dump = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
        g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr); // set once: every thread that crashed waits on it
        HANDLE thread = g_request != nullptr && g_done != nullptr ? CreateThread(nullptr, 0, &report_thread, nullptr, 0, nullptr)
                                                                  : nullptr;
        if (thread != nullptr)
            CloseHandle(thread);
        else
            g_request = nullptr; // the report on the crashed thread instead
    }
    SetUnhandledExceptionFilter(&on_crash);
}

} // namespace crash

// The debug window's DLSS crash tests (crash_test, the game's): the helper fails as asked, where it's called (run_frame: at
// the frame's upscale, where a real crash would come), each a different way out for what the game and its log then show
DWORD WINAPI crash_test_thread(void*)
{
    int* volatile nowhere = nullptr;
    *nowhere = 1;
    return 0;
}

void crash_test(int kind)
{
    say("%s: crash test %d, as the game asked (/vana debug)", g_who, kind);
    if (kind == VANA_CRASH_TEST_AV) {
        int* volatile nowhere = nullptr;
        *nowhere = 1;
    } else if (kind == VANA_CRASH_TEST_THREAD) {
        HANDLE thread = CreateThread(nullptr, 0, &crash_test_thread, nullptr, 0, nullptr);
        if (thread != nullptr)
            WaitForSingleObject(thread, INFINITE); // the helper ends with that thread's crash
    } else if (kind == VANA_CRASH_TEST_CPP) {
        throw std::runtime_error("VanaCore's DLSS crash test");
    } else if (kind == VANA_CRASH_TEST_FASTFAIL) {
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    } else if (kind == VANA_CRASH_TEST_HANG) {
        Sleep(INFINITE);
    }
}

// --- the GPU's own timing of the helper's jobs ---------------------------------------------------------------------
// Timestamps on this queue round DLSS 5, DLSS and each frame frame generation makes, read back when the list's
// allocator comes round again (begin()), and handed to the game in the shared block for /vana stats. The game's
// timestamps can't tell: its queue only waits for this one.

constexpr int kTsPerList = 4;
ID3D12QueryHeap* g_ts = nullptr;
ID3D12Resource* g_ts_rb = nullptr;
uint64_t* g_ts_cpu = nullptr;
uint64_t g_ts_freq = 0;
int g_ts_kind[Gpu::kAllocs] = {}; // what each list's timestamps are of: 1 DLSS (5 and DLSS), 2 a frame generation's
bool g_ts_nr[Gpu::kAllocs] = {};
float g_gpu_nr_ms = 0.0f, g_gpu_sr_ms = 0.0f, g_gpu_fg_ms = 0.0f;
bool g_gpu_timed = false;

void ts_make()
{
    D3D12_QUERY_HEAP_DESC qh = {D3D12_QUERY_HEAP_TYPE_TIMESTAMP, Gpu::kAllocs * kTsPerList, 0};
    if (FAILED(g.dev->CreateQueryHeap(&qh, IID_PPV_ARGS(&g_ts))) || FAILED(g.queue->GetTimestampFrequency(&g_ts_freq)))
        return;
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 8ull * Gpu::kAllocs * kTsPerList, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
    if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_ts_rb))))
        return;
    g_ts_rb->Map(0, nullptr, reinterpret_cast<void**>(&g_ts_cpu));
}

// A timestamp in the list being recorded (slot 0-3 of its own)
void ts_mark(int slot)
{
    if (g_ts)
        g.list->EndQuery(g_ts, D3D12_QUERY_TYPE_TIMESTAMP, g.turn * kTsPerList + slot);
}

// The list's timestamps made readable: `count` of them, what they're of
void ts_resolve(int count, int kind, bool nr)
{
    if (!g_ts)
        return;
    g.list->ResolveQueryData(g_ts, D3D12_QUERY_TYPE_TIMESTAMP, g.turn * kTsPerList, count, g_ts_rb, 8ull * g.turn * kTsPerList);
    g_ts_kind[g.turn] = kind;
    g_ts_nr[g.turn] = nr;
}

// What the list this allocator last had recorded (which has finished), read
void ts_collect(int turn)
{
    if (!g_ts_cpu || !g_ts_freq || g_ts_kind[turn] == 0)
        return;
    const uint64_t* t = &g_ts_cpu[turn * kTsPerList];
    const double ms = 1000.0 / static_cast<double>(g_ts_freq);
    if (g_ts_kind[turn] == 1) {
        g_gpu_nr_ms = g_ts_nr[turn] && t[1] > t[0] ? static_cast<float>(static_cast<double>(t[1] - t[0]) * ms) : 0.0f;
        g_gpu_sr_ms = t[2] > t[1] ? static_cast<float>(static_cast<double>(t[2] - t[1]) * ms) : 0.0f;
    } else {
        g_gpu_fg_ms = t[1] > t[0] ? static_cast<float>(static_cast<double>(t[1] - t[0]) * ms) : 0.0f;
    }
    g_gpu_timed = true;
    g_ts_kind[turn] = 0;
}

bool g_nvidia = false; // the device is on an NVIDIA card (--check: DLSS is asked for only there)

// The device: on the adapter with `luid` when there is one, else (--check) the first NVIDIA one, or the first other
// graphics card where there's none (FSR's)
bool make_device(const LUID* luid, std::string& why)
{
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        why = "no DXGI factory";
        return false;
    }
    IDXGIAdapter1* adapter = nullptr;
    if (luid) {
        if (FAILED(factory->EnumAdapterByLuid(*luid, IID_PPV_ARGS(&adapter))))
            adapter = nullptr;
    } else {
        IDXGIAdapter1* other = nullptr;
        for (UINT i = 0; !adapter; ++i) {
            IDXGIAdapter1* a = nullptr;
            if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 d;
            const bool hardware = SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
            if (hardware && d.VendorId == 0x10DE)
                adapter = a;
            else if (hardware && !other)
                other = a;
            else
                a->Release();
        }
        if (!adapter)
            adapter = other, other = nullptr;
        if (other)
            other->Release();
    }
    factory->Release();
    if (!adapter) {
        why = luid ? "the game's graphics card wasn't found" : "no graphics card";
        return false;
    }
    DXGI_ADAPTER_DESC1 desc;
    g_nvidia = SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE;
    snprintf(g_card, sizeof g_card, "%s", adapter_name(adapter).c_str());
    say("%s: on %s", g_who, g_card);
    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.dev));
    adapter->Release();
    if (FAILED(hr)) {
        why = "no Direct3D 12 device on it";
        return false;
    }
    // The helper's queue at high priority where Windows gives it: its work is what the game's GPU queue waits on
    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    bool allocs = true;
    for (auto*& a : g.alloc)
        allocs = allocs && SUCCEEDED(g.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
    if (FAILED(g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) {
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        g.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue));
    }
    if (!g.queue || !allocs ||
        FAILED(g.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[0], nullptr, IID_PPV_ARGS(&g.list))) ||
        FAILED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.own)))) {
        why = "couldn't set up its queue";
        return false;
    }
    g.list->Close();
    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ts_make();
    return true;
}

// The time on QueryPerformanceCounter's clock (the game's too), and what a frame's work spent where, for its log's
// "late" lines
int64_t qpc()
{
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return q.QuadPart;
}

double qpc_ms(int64_t ticks)
{
    static const double per_ms = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    return static_cast<double>(ticks) / per_ms;
}

int64_t g_list_wait = 0; // this frame's waits in begin() for a free list (the GPU behind on the helper's lists)

// A frame handed over by the game at `posted`, done now: in the log when it took more than 8 ms (at most 5 a second),
// with where the time went
void note_frame(const char* what, uint64_t frame, int64_t posted, int64_t woke, int64_t evaluate, int64_t submit)
{
    const int64_t now = qpc();
    if (posted == 0 || qpc_ms(now - posted) < 8.0)
        return;
    static int64_t second_at = 0;
    static int said = 0;
    if (qpc_ms(now - second_at) > 1000.0)
        second_at = now, said = 0;
    if (++said > 5)
        return;
    say("%s: %s frame %llu late: done %.1f ms after the game handed it over (woke %.1f ms after; waited %.1f "
        "ms for a free list; %s's evaluate %.1f ms; submitting %.1f ms)",
        g_who, what, static_cast<unsigned long long>(frame), qpc_ms(now - posted), qpc_ms(woke - posted), qpc_ms(g_list_wait),
        g_fsr_mode ? "AMD" : "NVIDIA", qpc_ms(evaluate), qpc_ms(submit));
}

// A list to record into: the next allocator, once its last list is through
ID3D12GraphicsCommandList* begin()
{
    g.turn = (g.turn + 1) % Gpu::kAllocs;
    if (g.own->GetCompletedValue() < g.alloc_done[g.turn]) {
        const int64_t from = qpc();
        g.own->SetEventOnCompletion(g.alloc_done[g.turn], g.wake);
        WaitForSingleObject(g.wake, 2000);
        g_list_wait += qpc() - from;
    }
    ts_collect(g.turn);
    g.alloc[g.turn]->Reset();
    g.list->Reset(g.alloc[g.turn], nullptr);
    return g.list;
}

void finish(bool wait)
{
    g.list->Close();
    ID3D12CommandList* l = g.list;
    g.queue->ExecuteCommandLists(1, &l);
    g.queue->Signal(g.own, ++g.own_value);
    g.alloc_done[g.turn] = g.own_value;
    if (wait && g.own->GetCompletedValue() < g.own_value) {
        g.own->SetEventOnCompletion(g.own_value, g.wake);
        WaitForSingleObject(g.wake, 5000);
    }
}

// Nothing of this side's left on the queue: before letting go of what its lists used. (Not finish(): with no new
// list recorded that runs the last one again, on whatever was just released.)
void idle()
{
    g.queue->Signal(g.own, ++g.own_value);
    if (g.own->GetCompletedValue() < g.own_value) {
        g.own->SetEventOnCompletion(g.own_value, g.wake);
        WaitForSingleObject(g.wake, 5000);
    }
}

NVSDK_NGX_Parameter* g_caps = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Parameter* g_fg_params = nullptr; // frame generation's own
bool g_fg_available = false;
std::string g_fg_why;
int g_fg_max = 1; // frames made between two at most: 3 (4x) on RTX 50
NVSDK_NGX_Handle* g_feature = nullptr;

// NGX's own log lines, into this log: DLSS 5's DLL says which network each model number gets ("DLSSNR: Created feature 1
// (output 3840x2160, network ...x..., preset=0 -> name)", or that a number isn't in the DLL and what it falls back to), which is
// how the numbers of /vana's "Model (to try)" are told apart. Every line NGX gives, the first 600 of a run, and only when this
// log is on.
//
// Logging on or not, the lines that speak of an error or a failure are kept as well: NGX's own reason for refusing DLSS 5,
// which /vana shows beside the result code. The last such line while a DLSS 5 feature is made or run, and failing that the
// first one that names DLSS 5's DLL (NGX says why it won't load a DLL when it starts, before the feature is asked for).
std::mutex g_why_lock;
bool g_why_watch = false;
std::string g_why;     // the last in the window why_watch opens
std::string g_why_dll; // the first to name nvngx_dlssnr.dll

// A path in a line down to its file name (a drive's path: "C:\..." the line is shorter for)
std::string without_dirs(const std::string& line)
{
    std::string out;
    size_t at = 0;
    while (at < line.size()) {
        size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\'' && line[end] != '"')
            ++end;
        const std::string token = line.substr(at, end - at);
        const bool path = token.size() > 3 && token[1] == ':' && (token[2] == '\\' || token[2] == '/');
        const size_t slash = token.find_last_of("\\/");
        out += path && slash != std::string::npos ? token.substr(slash + 1) : token;
        if (end < line.size())
            out += line[end];
        at = end + 1;
    }
    return out;
}

// An NGX line without its "[time] [tid][function:line]" front or a closing full stop
std::string ngx_text(const char* message)
{
    std::string line = message;
    size_t at = 0;
    while (at < line.size() && line[at] == '[') {
        const size_t close = line.find(']', at);
        if (close == std::string::npos)
            break;
        at = close + 1;
        while (at < line.size() && line[at] == ' ')
            ++at;
    }
    line.erase(0, at);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' || line.back() == '.'))
        line.pop_back();
    return without_dirs(line);
}

bool reads_as_why(const std::string& lower)
{
    for (const char* word : {"error", "fail", "unable", "unsupported", "not supported", "denied", "invalid", "refus", "signature",
                             "cannot", "can't", "couldn't", "not allowed", "not found", "out of memory", "mismatch"})
        if (lower.find(word) != std::string::npos)
            return true;
    // NGX's deny list: "Feature dlssnr, denial value 0 for ..." is the allowed one
    const size_t denial = lower.find("denial value ");
    return denial != std::string::npos && denial + 13 < lower.size() && lower[denial + 13] != '0';
}

void why_watch(bool on)
{
    std::lock_guard<std::mutex> lock(g_why_lock);
    g_why_watch = on;
    if (on)
        g_why.clear();
}

// `base` with ": " and NGX's reason after it where it gave one (closing the window why_watch opened), cut to /vana's
// message's size
std::string why_said(const char* base)
{
    std::lock_guard<std::mutex> lock(g_why_lock);
    g_why_watch = false;
    std::string text = base;
    const std::string& said = g_why.empty() ? g_why_dll : g_why;
    if (!said.empty())
        text += ": " + said;
    const size_t room = sizeof(VanaDlssShared::nr_message) - 1;
    return text.size() > room ? text.substr(0, room - 3) + "..." : text;
}

void NVSDK_CONV ngx_log(const char* message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature feature)
{
    if (message != nullptr) {
        // the text alone: its "[time] [tid][function:line]" front names functions ("IsFeatureDenied") that aren't the reason
        const std::string text = ngx_text(message);
        snprintf(g_ngx_last, sizeof g_ngx_last, "%s", text.c_str());
        std::string lower = text;
        for (char& c : lower)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (!text.empty() && reads_as_why(lower)) {
            std::lock_guard<std::mutex> lock(g_why_lock);
            if (g_why_watch)
                g_why = text;
            if (g_why_dll.empty() && lower.find("dlssnr") != std::string::npos)
                g_why_dll = text;
        }
    }
    static int count = 0;
    if (g_log == nullptr || message == nullptr || count >= 600)
        return;
    ++count;
    std::string line = message;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
        line.pop_back();
    say("NGX[%d,%d]: %s", static_cast<int>(level), static_cast<int>(feature), line.c_str());
}

bool ngx_start(const std::wstring& data, std::string& why)
{
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.LoggingInfo.LoggingCallback = &ngx_log;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
    common.LoggingInfo.DisableOtherLoggingSinks = false;
    NVSDK_NGX_Result r =
        NVSDK_NGX_D3D12_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data.c_str(), g.dev, &common);
    if (NVSDK_NGX_FAILED(r)) {
        char text[96];
        snprintf(text, sizeof text, "NVIDIA's NGX didn't start (0x%08X): not an NVIDIA RTX card, or its driver", r);
        why = text;
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_GetCapabilityParameters(&g_caps)) || !g_caps) {
        why = "NGX gave no capabilities";
        return false;
    }
    int available = 0, needs_driver = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (!available) {
        if (needs_driver) {
            unsigned int major = 0, minor = 0;
            NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
            NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
            char text[128];
            snprintf(text, sizeof text, "DLSS needs a newer NVIDIA driver (%u.%u or later)", major, minor);
            why = text;
        } else {
            why = "DLSS isn't available on this graphics card";
        }
        return false;
    }
    // frame generation (DLSS-FG): on RTX 40 and later; DLSS goes on without it
    int fg = 0, fg_driver = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_Available, &fg);
    NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &fg_driver);
    g_fg_available = fg != 0 && NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&g_fg_params)) && g_fg_params;
    g_fg_why = g_fg_available ? "" : fg_driver ? "frame generation needs a newer NVIDIA driver" : "frame generation needs an RTX 40 or later";
    unsigned int fg_max = 0;
    if (g_fg_available && NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &fg_max)))
        g_fg_max = fg_max > 3 ? 3 : fg_max < 1 ? 1 : (int)fg_max;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g_params)) || !g_params) {
        why = "NGX gave no parameters";
        return false;
    }
    return true;
}

// The render size DLSS asks for at `quality`, for an output out_w x out_h
void optimal(unsigned out_w, unsigned out_h, NVSDK_NGX_PerfQuality_Value quality, unsigned& w, unsigned& h)
{
    unsigned max_w = 0, max_h = 0, min_w = 0, min_h = 0;
    float sharpness = 0.0f;
    w = h = 0;
    NGX_DLSS_GET_OPTIMAL_SETTINGS(g_caps, out_w, out_h, quality, &w, &h, &max_w, &max_h, &min_w, &min_h, &sharpness);
}

template <typename T>
void release(T*& p)
{
    if (p)
        p->Release();
    p = nullptr;
}

// --- DLSS 5 (neural rendering) -------------------------------------------------------------------------------
// NGX's feature 18, in nvngx_dlssnr.dll beside this program (NVIDIA's; VanaCore doesn't ship it). It's asked of NGX
// as DLSS is, and NGX decides whether this program gets it: it loads the DLL itself, and only one it accepts
// (NVIDIA's signature whole, the program one it's offered to). Where it says no, DLSS runs alone. The colour goes
// through it at the render size before DLSS, with DLSS's depth and motion; its parameters' names (DLSSNR.*) aren't
// in NVIDIA's SDK yet: these are the ones its DLL reads.

constexpr NVSDK_NGX_Feature kNeuralRendering = NVSDK_NGX_Feature_Reserved18;

NVSDK_NGX_Parameter* g_nr_params = nullptr;
NVSDK_NGX_Handle* g_nr = nullptr;
ID3D12Resource* g_nr_out = nullptr; // its picture: the colour's size, R16G16B16A16_FLOAT; DLSS's colour then
bool g_nr_available = false;
std::string g_nr_why;

// More than one pass (nr_passes): the colour through DLSS 5, then that picture through it again. Each pass after the
// first is a feature of its own, so each keeps its own history (one feature run twice a frame would take the second run
// for a frame with nothing moved), made with the local tone and structure strengths at g_nr_taper of the first's (they
// compound pass on pass: at full strength the dark parts go black). A pass's picture is a texture of its own, the next
// pass's colour (the model can't read and write one texture); the last pass's is g_nr_out.
constexpr int kNrPasses = 3;
NVSDK_NGX_Handle* g_nr_more[kNrPasses - 1] = {}; // passes 2 and 3
ID3D12Resource* g_nr_mid[kNrPasses - 1] = {};    // the pictures of passes 1 and 2, where another pass follows
int g_nr_passes = 1;                             // the passes made
float g_nr_taper = 0.5f;

// DLSS 5 at a smaller size (nr_scale; nrscale below): the size, a share of the colour's each way (1: the colour's), the
// colour, depth and motion made that size for it, and its change laid back onto the colour at the colour's size (DLSS's
// colour then). g_nr_out, and the passes' pictures, are the small size then
float g_nr_scale = 1.0f;
ID3D12Resource* g_nr_small[3] = {};
ID3D12Resource* g_nr_full = nullptr;

struct NrFrame {
    unsigned w = 0, h = 0;   // the inputs' size, and the part of them drawn (from (0, 0))
    unsigned rw = 0, rh = 0;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* out = nullptr; // the picture it makes: g_nr_out where none is given
    float mv_x = 1.0f, mv_y = 1.0f; // the motion's scale to render pixels
    int inverted = 0, reset = 1;
    int style = 1;
    float intensity = 1.0f;
    float tone = 1.0f, structure = 1.0f, skin = -1.0f; // its local tone and structure strengths, the skin's (-1: its own)
    int auto_mask = 0;
    int preset = 0; // DLSS 5's model, as it numbers them (0: its default)
    int passes = 1; // with nr_make: how many passes (nr_passes), and the later passes' share of the strengths
    float taper = 0.5f;
    float scale = 1.0f; // ... and the size it runs at (nr_scale; the inputs given are the colour's size)
};

// A pass after the first: its local strengths at `taper` of the first's (the skin's -1 stays: it follows the structure)
NrFrame nr_tapered(const NrFrame& f, float taper)
{
    NrFrame t = f;
    t.tone = f.tone * taper, t.structure = f.structure * taper;
    t.skin = f.skin < 0.0f ? f.skin : f.skin * taper;
    return t;
}

ID3D12Resource* texture(DXGI_FORMAT format, unsigned w, unsigned h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                        HRESULT* result = nullptr)
{
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC d = {D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, format, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags};
    ID3D12Resource* r = nullptr;
    const HRESULT hr = g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
    if (result)
        *result = hr;
    return FAILED(hr) ? nullptr : r;
}

// Whether there's a DLSS 5 to ask for: its DLL beside this program (NGX's answer comes when the feature is made)
void nr_start()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dll = path;
    dll = dll.substr(0, dll.find_last_of(L"\\/") + 1) + L"nvngx_dlssnr.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_nr_why = "nvngx_dlssnr.dll isn't beside VanaCore.Dlss.exe";
        return;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g_nr_params)) || !g_nr_params) {
        g_nr_why = "NGX gave no parameters for DLSS 5";
        return;
    }
    int listed = 0;
    NVSDK_NGX_Parameter_GetI(g_caps, "DLSSNR.Available", &listed);
    say("DLSS helper: DLSS 5: nvngx_dlssnr.dll is here; NGX %s it among what this program can use", listed ? "lists" : "doesn't list");
    g_nr_available = true;
}

void nr_set(const NrFrame& f)
{
    NVSDK_NGX_Parameter* p = g_nr_params;
    p->Set("DLSSNR.Width", f.w), p->Set("DLSSNR.Height", f.h);
    p->Set("DLSSNR.Enabled", 1);
    p->Set("DLSSNR.Reset", f.reset);
    p->Set("DLSSNR.Style", f.style);
    p->Set("DLSSNR.Hint.Render.Preset", f.preset);
    p->Set("DLSSNR.Intensity", f.intensity);
    p->Set("DLSSNR.LocalToneStrength", f.tone), p->Set("DLSSNR.LocalStructureStrength", f.structure);
    p->Set("DLSSNR.SkinStructureStrength", f.skin);
    p->Set("DLSSNR.UseAutoMask", f.auto_mask), p->Set("DLSSNR.UICorrection", 0);
    p->Set("DLSSNR.DepthInverted", f.inverted);
    p->Set("DLSSNR.ScalingRatio", 1.0f); // its picture the colour's size
    ID3D12Resource* out = f.out ? f.out : g_nr_out;
    p->Set("DLSSNR.Color", f.color), p->Set("DLSSNR.Depth", f.depth), p->Set("DLSSNR.MVec", f.motion);
    p->Set("DLSSNR.Output", out), p->Set("DLSSNR.Backbuffer", out);
    for (const char* rect : {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect", "DLSSNR.MVecSubrect"}) {
        const std::string name = rect;
        p->Set((name + "BaseX").c_str(), 0), p->Set((name + "BaseY").c_str(), 0);
        p->Set((name + "Width").c_str(), f.rw), p->Set((name + "Height").c_str(), f.rh);
    }
    p->Set("DLSSNR.MVecScaleX", f.mv_x), p->Set("DLSSNR.MVecScaleY", f.mv_y);
}

namespace fxadd {
void drop();
}
namespace nrmix {
void drop();
}
namespace nrscale {
bool build();
void views(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* const mini[3],
           ID3D12Resource* nr, ID3D12Resource* full);
}

void nr_drop()
{
    fxadd::drop(); // its views are of DLSS 5's picture: made again with the next one
    nrmix::drop();
    if (g_nr || g_nr_more[0] || g_nr_more[1])
        idle();
    if (g_nr) {
        NVSDK_NGX_D3D12_ReleaseFeature(g_nr);
        g_nr = nullptr;
    }
    for (NVSDK_NGX_Handle*& more : g_nr_more) {
        if (more)
            NVSDK_NGX_D3D12_ReleaseFeature(more);
        more = nullptr;
    }
    for (ID3D12Resource*& mid : g_nr_mid)
        release(mid);
    g_nr_passes = 1;
    release(g_nr_out);
    for (ID3D12Resource*& mini : g_nr_small)
        release(mini);
    release(g_nr_full);
    g_nr_scale = 1.0f;
    if (g_sh)
        g_sh->nr_w = 0, g_sh->nr_h = 0;
}

// DLSS 5 for inputs of this size: its picture, and the feature, if NGX gives it
bool nr_make(const NrFrame& full, std::string& why)
{
    doing("making DLSS 5");
    nr_drop();
    // at a smaller size (nr_scale): the inputs it's handed made that size, and the picture at the colour's size its change
    // goes back onto; where they can't be made, the colour's size
    NrFrame f = full;
    if (full.scale > 0.0f && full.scale < 0.99f && nrscale::build()) {
        const float s = full.scale < 0.25f ? 0.25f : full.scale;
        f.w = static_cast<unsigned>(full.w * s + 0.999f), f.h = static_cast<unsigned>(full.h * s + 0.999f);
        f.w = f.w ? f.w : 1, f.h = f.h ? f.h : 1, f.rw = f.w, f.rh = f.h;
        const DXGI_FORMAT formats[3] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
        bool made = true;
        for (int i = 0; i < 3; ++i)
            made = made && (g_nr_small[i] = texture(formats[i], f.w, f.h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != nullptr;
        made = made && (g_nr_full = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, full.w, full.h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) != nullptr;
        if (made) {
            f.color = g_nr_small[0], f.depth = g_nr_small[1], f.motion = g_nr_small[2], g_nr_scale = s;
        } else {
            say("DLSS helper: DLSS 5's smaller size: its %ux%u textures couldn't be made; it runs at the colour's size", f.w, f.h);
            for (ID3D12Resource*& mini : g_nr_small)
                release(mini);
            release(g_nr_full);
            f = full;
        }
    }
    HRESULT hr = S_OK;
    g_nr_out = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, f.w, f.h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &hr);
    if (!g_nr_out) {
        char text[128];
        snprintf(text, sizeof text, "couldn't make DLSS 5's %ux%u picture (0x%08X; the device 0x%08X)", f.w, f.h,
                 static_cast<unsigned>(hr), static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
        why = text;
        nr_drop();
        return false;
    }
    nr_set(f);
    g_nr_params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u), g_nr_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    ID3D12GraphicsCommandList* l = begin();
    why_watch(true);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_CreateFeature(l, kNeuralRendering, g_nr_params, &g_nr);
    finish(true);
    char text[160];
    snprintf(text, sizeof text, "NVIDIA's NGX doesn't give DLSS 5 to this program (0x%08X)", r);
    const std::string said = why_said(text);
    if (NVSDK_NGX_FAILED(r) || !g_nr) {
        why = said;
        g_nr = nullptr;
        nr_drop();
        return false;
    }
    // passes 2 and 3 (nr_passes): their pictures first (each pass's colour is the one before's), then a feature each,
    // tapered. One that can't be made leaves the passes before it running alone
    const int passes = f.passes < 1 ? 1 : f.passes > kNrPasses ? kNrPasses : f.passes;
    g_nr_taper = f.taper < 0.0f ? 0.0f : f.taper > 1.0f ? 1.0f : f.taper;
    g_nr_passes = 1;
    for (int i = 1; i < passes; ++i)
        if (!(g_nr_mid[i - 1] = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, f.w, f.h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS)))
            break;
    for (int i = 1; i < passes && g_nr_mid[i - 1]; ++i) {
        NrFrame t = nr_tapered(f, g_nr_taper);
        t.color = g_nr_mid[i - 1], t.out = i + 1 < passes ? g_nr_mid[i] : g_nr_out;
        nr_set(t);
        l = begin();
        const NVSDK_NGX_Result more = NVSDK_NGX_D3D12_CreateFeature(l, kNeuralRendering, g_nr_params, &g_nr_more[i - 1]);
        finish(true);
        if (NVSDK_NGX_FAILED(more) || !g_nr_more[i - 1]) {
            say("DLSS helper: DLSS 5's pass %d couldn't be made (0x%08X); %d pass%s", i + 1, more, i, i > 1 ? "es" : "");
            g_nr_more[i - 1] = nullptr;
            break;
        }
        g_nr_passes = i + 1;
    }
    for (int i = g_nr_passes; i < kNrPasses; ++i)
        release(g_nr_mid[i - 1]); // the last pass made writes g_nr_out: no picture after it
    nr_set(f);
    if (g_nr_full)
        nrscale::views(full.color, full.depth, full.motion, g_nr_small, g_nr_out, g_nr_full);
    if (g_sh)
        g_sh->nr_w = f.w, g_sh->nr_h = f.h;
    say("DLSS helper: DLSS 5 at %ux%u (%.0f%% of %ux%u), model %d, %d pass%s (the later at %.2f of the local strengths), before DLSS",
        f.w, f.h, g_nr_scale * 100.0f, full.w, full.h, f.preset, g_nr_passes, g_nr_passes > 1 ? "es" : "", g_nr_taper);
    return true;
}

// --- the game's effects back onto DLSS 5's picture ([graphics.effects] dlss5_fx_out) ---------------------------
// The game hands DLSS 5 the scene as it was before its own effects (spells, flames, glows) were drawn into it, and the
// effects' own picture beside it (fx: what they changed, the finished colour less that); once DLSS 5 has made its
// picture, fx is added onto it here - a compute pass of this program's own, on its queue - and DLSS reads the sum. So
// DLSS 5 doesn't flatten them: they're drawn as the game drew them, made larger by DLSS with the rest. Built the first
// time DLSS 5 is made with the effects' picture (build, from take_inputs); its shader is compiled then (D3DCompile, from
// d3dcompiler_47.dll, loaded by name). Where it can't be, nr_fx_ok stays 0 and the game hands the colour over whole,
// effects and all.

namespace fxadd {

typedef HRESULT(WINAPI* PfnCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**,
                                    ID3DBlob**);

const char kSource[] =
    "Texture2D<float4> fx : register(t0);\n"
    "RWTexture2D<float4> pic : register(u0);\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint w, h;\n"
    "  fx.GetDimensions(w, h);\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  float4 c = pic[id.xy];\n"
    "  c.rgb = max(c.rgb + fx[id.xy].rgb, 0.0);\n"
    "  pic[id.xy] = c;\n"
    "}\n";

ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr; // fx's view, then the picture's
ID3D12Resource* g_for[2] = {};          // ... the textures they're of
bool g_tried = false;                   // the shader and pipeline: made once (g_pso NULL after: they couldn't be)

// The pipeline, once: false (and why) where it can't be made
bool build(std::string& why)
{
    if (g_tried) {
        if (!g_pso)
            why = "its compute pass couldn't be made (see the log)";
        return g_pso != nullptr;
    }
    g_tried = true;
    // the picture is read and written in place: a typed load of R16G16B16A16_FLOAT, which DirectX 12 cards mostly have
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    if (FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt)) || !opt.TypedUAVLoadAdditionalFormats ||
        FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
        why = "the card can't read a float picture in a compute pass";
        say("DLSS helper: DLSS 5's effects kept out: %s; they go through DLSS 5", why.c_str());
        return false;
    }
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    PfnCompile compile = dll ? reinterpret_cast<PfnCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    ID3DBlob *cs = nullptr, *err = nullptr, *sig = nullptr;
    if (!compile || FAILED(compile(kSource, sizeof kSource - 1, "fxadd", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        why = compile ? "its compute pass didn't compile" : "d3dcompiler_47.dll isn't here";
        say("DLSS helper: DLSS 5's effects kept out: %s%s%s; they go through DLSS 5", why.c_str(), err ? ": " : "",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        release(err);
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.DescriptorTable.NumDescriptorRanges = 2, p.DescriptorTable.pDescriptorRanges = ranges;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {1, &p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
              SUCCEEDED(g.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root))) &&
              SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)));
    if (ok) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_root;
        pd.CS.pShaderBytecode = cs->GetBufferPointer(), pd.CS.BytecodeLength = cs->GetBufferSize();
        ok = SUCCEEDED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)));
    }
    release(cs), release(err), release(sig);
    if (!ok) {
        release(g_pso), release(g_heap), release(g_root);
        why = "its compute pass couldn't be made";
        say("DLSS helper: DLSS 5's effects kept out: %s; they go through DLSS 5", why.c_str());
        return false;
    }
    say("DLSS helper: DLSS 5's effects kept out: its compute pass is made");
    return true;
}

// The views of fx and DLSS 5's picture, where they're new: after drop (nr_drop, drop_inputs, which wait for the queue to be
// idle first), so never while a list that reads them is on the GPU
void views(ID3D12Resource* fx, ID3D12Resource* pic)
{
    if (g_for[0] == fx && g_for[1] == pic)
        return;
    const UINT step = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE at = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, sv.Texture2D.MipLevels = 1;
    g.dev->CreateShaderResourceView(fx, &sv, at);
    at.ptr += step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->CreateUnorderedAccessView(pic, nullptr, &uv, at);
    g_for[0] = fx, g_for[1] = pic;
}

// fx added onto DLSS 5's picture (in unordered access, as DLSS 5 leaves it), recorded into l; fx is in COMMON, as the game
// leaves it, before and after. w x h: the part of them drawn
void run(ID3D12GraphicsCommandList* l, ID3D12Resource* fx, ID3D12Resource* pic, unsigned w, unsigned h)
{
    views(fx, pic);
    D3D12_RESOURCE_BARRIER b[2] = {};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, b[0].UAV.pResource = pic; // DLSS 5's writes done
    b[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b[1].Transition.pResource = fx;
    b[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON, b[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    l->ResourceBarrier(2, b);
    l->SetDescriptorHeaps(1, &g_heap);
    l->SetComputeRootSignature(g_root);
    l->SetPipelineState(g_pso);
    l->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, b[0].UAV.pResource = pic;
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    l->ResourceBarrier(2, b);
}

void drop()
{
    g_for[0] = g_for[1] = nullptr;
}

} // namespace fxadd

// --- DLSS 5's picture blended back with the colour it was handed (nr_mix, nr_colour, nr_brighten) --------------------
// A compute pass of this program's own, on DLSS 5's last picture before the game's effects go back on (fxadd) and DLSS
// reads it. In linear light (the colour is the game's, gamma-encoded, 2.2), per pixel: the colour scaled to DLSS 5's
// brightness, keeping its own hue and saturation, and the colour taken toward DLSS 5's picture, colour and all; nr_colour
// picks between the two, and nr_mix is how far either goes (its brightness ratio to that power, the other a straight
// blend: 0 the colour as it was handed, past 1 on beyond DLSS 5). The ratio is kept to 64 (a dark pixel the model lit
// would blow up past 1), and nr_brighten, where it's set, limits how much brighter than the colour a pixel may end
// (darker isn't limited): against a light the model turns into blocks of colour. Built with DLSS 5 (build, from
// take_inputs), as fxadd is; where it can't be, DLSS 5's picture goes as it is.

namespace nrmix {

const char kSource[] =
    "Texture2D<float4> src : register(t0);\n"
    "RWTexture2D<float4> pic : register(u0);\n"
    "cbuffer k : register(b0) { float mix_k; float colour_k; float brighten_k; uint w; uint h; };\n"
    "static const float3 Y = float3(0.2126, 0.7152, 0.0722);\n"
    "float3 lin(float3 c) { return pow(max(c, 0.0), 2.2); }\n"
    "float3 enc(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= w || id.y >= h) return;\n"
    "  float4 p = pic[id.xy];\n"
    "  float3 o = lin(src[id.xy].rgb), n = lin(p.rgb);\n"
    "  float yo = dot(o, Y), yn = dot(n, Y);\n"
    "  float r = min(pow(max(yn, 1e-6) / max(yo, 1e-6), mix_k), 64.0);\n"
    "  float3 c = lerp(o * r, max(lerp(o, n, mix_k), 0.0), colour_k);\n"
    "  if (brighten_k > 0.0) {\n"
    "    float most = brighten_k * max(yo, 0.001), yc = dot(c, Y);\n"
    "    if (yc > most) c *= most / yc;\n"
    "  }\n"
    "  p.rgb = enc(c);\n"
    "  pic[id.xy] = p;\n"
    "}\n";

struct Constants {
    float mix, colour, brighten;
    uint32_t w, h;
};

ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_pso = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr; // the colour's view, then the picture's
ID3D12Resource* g_for[2] = {};          // ... the textures they're of
bool g_tried = false;                   // made once (g_pso NULL after: it couldn't be)

// Whether a frame asks for the pass at all: at 1, 1 and no limit DLSS 5's picture goes as it is, untouched
bool wanted(const VanaDlssShared* sh)
{
    return sh->nr_mix != 1.0f || sh->nr_colour != 1.0f || sh->nr_brighten > 0.0f;
}

// The pipeline, once: false where it can't be made (said in the log)
bool build()
{
    if (g_tried)
        return g_pso != nullptr;
    g_tried = true;
    // the picture is read and written in place, as fxadd's
    D3D12_FEATURE_DATA_D3D12_OPTIONS opt = {};
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = {DXGI_FORMAT_R16G16B16A16_FLOAT};
    if (FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &opt, sizeof opt)) || !opt.TypedUAVLoadAdditionalFormats ||
        FAILED(g.dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)) ||
        !(fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD)) {
        say("DLSS helper: DLSS 5's blend: the card can't read a float picture in a compute pass; DLSS 5's picture as it is");
        return false;
    }
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<fxadd::PfnCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    ID3DBlob *cs = nullptr, *err = nullptr, *sig = nullptr;
    if (!compile || FAILED(compile(kSource, sizeof kSource - 1, "nrmix", nullptr, nullptr, "main", "cs_5_0", 0, 0, &cs, &err))) {
        say("DLSS helper: DLSS 5's blend: %s%s%s; DLSS 5's picture as it is",
            compile ? "its compute pass didn't compile" : "d3dcompiler_47.dll isn't here", err ? ": " : "",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        release(err);
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2, p[0].DescriptorTable.pDescriptorRanges = ranges;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 0, p[1].Constants.Num32BitValues = sizeof(Constants) / 4;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {2, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
              SUCCEEDED(g.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root))) &&
              SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)));
    if (ok) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_root;
        pd.CS.pShaderBytecode = cs->GetBufferPointer(), pd.CS.BytecodeLength = cs->GetBufferSize();
        ok = SUCCEEDED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pso)));
    }
    release(cs), release(err), release(sig);
    if (!ok) {
        release(g_pso), release(g_heap), release(g_root);
        say("DLSS helper: DLSS 5's blend: its compute pass couldn't be made; DLSS 5's picture as it is");
        return false;
    }
    say("DLSS helper: DLSS 5's blend: its compute pass is made");
    return true;
}

// The views of the colour and DLSS 5's picture, where they're new (after drop, as fxadd's)
void views(ID3D12Resource* src, ID3D12Resource* pic)
{
    if (g_for[0] == src && g_for[1] == pic)
        return;
    const UINT step = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE at = g_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, sv.Texture2D.MipLevels = 1;
    g.dev->CreateShaderResourceView(src, &sv, at);
    at.ptr += step;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
    uv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT, uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.dev->CreateUnorderedAccessView(pic, nullptr, &uv, at);
    g_for[0] = src, g_for[1] = pic;
}

// The blend, recorded into l: src (the colour DLSS 5 was handed) readable by shaders, as run_frame has it; pic (DLSS 5's
// picture) in unordered access, as DLSS 5 leaves it. w x h: the part of them drawn
void run(ID3D12GraphicsCommandList* l, ID3D12Resource* src, ID3D12Resource* pic, unsigned w, unsigned h, const VanaDlssShared* sh)
{
    views(src, pic);
    const Constants k = {sh->nr_mix, sh->nr_colour, sh->nr_brighten, w, h};
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, b.UAV.pResource = pic; // DLSS 5's writes done
    l->ResourceBarrier(1, &b);
    l->SetDescriptorHeaps(1, &g_heap);
    l->SetComputeRootSignature(g_root);
    l->SetPipelineState(g_pso);
    l->SetComputeRootDescriptorTable(0, g_heap->GetGPUDescriptorHandleForHeapStart());
    l->SetComputeRoot32BitConstants(1, sizeof k / 4, &k, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    l->ResourceBarrier(1, &b); // ... and this pass's, before what reads it next
}

void drop()
{
    g_for[0] = g_for[1] = nullptr;
}

} // namespace nrmix

// --- DLSS 5 at a smaller size (nr_scale, [graphics.effects] dlss5_scale) ----------------------------------------
// DLSS 5's cost goes with the pixels it's handed. Below 1, down makes the colour, depth and motion smaller (the colour
// filtered, the depth and motion the nearest texel's: never a blend of two surfaces) and DLSS 5 runs on those; up then
// lays what it changed back onto the colour at the colour's size: per pixel, in linear light, DLSS 5's picture over the
// colour it was handed, both at the smaller size, as a ratio a channel, taken from the four nearest small texels weighted
// by how near their depth is to the pixel's (so its edges stay the scene's, not the small size's), and the colour times
// that. DLSS 5's light and colour come through whole; detail it adds of its own (skin, structure) comes at the smaller
// size. Two compute passes of this program's own, built with DLSS 5 (nr_make); where they can't be, it runs at the
// colour's size.

namespace nrscale {

const char kDown[] =
    "Texture2D<float4> col : register(t0);\n"
    "Texture2D<float> dep : register(t1);\n"
    "Texture2D<float2> mot : register(t2);\n"
    "RWTexture2D<float4> scol : register(u0);\n"
    "RWTexture2D<float> sdep : register(u1);\n"
    "RWTexture2D<float2> smot : register(u2);\n"
    "SamplerState lin_s : register(s0);\n"
    "cbuffer k : register(b0) { uint rw; uint rh; uint sw; uint sh; };\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= sw || id.y >= sh) return;\n"
    "  uint w, h;\n"
    "  col.GetDimensions(w, h);\n"
    "  float2 c = (id.xy + 0.5) * float2(rw, rh) / float2(sw, sh);\n"
    "  scol[id.xy] = col.SampleLevel(lin_s, c / float2(w, h), 0);\n"
    "  uint2 p = min(uint2(c), uint2(rw - 1, rh - 1));\n"
    "  sdep[id.xy] = dep[p];\n"
    "  smot[id.xy] = mot[p];\n"
    "}\n";

const char kUp[] =
    "Texture2D<float4> col : register(t0);\n"
    "Texture2D<float> dep : register(t1);\n"
    "Texture2D<float4> scol : register(t2);\n"
    "Texture2D<float> sdep : register(t3);\n"
    "Texture2D<float4> snr : register(t4);\n"
    "RWTexture2D<float4> pic : register(u0);\n"
    "cbuffer k : register(b0) { uint rw; uint rh; uint sw; uint sh; };\n"
    "static const float E = 0.02;\n"
    "float3 lin(float3 c) { return pow(max(c, 0.0), 2.2); }\n"
    "float3 enc(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }\n"
    "[numthreads(8, 8, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "  if (id.x >= rw || id.y >= rh) return;\n"
    "  float4 o = col[id.xy];\n"
    "  float z = max(1.0 - dep[id.xy], 1e-7);\n"
    "  float2 q = (id.xy + 0.5) * float2(sw, sh) / float2(rw, rh) - 0.5;\n"
    "  int2 b = int2(floor(q));\n"
    "  float2 f = q - b;\n"
    "  float3 r = 0.0, nearest = 1.0;\n"
    "  float sum = 0.0, best = 1e30;\n"
    "  for (int j = 0; j < 4; ++j) {\n"
    "    int2 t = clamp(b + int2(j & 1, j >> 1), int2(0, 0), int2(sw - 1, sh - 1));\n"
    "    float bw = ((j & 1) ? f.x : 1.0 - f.x) * ((j >> 1) ? f.y : 1.0 - f.y);\n"
    "    float d = abs(log(max(1.0 - sdep[t], 1e-7) / z));\n"
    "    float3 rt = (lin(snr[t].rgb) + E) / (lin(scol[t].rgb) + E);\n"
    "    float w = bw * exp(-12.0 * d);\n"
    "    r += rt * w, sum += w;\n"
    "    if (d < best) best = d, nearest = rt;\n"
    "  }\n"
    "  r = min(sum > 1e-3 ? r / sum : nearest, 64.0);\n"
    "  pic[id.xy] = float4(enc((lin(o.rgb) + E) * r - E), o.a);\n"
    "}\n";

ID3D12RootSignature* g_root = nullptr;
ID3D12PipelineState* g_down = nullptr;
ID3D12PipelineState* g_up = nullptr;
ID3D12DescriptorHeap* g_heap = nullptr; // down's 5 inputs and 3 outputs, then up's
bool g_tried = false;                   // made once (g_up NULL after: they couldn't be)
constexpr UINT kSet = 8;

// The pipelines, once: false where they can't be made (said in the log)
bool build()
{
    if (g_tried)
        return g_up != nullptr;
    g_tried = true;
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dll ? reinterpret_cast<fxadd::PfnCompile>(GetProcAddress(dll, "D3DCompile")) : nullptr;
    ID3DBlob *down = nullptr, *up = nullptr, *err = nullptr, *sig = nullptr;
    if (!compile || FAILED(compile(kDown, sizeof kDown - 1, "nrdown", nullptr, nullptr, "main", "cs_5_0", 0, 0, &down, &err)) ||
        FAILED(compile(kUp, sizeof kUp - 1, "nrup", nullptr, nullptr, "main", "cs_5_0", 0, 0, &up, &err))) {
        say("DLSS helper: DLSS 5's smaller size: %s%s%s; it runs at the colour's size",
            compile ? "its compute passes didn't compile" : "d3dcompiler_47.dll isn't here", err ? ": " : "",
            err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        release(down), release(up), release(err);
        return false;
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0, 0, 5}};
    D3D12_ROOT_PARAMETER p[2] = {};
    p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p[0].DescriptorTable.NumDescriptorRanges = 2, p[0].DescriptorTable.pDescriptorRanges = ranges;
    p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    p[1].Constants.ShaderRegister = 0, p[1].Constants.Num32BitValues = 4;
    p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX, samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rd = {2, p, 1, &samp, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2 * kSet, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    bool ok = SUCCEEDED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) &&
              SUCCEEDED(g.dev->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&g_root))) &&
              SUCCEEDED(g.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_heap)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_root;
    if (ok) {
        pd.CS.pShaderBytecode = down->GetBufferPointer(), pd.CS.BytecodeLength = down->GetBufferSize();
        ok = SUCCEEDED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_down)));
    }
    if (ok) {
        pd.CS.pShaderBytecode = up->GetBufferPointer(), pd.CS.BytecodeLength = up->GetBufferSize();
        ok = SUCCEEDED(g.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_up)));
    }
    release(down), release(up), release(err), release(sig);
    if (!ok) {
        release(g_up), release(g_down), release(g_heap), release(g_root);
        say("DLSS helper: DLSS 5's smaller size: its compute passes couldn't be made; it runs at the colour's size");
        return false;
    }
    say("DLSS helper: DLSS 5's smaller size: its compute passes are made");
    return true;
}

// The views (each set's unused ones null): down reads the game's colour, depth and motion and writes the small ones; up
// reads the colour, its depth, the small colour and depth and DLSS 5's small picture, and writes the picture at the
// colour's size. Made with DLSS 5 (nr_make, after nr_drop has waited for the queue)
void views(ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* const mini[3],
           ID3D12Resource* nr, ID3D12Resource* full)
{
    const UINT step = g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = g_heap->GetCPUDescriptorHandleForHeapStart();
    const DXGI_FORMAT RGBA = DXGI_FORMAT_R16G16B16A16_FLOAT, R = DXGI_FORMAT_R32_FLOAT, RG = DXGI_FORMAT_R16G16_FLOAT;
    auto srv = [&](UINT at, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Format = f, v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, v.Texture2D.MipLevels = 1;
        g.dev->CreateShaderResourceView(r, &v, {base.ptr + at * step});
    };
    auto uav = [&](UINT at, ID3D12Resource* r, DXGI_FORMAT f) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC v = {};
        v.Format = f, v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g.dev->CreateUnorderedAccessView(r, nullptr, &v, {base.ptr + at * step});
    };
    srv(0, color, RGBA), srv(1, depth, R), srv(2, motion, RG), srv(3, nullptr, RGBA), srv(4, nullptr, RGBA);
    uav(5, mini[0], RGBA), uav(6, mini[1], R), uav(7, mini[2], RG);
    srv(kSet + 0, color, RGBA), srv(kSet + 1, depth, R), srv(kSet + 2, mini[0], RGBA), srv(kSet + 3, mini[1], R);
    srv(kSet + 4, nr, RGBA);
    uav(kSet + 5, full, RGBA), uav(kSet + 6, nullptr, RGBA), uav(kSet + 7, nullptr, RGBA);
}

void dispatch(ID3D12GraphicsCommandList* l, ID3D12PipelineState* pso, UINT set, unsigned rw, unsigned rh, unsigned sw,
              unsigned sh, unsigned w, unsigned h)
{
    const uint32_t k[4] = {rw, rh, sw, sh};
    l->SetDescriptorHeaps(1, &g_heap);
    l->SetComputeRootSignature(g_root);
    l->SetPipelineState(pso);
    D3D12_GPU_DESCRIPTOR_HANDLE at = g_heap->GetGPUDescriptorHandleForHeapStart();
    at.ptr += static_cast<UINT64>(set) * g.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    l->SetComputeRootDescriptorTable(0, at);
    l->SetComputeRoot32BitConstants(1, 4, k, 0);
    l->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
}

// The small colour, depth and motion, recorded into l: the game's readable by shaders (as run_frame has them), the small
// ones from unordered access to readable (`in`), for DLSS 5. rw x rh: the part of the colour drawn; sw x sh: of the small
void down(ID3D12GraphicsCommandList* l, ID3D12Resource* const mini[3], D3D12_RESOURCE_STATES in, unsigned rw, unsigned rh,
          unsigned sw, unsigned sh)
{
    dispatch(l, g_down, 0, rw, rh, sw, sh, sw, sh);
    D3D12_RESOURCE_BARRIER b[3] = {};
    for (int i = 0; i < 3; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b[i].Transition.pResource = mini[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b[i].Transition.StateAfter = in;
    }
    l->ResourceBarrier(3, b);
}

// ... and back to unordered access, once DLSS 5 and up are done with them
void done(ID3D12GraphicsCommandList* l, ID3D12Resource* const mini[3], D3D12_RESOURCE_STATES in)
{
    D3D12_RESOURCE_BARRIER b[3] = {};
    for (int i = 0; i < 3; ++i) {
        b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b[i].Transition.pResource = mini[i];
        b[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b[i].Transition.StateBefore = in, b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    l->ResourceBarrier(3, b);
}

// DLSS 5's small picture (nr, in unordered access as it leaves it, and after) laid onto the colour into full (unordered
// access, as nrmix and fxadd take it)
void up(ID3D12GraphicsCommandList* l, ID3D12Resource* nr, D3D12_RESOURCE_STATES in, unsigned rw, unsigned rh, unsigned sw,
        unsigned sh)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, b.Transition.pResource = nr;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, b.Transition.StateAfter = in;
    l->ResourceBarrier(1, &b);
    dispatch(l, g_up, kSet, rw, rh, sw, sh, rw, rh);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
}

} // namespace nrscale

// --- AMD FSR -------------------------------------------------------------------------------------------------
// AMD's FSR API (ffx_api.h), from amd_fidelityfx_loader_dx12.dll beside this program, which loads FSR's own DLL
// (amd_fidelityfx_upscaler_dx12.dll) from beside it: loaded by name when the game asks for FSR, nothing of AMD's
// linked. FSR takes DLSS's place on the same textures: the colour (the game's as it draws it, gamma-encoded: sRGB to
// FSR), the depth (0-1, with no far plane: 1 - near / w), the motion (render pixels, to where each pixel was the
// frame before) and the jitter (render pixels, as DLSS has it: the scene moved by 2 * jitter / size in clip space,
// y down, as AMD's own samples do it). The upscaler DLL holds several versions: by default the FSR API picks one
// (FSR 4 where AMD's DLL runs it, a Radeon RX 7000 or 9000; FSR 3.1 on any other DirectX 12 card), and
// [graphics] fsr.version asks for another (sr_version_want).

struct Ffx {
    HMODULE dll = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};
Ffx g_ffx;
ffxContext g_fsr = nullptr;
// What the context was made from: the FSR API may read what they point to until the context is destroyed
struct FsrMade {
    ffxCreateContextDescUpscale upscale = {};
    ffxCreateContextDescUpscaleVersion version = {};
    ffxCreateBackendDX12Desc backend = {};
    ffxOverrideVersion pick = {};
};
FsrMade g_fsr_made;
bool g_fsr_debug = false; // VANA_FSR_DEBUG=1: the FSR API checks what it's given, and its warnings go into this log

std::string utf8(const wchar_t* text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], n, nullptr, nullptr);
    out.resize(static_cast<size_t>(n - 1));
    return out;
}

// The FSR API's errors and warnings (its checker's too, with VANA_FSR_DEBUG), into this log: the first 300 of a run
void fsr_message(uint32_t type, const wchar_t* message)
{
    static int count = 0;
    if (message == nullptr || count >= 300)
        return;
    ++count;
    std::string text = utf8(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    say("FSR[%s]: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", text.c_str());
}

struct FsrVersion {
    uint64_t id;
    std::string name;
    int major; // the name's first number: 4 for "4.1.1"
};

// The FSR versions the upscaler DLL has for this card (the device's), in the FSR API's order
std::vector<FsrVersion> fsr_versions()
{
    std::vector<FsrVersion> out;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = g.dev;
    uint64_t count = 0;
    q.outputCount = &count;
    if (g_ffx.query(nullptr, &q.header) != FFX_API_RETURN_OK || count == 0)
        return out;
    std::vector<uint64_t> ids(static_cast<size_t>(count));
    std::vector<const char*> names(static_cast<size_t>(count), nullptr);
    q.versionIds = ids.data(), q.versionNames = names.data();
    if (g_ffx.query(nullptr, &q.header) != FFX_API_RETURN_OK)
        return out;
    for (size_t i = 0; i < ids.size() && i < count; ++i) {
        // copied at once: the DLL may keep a name where a later query writes over it
        FsrVersion v{ids[i], names[i] ? names[i] : "", 0};
        const size_t digit = v.name.find_first_of("0123456789");
        v.major = digit == std::string::npos ? 0 : atoi(v.name.c_str() + digit);
        out.push_back(v);
    }
    return out;
}

// A version's name as /vana shows it: "FSR 4.1.1"
std::string fsr_name(const char* name)
{
    if (name == nullptr || name[0] == '\0')
        return "FSR";
    return isdigit(static_cast<unsigned char>(name[0])) ? std::string("FSR ") + name : std::string(name);
}

// AMD's DLL `name` beside this program
bool amd_dll_here(const wchar_t* name)
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring file = path;
    file = file.substr(0, file.find_last_of(L"\\/") + 1) + name;
    return GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The FSR API (its loader, from beside this program): for FSR's upscaling, or AMD's frame generation with DLSS's
bool fsr_load(std::string& why)
{
    if (g_ffx.dll)
        return true;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    const std::wstring loader = dir + L"amd_fidelityfx_loader_dx12.dll";
    if (GetFileAttributesW(loader.c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = "AMD's FSR DLLs aren't beside VanaCore.Dlss.exe";
        return false;
    }
    // its own folder searched first for what it loads in turn (FSR's DLLs)
    g_ffx.dll = LoadLibraryExW(loader.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_ffx.dll) {
        char text[96];
        snprintf(text, sizeof text, "AMD's FSR loader didn't load (error %lu)", GetLastError());
        why = text;
        return false;
    }
    g_ffx.create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_ffx.dll, "ffxCreateContext"));
    g_ffx.destroy = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_ffx.dll, "ffxDestroyContext"));
    g_ffx.configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_ffx.dll, "ffxConfigure"));
    g_ffx.query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(g_ffx.dll, "ffxQuery"));
    g_ffx.dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(g_ffx.dll, "ffxDispatch"));
    if (!g_ffx.create || !g_ffx.destroy || !g_ffx.configure || !g_ffx.query || !g_ffx.dispatch) {
        why = "AMD's FSR loader is missing functions of the FSR API";
        FreeLibrary(g_ffx.dll);
        g_ffx = Ffx();
        return false;
    }
    g_fsr_debug = getenv("VANA_FSR_DEBUG") != nullptr;
    return true;
}

// The FSR API loaded from beside this program, and whether it has an upscaler for this card
bool fsr_start(std::string& why)
{
    if (!amd_dll_here(L"amd_fidelityfx_upscaler_dx12.dll")) {
        why = "AMD's FSR DLLs aren't beside VanaCore.Dlss.exe";
        return false;
    }
    if (!fsr_load(why))
        return false;
    const std::vector<FsrVersion> versions = fsr_versions();
    if (versions.empty()) {
        why = "AMD's FSR has no upscaler for this graphics card or its driver";
        return false;
    }
    std::string list;
    for (const FsrVersion& v : versions)
        list += (list.empty() ? "" : ", ") + v.name;
    say("%s: FSR's versions for this card: %s", g_who, list.c_str());
    return true;
}

void fsr_drop()
{
    if (g_fsr) {
        idle();
        g_ffx.destroy(&g_fsr, nullptr);
        g_fsr = nullptr;
    }
}

// FSR for the game's textures' sizes (generation changed), at the version asked for where the DLL has it
bool fsr_make(VanaDlssShared* sh, std::string& why)
{
    fsr_drop();
    FsrMade& m = g_fsr_made;
    m = FsrMade();
    m.upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // the colour as the game draws it, gamma-encoded (FSR otherwise takes it for linear light); its exposure worked out
    // by FSR, as AMD advises; the depth 0-1, with no far plane where the game's has none
    uint32_t flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    if (sh->depth_inverted)
        flags |= FFX_UPSCALE_ENABLE_DEPTH_INVERTED;
    if (sh->depth_infinite)
        flags |= FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (g_fsr_debug)
        flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    m.upscale.flags = flags;
    m.upscale.maxRenderSize.width = sh->in_w, m.upscale.maxRenderSize.height = sh->in_h;
    m.upscale.maxUpscaleSize.width = sh->out_w, m.upscale.maxUpscaleSize.height = sh->out_h;
    m.upscale.fpMessage = &fsr_message;
    m.version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    m.version.version = FFX_UPSCALER_VERSION;
    m.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    m.backend.device = g.dev;
    m.upscale.header.pNext = &m.version.header;
    m.version.header.pNext = &m.backend.header;
    // a version asked for ([graphics] fsr.version): the first of that major version the DLL has for this card
    if (sh->sr_version_want > 0) {
        for (const FsrVersion& v : fsr_versions())
            if (v.major == sh->sr_version_want) {
                m.pick.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
                m.pick.versionId = v.id;
                m.backend.header.pNext = &m.pick.header;
                break;
            }
        if (!m.backend.header.pNext)
            say("%s: FSR %d isn't there for this card; FSR's own choice", g_who, sh->sr_version_want);
    }
    const ffxReturnCode_t r = g_ffx.create(&g_fsr, &m.upscale.header, nullptr);
    if (r != FFX_API_RETURN_OK || !g_fsr) {
        char text[128];
        snprintf(text, sizeof text, "FSR couldn't start for %ux%u to %ux%u (FSR API error %u)", sh->in_w, sh->in_h, sh->out_w,
                 sh->out_h, static_cast<unsigned>(r));
        why = text;
        g_fsr = nullptr;
        return false;
    }
    if (g_fsr_debug) {
        ffxConfigureDescGlobalDebug1 debug = {};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        debug.fpMessage = &fsr_message;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        g_ffx.configure(&g_fsr, &debug.header);
    }
    // the version it runs, for /vana
    ffxQueryGetProviderVersion version = {};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const std::string name = fsr_name(g_ffx.query(&g_fsr, &version.header) == FFX_API_RETURN_OK ? version.versionName : nullptr);
    snprintf(sh->sr_version, sizeof sh->sr_version, "%s", name.c_str());
    say("%s: %s from up to %ux%u to %ux%u%s (generation %u)", g_who, name.c_str(), sh->in_w, sh->in_h, sh->out_w, sh->out_h,
        sh->depth_infinite ? ", depth without a far plane" : "", sh->generation);
    return true;
}

// --- serving the game ---------------------------------------------------------------------------------------

struct Inputs {
    uint32_t generation = 0;
    ID3D12Resource* color = nullptr;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* output = nullptr;
    ID3D12Fence* fence = nullptr;
    ID3D12Resource* fx = nullptr;  // the game's effects' own picture, with DLSS 5 (fxadd): none without it
    ID3D12Resource* reactive = nullptr; // the game's mask of what isn't the static world (R8; dlss_ipc.h): FSR's reactive mask,
                                        // DLSS's bias-current-colour mask; none if the game gave none
    ID3D12Resource* tc = nullptr;       // ... and of its effects alone: FSR's transparency and composition mask
    uint32_t out_w = 0, out_h = 0; // the output's size (FSR upscales to it)
};
Inputs g_in;
uint64_t g_ran = 0; // the last frame run (the game's event also comes for new textures alone)

void drop_inputs()
{
    nr_drop();
    fsr_drop();
    if (g_feature) {
        idle();
        NVSDK_NGX_D3D12_ReleaseFeature(g_feature);
        g_feature = nullptr;
    }
    release(g_in.color), release(g_in.depth), release(g_in.motion), release(g_in.output), release(g_in.fence);
    release(g_in.fx), release(g_in.reactive), release(g_in.tc);
    fxadd::drop();
}

// DLSS 5 given up, for the game to see (and why)
void nr_gone(VanaDlssShared* sh, const std::string& why)
{
    g_nr_available = false, g_nr_why = why;
    snprintf(sh->nr_message, sizeof sh->nr_message, "%s", why.c_str());
    sh->nr_available = 0, sh->nr_fx_ok = 0;
    say("DLSS helper: DLSS 5: %s; DLSS alone", why.c_str());
}

// One of the game's handles (duplicated into this process): opened, then closed (the object stays while it's open)
template <typename T>
bool open(uint64_t handle, T*& out)
{
    if (!handle)
        return false;
    const bool ok = SUCCEEDED(g.dev->OpenSharedHandle(reinterpret_cast<HANDLE>(handle), IID_PPV_ARGS(&out)));
    CloseHandle(reinterpret_cast<HANDLE>(handle));
    return ok;
}

// The game's fence: opened from its handle, or with sh->cpu_sync (none is shared) a fence of this side's own, only for
// the order of its queue's work (the game waits for its queue and tells this side by an event, and is told by one)
bool game_fence(const VanaDlssShared* sh, uint64_t handle, ID3D12Fence*& out)
{
    if (!sh->cpu_sync)
        return open(handle, out);
    return SUCCEEDED(g.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&out)));
}

// The game's textures and fence (generation changed), and DLSS's feature for their sizes
bool take_inputs(VanaDlssShared* sh, std::string& why)
{
    char what[96];
    snprintf(what, sizeof what, "making %s for %ux%u to %ux%u", g_fsr_mode ? "FSR" : "DLSS", sh->in_w, sh->in_h, sh->out_w, sh->out_h);
    doing(what);
    drop_inputs();
    g_in.generation = sh->generation;
    sh->nr_fx_ok = 0;
    // the effects' own picture (DLSS 5's, fxadd) first: opened, or its handle closed, whatever else happens
    const bool fx = sh->fx && open(sh->fx, g_in.fx);
    if (sh->reactive)
        open(sh->reactive, g_in.reactive); // (without them, the upscaler has no masks: as before there were any)
    if (sh->tc)
        open(sh->tc, g_in.tc);
    if (!open(sh->color, g_in.color) || !open(sh->depth, g_in.depth) || !open(sh->motion, g_in.motion) ||
        !open(sh->output, g_in.output) || !game_fence(sh, sh->fence, g_in.fence)) {
        why = "couldn't open the game's textures";
        return false;
    }
    g_in.out_w = sh->out_w, g_in.out_h = sh->out_h;
    if (g_fsr_mode)
        return fsr_make(sh, why);
    NVSDK_NGX_DLSS_Create_Params p = {};
    p.Feature.InWidth = sh->in_w, p.Feature.InHeight = sh->in_h;
    p.Feature.InTargetWidth = sh->out_w, p.Feature.InTargetHeight = sh->out_h;
    p.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(sh->quality);
    p.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                             (sh->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
    // The model DLSS runs (its render preset), where one is asked for: for every quality it might be made at
    if (sh->sr_preset > 0) {
        for (const char* name : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                                 NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance})
            g_params->Set(name, static_cast<unsigned int>(sh->sr_preset));
    }
    ID3D12GraphicsCommandList* l = begin();
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSS_EXT(l, 1, 1, &g_feature, g_params, &p);
    finish(true);
    if (NVSDK_NGX_FAILED(r) || !g_feature) {
        char text[96];
        snprintf(text, sizeof text, "DLSS couldn't start for %ux%u to %ux%u (0x%08X)", sh->in_w, sh->in_h, sh->out_w,
                 sh->out_h, r);
        why = text;
        g_feature = nullptr;
        return false;
    }
    say("DLSS helper: DLSS from up to %ux%u to %ux%u, quality %d, model %d (generation %u)", sh->in_w, sh->in_h, sh->out_w, sh->out_h,
        sh->quality, sh->sr_preset, sh->generation);
    // DLSS 5, when the game asks (its colour then the format it takes). It alone gives up where NGX says no: the
    // game sees nr_available go and hands the colour over as before, DLSS goes on
    if (sh->nr && g_nr_available && sh->color_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        NrFrame f;
        f.w = sh->in_w, f.h = sh->in_h, f.rw = sh->in_w, f.rh = sh->in_h;
        f.color = g_in.color, f.depth = g_in.depth, f.motion = g_in.motion;
        f.inverted = sh->depth_inverted, f.style = sh->nr_style, f.intensity = sh->nr_intensity;
        f.tone = sh->nr_tone, f.structure = sh->nr_structure, f.skin = sh->nr_skin, f.auto_mask = sh->nr_auto_mask;
        f.preset = sh->nr_preset;
        f.passes = sh->nr_passes, f.taper = sh->nr_taper, f.scale = sh->nr_scale;
        std::string nr_why;
        if (!nr_make(f, nr_why))
            nr_gone(sh, nr_why);
        // ... and the game's effects added back onto its picture, where the game keeps them out of it (dlss5_fx_out)
        std::string fx_why;
        if (g_nr && fx && fxadd::build(fx_why))
            sh->nr_fx_ok = 1;
        // ... and its blend with the colour (nr_mix: the pass made now, so turning it on later costs no frame)
        if (g_nr)
            nrmix::build();
    }
    return true;
}

void barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from, b.Transition.StateAfter = to;
    l->ResourceBarrier(1, &b);
}

// A frame through FSR, recorded into l: its inputs in the shader-readable state (`in` below), its output in unordered
// access. FSR's own sharpening (RCAS) where the game asks for it. The game's masks (dlss_ipc.h reactive, tc) where it gives
// them: AMD's guidance is to mark particles and alpha-blended things (its shading-change detection is a fallback), and
// VanaCore can't give FSR their motion (nor a character's, without guessing), so it marks them and what moves instead
ffxReturnCode_t fsr_run(ID3D12GraphicsCommandList* l, const VanaDlssShared* sh)
{
    const uint32_t read = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = l;
    d.color = ffxApiGetResourceDX12(g_in.color, read);
    d.depth = ffxApiGetResourceDX12(g_in.depth, read);
    d.motionVectors = ffxApiGetResourceDX12(g_in.motion, read);
    d.exposure = ffxApiGetResourceDX12(nullptr, read);
    // the game's masks: what isn't the static world (its effects, and what moves on its own), and its effects alone
    d.reactive = ffxApiGetResourceDX12(g_in.reactive, read);
    d.transparencyAndComposition = ffxApiGetResourceDX12(g_in.tc, read);
    d.output = ffxApiGetResourceDX12(g_in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = sh->jitter_x, d.jitterOffset.y = sh->jitter_y;
    d.motionVectorScale.x = sh->motion_scale_x, d.motionVectorScale.y = sh->motion_scale_y;
    d.renderSize.width = sh->render_w ? sh->render_w : sh->in_w;
    d.renderSize.height = sh->render_h ? sh->render_h : sh->in_h;
    d.upscaleSize.width = g_in.out_w, d.upscaleSize.height = g_in.out_h;
    const float sharp = sh->sharpness;
    d.enableSharpening = sharp > 0.0f;
    d.sharpness = sharp < 0.0f ? 0.0f : sharp > 1.0f ? 1.0f : sharp;
    d.frameTimeDelta = sh->sr_frame_ms > 0.0f ? sh->sr_frame_ms : 16.7f;
    d.preExposure = 1.0f;
    d.reset = sh->reset != 0;
    d.cameraNear = sh->sr_near > 0.0f ? sh->sr_near : 0.1f;
    d.cameraFar = sh->sr_far > d.cameraNear ? sh->sr_far : d.cameraNear * 1.0e6f;
    d.cameraFovAngleVertical = sh->sr_fov > 0.0f ? sh->sr_fov : 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    return g_ffx.dispatch(&g_fsr, &d.header);
}

// A frame: after the game's fence reaches `frame`, DLSS (or FSR) into the output; then the fence to frame + 1
void run_frame(VanaDlssShared* sh)
{
    const int64_t woke = qpc(), posted = sh->posted_qpc;
    g_list_wait = 0;
    const uint64_t frame = sh->frame;
    // (with cpu_sync the game waited for its queue before it set the event: the inputs are through)
    if (!sh->cpu_sync)
        g.queue->Wait(g_in.fence, frame);
    ID3D12GraphicsCommandList* l = begin();
    const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier(l, g_in.color, D3D12_RESOURCE_STATE_COMMON, in);
    barrier(l, g_in.depth, D3D12_RESOURCE_STATE_COMMON, in);
    barrier(l, g_in.motion, D3D12_RESOURCE_STATE_COMMON, in);
    const bool mask = g_in.reactive != nullptr, tcm = g_fsr_mode && g_in.tc;
    if (mask)
        barrier(l, g_in.reactive, D3D12_RESOURCE_STATE_COMMON, in);
    if (tcm)
        barrier(l, g_in.tc, D3D12_RESOURCE_STATE_COMMON, in);
    barrier(l, g_in.output, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // DLSS 5 first, when the game asks: the colour through it, and DLSS on its picture
    ID3D12Resource* color = g_in.color;
    NVSDK_NGX_Result nr = NVSDK_NGX_Result_Success;
    std::string nr_said; // NGX's reason, if it gave one with a failure
    int64_t evaluate = 0;
    int mids = 0; // the passes' pictures made readable for the pass after (g_nr_mid): back to unordered access below
    const bool nr_ran = g_nr && sh->nr;
    ts_mark(0);
    if (g_nr && sh->nr) {
        NrFrame f;
        f.w = sh->in_w, f.h = sh->in_h;
        f.rw = sh->render_w ? sh->render_w : sh->in_w, f.rh = sh->render_h ? sh->render_h : sh->in_h;
        f.color = g_in.color, f.depth = g_in.depth, f.motion = g_in.motion;
        f.mv_x = sh->motion_scale_x, f.mv_y = sh->motion_scale_y;
        f.inverted = sh->depth_inverted, f.reset = sh->reset;
        f.style = sh->nr_style, f.intensity = sh->nr_intensity;
        f.tone = sh->nr_tone, f.structure = sh->nr_structure, f.skin = sh->nr_skin, f.auto_mask = sh->nr_auto_mask;
        f.preset = sh->nr_preset;
        // at a smaller size (nr_scale): the colour, depth and motion made that size, DLSS 5 on those
        NrFrame at = f;
        unsigned sw = 0, shh = 0;
        if (g_nr_full) {
            D3D12_RESOURCE_DESC d = g_nr_small[0]->GetDesc();
            sw = static_cast<unsigned>(f.rw * g_nr_scale + 0.5f), shh = static_cast<unsigned>(f.rh * g_nr_scale + 0.5f);
            sw = sw < 1 ? 1 : sw > d.Width ? static_cast<unsigned>(d.Width) : sw;
            shh = shh < 1 ? 1 : shh > d.Height ? d.Height : shh;
            nrscale::down(l, g_nr_small, in, f.rw, f.rh, sw, shh);
            at.w = static_cast<unsigned>(d.Width), at.h = d.Height, at.rw = sw, at.rh = shh;
            at.color = g_nr_small[0], at.depth = g_nr_small[1], at.motion = g_nr_small[2];
            at.mv_x = f.mv_x * sw / f.rw, at.mv_y = f.mv_y * shh / f.rh;
        }
        const int64_t from = qpc();
        why_watch(true);
        // each pass on the one before's picture (nr_passes; the later ones tapered), the last into g_nr_out
        for (int i = 0; i < g_nr_passes && NVSDK_NGX_SUCCEED(nr); ++i) {
            NrFrame t = i ? nr_tapered(at, g_nr_taper) : at;
            t.color = i ? g_nr_mid[i - 1] : at.color;
            t.out = i + 1 < g_nr_passes ? g_nr_mid[i] : g_nr_out;
            char what[48];
            snprintf(what, sizeof what, "running DLSS 5, pass %d of %d", i + 1, g_nr_passes);
            doing(what, frame);
            nr_set(t);
            nr = NVSDK_NGX_D3D12_EvaluateFeature(l, i ? g_nr_more[i - 1] : g_nr, g_nr_params, nullptr);
            if (NVSDK_NGX_SUCCEED(nr) && i + 1 < g_nr_passes)
                barrier(l, t.out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, in), ++mids;
        }
        if (NVSDK_NGX_FAILED(nr)) {
            char text[96];
            snprintf(text, sizeof text, "DLSS 5 failed on a frame (0x%08X)", nr);
            nr_said = why_said(text);
        } else {
            why_watch(false);
        }
        evaluate += qpc() - from;
        ID3D12Resource* pic = g_nr_out;
        if (g_nr_full && NVSDK_NGX_SUCCEED(nr)) {
            // its change laid back onto the colour at the colour's size
            doing("laying DLSS 5's picture onto the colour", frame);
            nrscale::up(l, g_nr_out, in, f.rw, f.rh, sw, shh);
            pic = g_nr_full;
        }
        if (g_nr_full)
            nrscale::done(l, g_nr_small, in);
        if (NVSDK_NGX_SUCCEED(nr)) {
            doing("blending DLSS 5's picture", frame);
            // blended back with the colour it was handed (nr_mix, nr_colour, nr_brighten), where that's asked for
            if (nrmix::wanted(sh) && nrmix::g_pso)
                nrmix::run(l, g_in.color, pic, f.rw, f.rh, sh);
            // the game's effects, kept out of DLSS 5 (dlss5_fx_out), back onto its picture before DLSS reads it
            if (sh->nr_fx && sh->nr_fx_ok && g_in.fx)
                fxadd::run(l, g_in.fx, pic, f.rw, f.rh);
            barrier(l, pic, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, in);
            color = pic;
        }
    }
    ts_mark(1);
    int64_t from = qpc();
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    ffxReturnCode_t fsr = FFX_API_RETURN_OK;
    doing(g_fsr_mode ? "running FSR" : "running DLSS", frame);
    if (sh->crash_test != VANA_CRASH_TEST_NONE)
        crash_test(sh->crash_test);
    if (g_fsr_mode) {
        fsr = fsr_run(l, sh);
    } else {
        NVSDK_NGX_D3D12_DLSS_Eval_Params e = {};
        e.Feature.pInColor = color, e.Feature.pInOutput = g_in.output;
        e.pInDepth = g_in.depth, e.pInMotionVectors = g_in.motion;
        e.InJitterOffsetX = sh->jitter_x, e.InJitterOffsetY = sh->jitter_y;
        e.InRenderSubrectDimensions.Width = sh->render_w, e.InRenderSubrectDimensions.Height = sh->render_h;
        e.InReset = sh->reset;
        e.InMVScaleX = sh->motion_scale_x, e.InMVScaleY = sh->motion_scale_y;
        // the game's mask of what isn't the static world (its effects, what moves on its own): where DLSS is to lean on this
        // frame's colour rather than its history, as FSR's reactive mask has it
        e.pInBiasCurrentColorMask = mask ? g_in.reactive : nullptr;
        r = NGX_D3D12_EVALUATE_DLSS_EXT(l, g_feature, g_params, &e);
    }
    evaluate += qpc() - from;
    ts_mark(2);
    ts_resolve(3, 1, nr_ran);
    if (color == g_nr_out || (g_nr_full && color == g_nr_full))
        barrier(l, color, in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int i = 0; i < mids; ++i)
        barrier(l, g_nr_mid[i], in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(l, g_in.color, in, D3D12_RESOURCE_STATE_COMMON);
    barrier(l, g_in.depth, in, D3D12_RESOURCE_STATE_COMMON);
    barrier(l, g_in.motion, in, D3D12_RESOURCE_STATE_COMMON);
    if (mask)
        barrier(l, g_in.reactive, in, D3D12_RESOURCE_STATE_COMMON);
    if (tcm)
        barrier(l, g_in.tc, in, D3D12_RESOURCE_STATE_COMMON);
    barrier(l, g_in.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    from = qpc();
    doing(g_fsr_mode ? "handing FSR's work to the graphics card" : "handing DLSS's work to the graphics card", frame);
    finish(false);
    g.queue->Signal(g_in.fence, frame + 1);
    // no fence shared with the game (cpu_sync): its queue waits on a signal from its thread, sent when this event is set,
    // once this side's work is through
    if (sh->cpu_sync)
        idle();
    sh->done = frame;
    sh->gpu_nr_ms = g_gpu_nr_ms, sh->gpu_sr_ms = g_gpu_sr_ms, sh->gpu_fg_ms = g_gpu_fg_ms, sh->gpu_timed = g_gpu_timed ? 1 : 0;
    if (sh->cpu_sync)
        SetEvent(g_game_done);
    note_frame(g_fsr_mode ? "FSR" : "DLSS", frame, posted, woke, evaluate, qpc() - from);
    // VANA_DLSS_DEBUG=1: the inputs' and the output's middle texels, the first few frames
    static int debugged = 0;
    if (getenv("VANA_DLSS_DEBUG") && debugged < 3 && frame > 20) {
        ++debugged;
        ID3D12Resource* srcs[4] = {g_in.color, g_in.depth, g_in.motion, g_in.output};
        const char* names[4] = {"colour", "depth", "motion", "output"};
        for (int i = 0; i < 4; ++i) {
            D3D12_RESOURCE_DESC d = srcs[i]->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
            UINT64 total = 0;
            g.dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC bd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, total, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                                      D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            ID3D12Resource* rb = nullptr;
            if (FAILED(g.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&rb))))
                continue;
            g.queue->Wait(g_in.fence, frame + 1);
            ID3D12GraphicsCommandList* l2 = begin();
            barrier(l2, srcs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
            to.pResource = rb, to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, to.PlacedFootprint = fp;
            from.pResource = srcs[i], from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            l2->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier(l2, srcs[i], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            finish(true);
            uint8_t* p = nullptr;
            rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
            const UINT64 row = fp.Footprint.RowPitch, w = fp.Footprint.Width, h = fp.Footprint.Height;
            const UINT64 bpp = row / (w ? w : 1) >= 8 ? 8 : 4;
            const uint8_t* at_l = p + (h / 2) * row + (w / 4) * (d.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : bpp);
            const uint8_t* at_r = p + (h / 2) * row + (3 * w / 4) * (d.Format == DXGI_FORMAT_R16G16_FLOAT ? 4 : bpp);
            say("DLSS helper: %s (format %d, %llux%llu): left %08x, right %08x", names[i], d.Format, w, h,
                *reinterpret_cast<const uint32_t*>(at_l), *reinterpret_cast<const uint32_t*>(at_r));
            rb->Unmap(0, nullptr);
            rb->Release();
        }
    }
    static bool said = false;
    if (NVSDK_NGX_FAILED(r) && !said) {
        said = true;
        say("DLSS helper: DLSS failed on a frame (0x%08X)", r);
    }
    static bool fsr_said = false;
    if (fsr != FFX_API_RETURN_OK && !fsr_said) {
        fsr_said = true;
        say("FSR helper: FSR failed on a frame (FSR API error %u)", static_cast<unsigned>(fsr));
    }
    if (NVSDK_NGX_FAILED(nr)) {
        nr_drop();
        nr_gone(sh, nr_said);
    }
}

// --- frame generation ----------------------------------------------------------------------------------------

struct FgInputs {
    uint32_t generation = 0;
    ID3D12Resource* backbuffer = nullptr;
    ID3D12Resource* hudless = nullptr;
    ID3D12Resource* output[3] = {};
    ID3D12Fence* fence = nullptr;
    uint32_t count = 0;
};
FgInputs g_fg_in;
NVSDK_NGX_Handle* g_fg = nullptr;
uint64_t g_fg_ran = 0; // the last frame run (the game's event also comes for new textures alone)

// --- AMD's frame generation ---------------------------------------------------------------------------------
// FSR frame generation (amd_fidelityfx_framegeneration_dx12.dll, through the FSR API's loader), with either upscaler,
// without AMD's swapchain (the game's is in its own process): each frame configured, then its preparation dispatched on
// the scene's depth and motion (DLSS's or FSR's inputs, still this frame's) and the camera, then the frame made between
// the frame before and this one from the frame (presentColor) and the scene without the game's interface (HUDLessColor),
// into fg_output[0] - in that order, as FSR FG 4 requires. One frame between two (2x). AMD's frame id is the game's frame
// number, so a frame it skipped (a menu without a scene) starts it afresh.

ffxContext g_ffg = nullptr;
struct FfgMade {
    ffxCreateContextDescFrameGeneration fg = {};
    ffxCreateContextDescFrameGenerationVersion version = {};
    ffxCreateContextDescFrameGenerationHudless hudless = {};
    ffxCreateBackendDX12Desc backend = {};
};
FfgMade g_ffg_made;
bool g_ffg_available = false; // AMD's DLLs here (made when the game asks for it; a failure then gives it up)
std::string g_ffg_why;

void ffg_drop()
{
    if (g_ffg) {
        idle();
        g_ffx.destroy(&g_ffg, nullptr);
        g_ffg = nullptr;
    }
}

// AMD's frame generation for the game's frame (its size and format) and the scene's depth and motion (in_w x in_h)
bool ffg_make(VanaDlssShared* sh, std::string& why)
{
    ffg_drop();
    if (!fsr_load(why))
        return false;
    FfgMade& m = g_ffg_made;
    m = FfgMade();
    const uint32_t format = ffxApiGetSurfaceFormatDX12(static_cast<DXGI_FORMAT>(sh->fg_format));
    m.fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    uint32_t flags = 0;
    if (sh->depth_inverted)
        flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED;
    if (sh->depth_infinite)
        flags |= FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;
    if (g_fsr_debug)
        flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    m.fg.flags = flags;
    m.fg.displaySize.width = sh->fg_w, m.fg.displaySize.height = sh->fg_h;
    m.fg.maxRenderSize.width = sh->in_w, m.fg.maxRenderSize.height = sh->in_h;
    m.fg.backBufferFormat = format;
    m.version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    m.version.version = FFX_FRAMEGENERATION_VERSION;
    m.hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
    m.hudless.hudlessBackBufferFormat = format;
    m.backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    m.backend.device = g.dev;
    m.fg.header.pNext = &m.version.header;
    m.version.header.pNext = &m.hudless.header;
    m.hudless.header.pNext = &m.backend.header;
    const ffxReturnCode_t r = g_ffx.create(&g_ffg, &m.fg.header, nullptr);
    if (r != FFX_API_RETURN_OK || !g_ffg) {
        char text[128];
        snprintf(text, sizeof text, "AMD's frame generation couldn't start for %ux%u (FSR API error %u)", sh->fg_w, sh->fg_h,
                 static_cast<unsigned>(r));
        why = text;
        g_ffg = nullptr;
        return false;
    }
    if (g_fsr_debug) {
        ffxConfigureDescGlobalDebug1 debug = {};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        debug.fpMessage = &fsr_message;
        debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
        g_ffx.configure(&g_ffg, &debug.header);
    }
    ffxQueryGetProviderVersion version = {};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const bool named = g_ffx.query(&g_ffg, &version.header) == FFX_API_RETURN_OK && version.versionName && version.versionName[0];
    const std::string name = named ? (isdigit(static_cast<unsigned char>(version.versionName[0])) ? std::string("FSR FG ") + version.versionName
                                                                                                    : std::string(version.versionName))
                                   : std::string("FSR FG");
    snprintf(sh->fg_version, sizeof sh->fg_version, "%s", name.c_str());
    say("%s: %s at %ux%u (the scene's depth and motion %ux%u), one frame between two (generation %u)", g_who, name.c_str(),
        sh->fg_w, sh->fg_h, sh->in_w, sh->in_h, sh->fg_generation);
    return true;
}

// A frame made by AMD's frame generation, recorded into l: the frame, hudless, depth and motion in the shader-readable
// state, the output in unordered access
ffxReturnCode_t ffg_run(ID3D12GraphicsCommandList* l, const VanaDlssShared* sh, ID3D12Resource* depth, ID3D12Resource* motion)
{
    const uint32_t read = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
    FfxApiRect2D whole = {};
    whole.width = static_cast<int32_t>(sh->fg_w), whole.height = static_cast<int32_t>(sh->fg_h);
    const uint64_t id = sh->fg_frame_id;
    ffxConfigureDescFrameGeneration c = {};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.frameGenerationEnabled = true;
    c.allowAsyncWorkloads = false;
    c.HUDLessColor = g_fg_in.hudless ? ffxApiGetResourceDX12(g_fg_in.hudless, read) : FfxApiResource{};
    c.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY; // no swapchain of AMD's: the game shows the frames
    c.onlyPresentGenerated = false;
    c.generationRect = whole;
    c.frameID = id;
    ffxReturnCode_t r = g_ffx.configure(&g_ffg, &c.header);
    if (r != FFX_API_RETURN_OK)
        return r;
    ffxDispatchDescFrameGenerationPrepareV2 p = {};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = id;
    p.flags = c.flags;
    p.commandList = l;
    p.renderSize.width = sh->fg_render_w ? sh->fg_render_w : sh->in_w;
    p.renderSize.height = sh->fg_render_h ? sh->fg_render_h : sh->in_h;
    p.jitterOffset.x = sh->fg_jitter_x, p.jitterOffset.y = sh->fg_jitter_y;
    p.motionVectorScale.x = sh->fg_motion_scale[0], p.motionVectorScale.y = sh->fg_motion_scale[1];
    p.frameTimeDelta = sh->fg_frame_ms > 0.0f ? sh->fg_frame_ms : 16.7f;
    p.reset = sh->fg_reset != 0;
    p.cameraNear = sh->cam_near > 0.0f ? sh->cam_near : 0.1f;
    p.cameraFar = sh->cam_far > p.cameraNear ? sh->cam_far : p.cameraNear * 1.0e6f;
    p.cameraFovAngleVertical = sh->cam_fov > 0.0f ? sh->cam_fov : 1.0f;
    p.viewSpaceToMetersFactor = 1.0f;
    p.depth = ffxApiGetResourceDX12(depth, read);
    p.motionVectors = ffxApiGetResourceDX12(motion, read);
    memcpy(p.cameraPosition, sh->cam_pos, 12), memcpy(p.cameraUp, sh->cam_up, 12);
    memcpy(p.cameraRight, sh->cam_right, 12), memcpy(p.cameraForward, sh->cam_fwd, 12);
    r = g_ffx.dispatch(&g_ffg, &p.header);
    if (r != FFX_API_RETURN_OK)
        return r;
    ffxDispatchDescFrameGeneration d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    d.commandList = l;
    d.presentColor = ffxApiGetResourceDX12(g_fg_in.backbuffer, read);
    d.outputs[0] = ffxApiGetResourceDX12(g_fg_in.output[0], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.numGeneratedFrames = 1;
    d.reset = sh->fg_reset != 0;
    d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    d.minMaxLuminance[0] = 0.0f, d.minMaxLuminance[1] = 1.0f; // (read for HDR frames only)
    d.generationRect = whole;
    d.frameID = id;
    return g_ffx.dispatch(&g_ffg, &d.header);
}

void drop_fg()
{
    ffg_drop();
    if (g_fg) {
        idle();
        NVSDK_NGX_D3D12_ReleaseFeature(g_fg);
        g_fg = nullptr;
    }
    release(g_fg_in.backbuffer), release(g_fg_in.hudless), release(g_fg_in.fence);
    for (auto& o : g_fg_in.output)
        release(o);
}

// The game's frame textures and fence (fg_generation changed), and frame generation (NVIDIA's or AMD's: fg_kind) for
// their sizes
bool take_fg(VanaDlssShared* sh, std::string& why)
{
    drop_fg();
    const bool amd = sh->fg_kind == VANA_FG_FSR;
    g_fg_in.generation = sh->fg_generation;
    const uint32_t most = amd ? 1u : static_cast<uint32_t>(g_fg_max); // AMD's makes one between two
    g_fg_in.count = sh->fg_count < 1 ? 1 : sh->fg_count > most ? most : sh->fg_count;
    if (!open(sh->fg_backbuffer, g_fg_in.backbuffer) || !game_fence(sh, sh->fg_fence, g_fg_in.fence) ||
        (sh->fg_hudless && !open(sh->fg_hudless, g_fg_in.hudless))) {
        why = "couldn't open the game's frame";
        return false;
    }
    for (uint32_t i = 0; i < g_fg_in.count; ++i)
        if (!open(sh->fg_output[i], g_fg_in.output[i])) {
            why = "couldn't open frame generation's outputs";
            return false;
        }
    char what[96];
    snprintf(what, sizeof what, "making %s frame generation for %ux%u", amd ? "AMD's" : "DLSS", sh->fg_w, sh->fg_h);
    doing(what);
    if (amd)
        return ffg_make(sh, why);
    NVSDK_NGX_DLSSG_Create_Params p = {};
    p.Width = sh->fg_w, p.Height = sh->fg_h, p.NativeBackbufferFormat = sh->fg_format;
    p.RenderWidth = sh->in_w, p.RenderHeight = sh->in_h, p.DynamicResolutionScaling = false;
    ID3D12GraphicsCommandList* l = begin();
    NVSDK_NGX_Result r = NGX_D3D12_CREATE_DLSSG(l, 1, 1, &g_fg, g_fg_params, &p);
    finish(true);
    if (NVSDK_NGX_FAILED(r) || !g_fg) {
        char text[96];
        snprintf(text, sizeof text, "frame generation couldn't start for %ux%u (0x%08X)", sh->fg_w, sh->fg_h, r);
        why = text;
        g_fg = nullptr;
        return false;
    }
    say("DLSS helper: frame generation at %ux%u (the scene's depth and motion %ux%u), %u frame(s) between two (generation %u)",
        sh->fg_w, sh->fg_h, sh->in_w, sh->in_h, g_fg_in.count, sh->fg_generation);
    return true;
}

void run_fg(VanaDlssShared* sh)
{
    const int64_t woke = qpc(), posted = sh->fg_posted_qpc;
    int64_t evaluate = 0, submit = 0;
    g_list_wait = 0;
    const uint64_t frame = sh->fg_frame;
    if (!sh->cpu_sync) // (with it the game waited for its queue before it set the event)
        g.queue->Wait(g_fg_in.fence, frame);
    const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    NVSDK_NGX_Result r = NVSDK_NGX_Result_Success;
    ffxReturnCode_t amd = FFX_API_RETURN_OK;
    for (uint32_t k = 1; k <= g_fg_in.count; ++k) {
        doing(g_ffg ? "running AMD's frame generation" : "running DLSS frame generation", frame);
        ID3D12GraphicsCommandList* l = begin();
        ID3D12Resource* ins[4] = {g_fg_in.backbuffer, g_fg_in.hudless, g_in.depth, g_in.motion};
        for (ID3D12Resource* res : ins)
            if (res)
                barrier(l, res, D3D12_RESOURCE_STATE_COMMON, in);
        ID3D12Resource* out = g_fg_in.output[k - 1];
        barrier(l, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (g_ffg) {
            // AMD's: configured, prepared and made in this one list (one frame: count is 1)
            ts_mark(0);
            const int64_t from = qpc();
            amd = ffg_run(l, sh, g_in.depth, g_in.motion);
            evaluate += qpc() - from;
            ts_mark(1);
            ts_resolve(2, 2, false);
            for (ID3D12Resource* res : ins)
                if (res)
                    barrier(l, res, in, D3D12_RESOURCE_STATE_COMMON);
            barrier(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            const int64_t at = qpc();
            doing("handing frame generation's work to the graphics card", frame);
            finish(false);
            submit += qpc() - at;
            continue;
        }
        NVSDK_NGX_D3D12_DLSSG_Eval_Params e = {};
        e.pBackbuffer = g_fg_in.backbuffer, e.pDepth = g_in.depth, e.pMVecs = g_in.motion, e.pHudless = g_fg_in.hudless;
        e.pOutputInterpFrame = out;
        NVSDK_NGX_DLSSG_Opt_Eval_Params o = {};
        o.multiFrameCount = g_fg_in.count, o.multiFrameIndex = k;
        memcpy(o.cameraViewToClip, sh->view_to_clip, 64), memcpy(o.clipToCameraView, sh->clip_to_view, 64);
        memcpy(o.clipToPrevClip, sh->clip_to_prev_clip, 64), memcpy(o.prevClipToClip, sh->prev_clip_to_clip, 64);
        for (int i = 0; i < 4; ++i)
            o.clipToLensClip[i][i] = 1.0f;
        // DLSS-G's motion scale makes the motion a share of the frame ([-1, 1]), not render pixels as DLSS's and AMD's
        // do: handed their 1, a pixel's motion read as the frame's width, and it fell back on its own flow, which tore
        // dark stonework into blocks as the camera turned
        const float rw = static_cast<float>(sh->fg_render_w ? sh->fg_render_w : sh->in_w);
        const float rh = static_cast<float>(sh->fg_render_h ? sh->fg_render_h : sh->in_h);
        o.mvecScale[0] = rw > 0.0f ? sh->fg_motion_scale[0] / rw : 0.0f;
        o.mvecScale[1] = rh > 0.0f ? sh->fg_motion_scale[1] / rh : 0.0f;
        // the depth and motion are DLSS's, jittered: without the jitter, its half-pixel step each frame read as the edges
        // moving, and the made frames shimmered along a head against the wall behind it
        o.jitterOffset[0] = sh->fg_jitter_x, o.jitterOffset[1] = sh->fg_jitter_y;
        memcpy(o.cameraPos, sh->cam_pos, 12), memcpy(o.cameraUp, sh->cam_up, 12);
        memcpy(o.cameraRight, sh->cam_right, 12), memcpy(o.cameraFwd, sh->cam_fwd, 12);
        o.cameraNear = sh->cam_near, o.cameraFar = sh->cam_far, o.cameraFOV = sh->cam_fov, o.cameraAspectRatio = sh->cam_aspect;
        o.cameraMotionIncluded = true, o.depthInverted = false, o.reset = sh->fg_reset != 0;
        ts_mark(0);
        const int64_t from = qpc();
        NVSDK_NGX_Result rk = NGX_D3D12_EVALUATE_DLSSG(l, g_fg, g_fg_params, &e, &o);
        evaluate += qpc() - from;
        ts_mark(1);
        ts_resolve(2, 2, false);
        if (NVSDK_NGX_FAILED(rk))
            r = rk;
        for (ID3D12Resource* res : ins)
            if (res)
                barrier(l, res, in, D3D12_RESOURCE_STATE_COMMON);
        barrier(l, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        const int64_t at = qpc();
        doing("handing frame generation's work to the graphics card", frame);
        finish(false);
        submit += qpc() - at;
    }
    g.queue->Signal(g_fg_in.fence, frame + 1);
    if (sh->cpu_sync)
        idle(); // (as run_frame's)
    sh->fg_done = frame;
    sh->gpu_fg_ms = g_gpu_fg_ms;
    if (sh->cpu_sync)
        SetEvent(g_game_done_fg);
    note_frame("frame generation", frame, posted, woke, evaluate, submit);
    static bool said = false;
    if (NVSDK_NGX_FAILED(r) && !said) {
        said = true;
        say("DLSS helper: frame generation failed on a frame (0x%08X)", r);
    }
    static bool amd_said = false;
    if (amd != FFX_API_RETURN_OK && !amd_said) {
        amd_said = true;
        say("%s: AMD's frame generation failed on a frame (FSR API error %u)", g_who, static_cast<unsigned>(amd));
    }
}

void set_state(VanaDlssShared* sh, HANDLE ready, int32_t state, const std::string& message)
{
    snprintf(sh->message, sizeof sh->message, "%s", message.c_str());
    sh->state = state;
    SetEvent(ready);
    say("%s: %s", g_who, message.c_str());
}

// Frame generation's turn (its event, or its frame handed over before the scene's being served): frame generation
// (NVIDIA's or AMD's: fg_kind) made for new textures (seconds, the first time), then said so (fg_made); the frame run
void serve_fg(VanaDlssShared* sh, std::string& why)
{
    if (!g_feature && !g_fsr)
        return;
    const bool amd = sh->fg_kind == VANA_FG_FSR;
    if (amd ? !g_ffg_available : !g_fg_available)
        return;
    if (sh->fg_generation != g_fg_in.generation) {
        if (!take_fg(sh, why)) {
            // frame generation alone gives up: the game's let go of it, the upscaler goes on
            if (amd) {
                snprintf(sh->fg_fsr_message, sizeof sh->fg_fsr_message, "%s", why.c_str());
                sh->fg_fsr_available = 0, g_ffg_available = false;
            } else {
                snprintf(sh->fg_message, sizeof sh->fg_message, "%s", why.c_str());
                sh->fg_available = 0, g_fg_available = false;
            }
            say("%s: %s", g_who, why.c_str());
            return;
        }
        sh->fg_made = g_fg_in.generation;
    }
    if ((amd ? g_ffg != nullptr : g_fg != nullptr) && sh->fg_frame > g_fg_ran) {
        g_fg_ran = sh->fg_frame;
        run_fg(sh);
    }
}

// Till the game ends, or lets this helper go (quit, with ".go")
void wait_let_go(VanaDlssShared* sh, HANDLE game, HANDLE go)
{
    HANDLE waits[2] = {game, go};
    while (!sh->quit)
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1)
            return;
}

int serve(DWORD pid, const wchar_t* log_path)
{
    crash::install(log_path);
    // The game's frame waits on this process each frame, but it has no window: Windows' power throttling (efficiency
    // mode: slow cores, coarse timers) would take it for background work. Out of that, and above normal priority.
    PROCESS_POWER_THROTTLING_STATE throttling = {};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;
    const bool unthrottled = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof throttling) != 0;
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    std::wstring base = VANA_DLSS_MAP + std::to_wstring(pid);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, base.c_str());
    HANDLE go = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L".go").c_str());
    HANDLE ready = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L".ready").c_str());
    HANDLE go_fg = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L".fg").c_str());
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!map || !go || !ready || !go_fg || !game) {
        say("DLSS helper: the game's shared memory, events or process (%lu) weren't there", pid);
        return 2;
    }
    auto* sh = static_cast<VanaDlssShared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanaDlssShared)));
    if (!sh || sh->version != VANA_DLSS_VERSION) {
        say("DLSS helper: the game's shared memory is of another version");
        return 2;
    }
    if (sh->cpu_sync) {
        // no fence is shared (Wine, Proton): the game is told by events that a frame's work is through
        g_game_done = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L".done").c_str());
        g_game_done_fg = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (base + L".fgdone").c_str());
        if (!g_game_done || !g_game_done_fg) {
            say("DLSS helper: the game's done events weren't there");
            return 2;
        }
        say("DLSS helper: no fence shared with the game (Wine or Proton); its frames are waited for on the CPU");
    }
    sh->helper_pid = GetCurrentProcessId();
    g_sh = sh;
    // the upscaler the game started this helper for: FSR, or DLSS (anything else is DLSS, as before there was a choice)
    g_fsr_mode = sh->upscaler == VANA_UPSCALER_FSR;
    g_who = g_fsr_mode ? "FSR helper" : "DLSS helper";
    LUID luid = {sh->luid_low, sh->luid_high};
    std::string why;
    std::wstring data = log_path ? std::wstring(log_path) : std::wstring();
    if (!data.empty())
        data = data.substr(0, data.find_last_of(L"\\/") + 1);
    if (data.empty()) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        data = temp;
    }
    doing("making its Direct3D 12 device");
    if (!make_device(&luid, why)) {
        set_state(sh, ready, VANA_DLSS_FAILED, why);
        return 1;
    }
    doing(g_fsr_mode ? "starting AMD's FSR" : "starting NVIDIA's NGX");
    if (g_fsr_mode ? !fsr_start(why) : !ngx_start(data, why)) {
        set_state(sh, ready, VANA_DLSS_UNAVAILABLE, why);
        // stays to the game's end (or till it's let go), so it doesn't start it again each frame
        wait_let_go(sh, game, go);
        return 0;
    }
    if (g_fsr_mode) {
        // NVIDIA's frame generation and DLSS 5 are DLSS's alone
        g_fg_available = false, g_fg_why = "NVIDIA's frame generation is DLSS's (an NVIDIA RTX 40 or later)";
        g_nr_available = false, g_nr_why = "DLSS 5 is DLSS's";
    } else {
        doing("asking NGX for DLSS 5");
        nr_start();
    }
    // AMD's frame generation, with either upscaler: where its DLLs are here (made, and loaded, when the game asks)
    g_ffg_available = amd_dll_here(L"amd_fidelityfx_loader_dx12.dll") && amd_dll_here(L"amd_fidelityfx_framegeneration_dx12.dll");
    g_ffg_why = g_ffg_available ? "" : "AMD's frame generation DLLs aren't beside VanaCore.Dlss.exe";
    snprintf(sh->fg_fsr_message, sizeof sh->fg_fsr_message, "%s", g_ffg_why.c_str());
    sh->fg_fsr_available = g_ffg_available ? 1 : 0;
    sh->fg_available = g_fg_available ? 1 : 0;
    sh->fg_count_max = g_fg_max;
    snprintf(sh->fg_message, sizeof sh->fg_message, "%s", g_fg_why.c_str());
    snprintf(sh->nr_message, sizeof sh->nr_message, "%s", g_nr_why.c_str());
    sh->nr_available = g_nr_available ? 1 : 0;
    set_state(sh, ready, VANA_DLSS_READY, g_fsr_mode ? "FSR ready" : "DLSS ready");
    say("%s: above normal priority, %s", g_who, unthrottled ? "out of Windows' power throttling" : "power throttling unchanged");
    if (!g_fsr_mode && !g_fg_available)
        say("DLSS helper: %s", g_fg_why.c_str());
    if (!g_fsr_mode && !g_nr_available)
        say("DLSS helper: DLSS 5: %s", g_nr_why.c_str());
    bool let_go = false;
    for (;;) {
        // the scene's (DLSS, FSR) or the frame's (frame generation): with both set, the lowest (the scene's) is returned
        HANDLE waits[3] = {go, go_fg, game};
        doing("waiting for the game's next frame");
        DWORD r = WaitForMultipleObjects(3, waits, FALSE, 2000);
        if (r == WAIT_OBJECT_0 + 2 || r == WAIT_FAILED)
            break;
        if (sh->quit) {
            let_go = true; // the game starts another helper, for the other upscaler
            break;
        }
        if (sh->state != VANA_DLSS_READY)
            continue;
        if (r == WAIT_OBJECT_0) {
            // A frame generation handed over before this scene goes first. The game's queue waits on it before it
            // hands the next scene over; this scene's DLSS, queued first, would wait on that hand-over for good: both
            // queues stuck, the card reset (a driver timeout, 2026-10-17, resizing the window with frame generation on,
            // when this side fell a frame behind)
            if (sh->fg_frame > g_fg_ran && sh->fg_posted_qpc < sh->posted_qpc)
                serve_fg(sh, why);
            // new textures: DLSS made for them, then said so (made); the game hands frames over from then on
            if (sh->generation != g_in.generation) {
                if (!take_inputs(sh, why)) {
                    set_state(sh, ready, VANA_DLSS_FAILED, why);
                    continue;
                }
                sh->made = g_in.generation;
            }
            if ((g_feature || g_fsr) && sh->frame > g_ran) {
                g_ran = sh->frame;
                run_frame(sh);
            }
        } else if (r == WAIT_OBJECT_0 + 1) {
            serve_fg(sh, why);
        }
    }
    doing("letting go of DLSS and frame generation");
    drop_fg();
    drop_inputs();
    if (!g_fsr_mode)
        NVSDK_NGX_D3D12_Shutdown1(g.dev);
    say("%s: %s; done", g_who, let_go ? "the game let it go" : "the game's gone");
    return 0;
}

// --check's DLSS (and DLSS 5), on an NVIDIA card: whether it runs
bool check_dlss()
{
    std::string why;
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    if (!ngx_start(temp, why)) {
        say("DLSS: no (%s)", why.c_str());
        return false;
    }
    say("DLSS: yes");
    if (g_fg_available)
        say("Frame generation: yes, up to %dx", g_fg_max + 1);
    else
        say("Frame generation: no (%s)", g_fg_why.c_str());
    const struct { NVSDK_NGX_PerfQuality_Value q; const char* name; } presets[] = {
        {NVSDK_NGX_PerfQuality_Value_DLAA, "DLAA"},
        {NVSDK_NGX_PerfQuality_Value_MaxQuality, "Quality"},
        {NVSDK_NGX_PerfQuality_Value_Balanced, "Balanced"},
        {NVSDK_NGX_PerfQuality_Value_MaxPerf, "Performance"},
        {NVSDK_NGX_PerfQuality_Value_UltraPerformance, "Ultra performance"},
    };
    for (const auto& p : presets) {
        unsigned w = 0, h = 0;
        optimal(3840, 2160, p.q, w, h);
        say("  %s: 3840x2160 from %ux%u", p.name, w, h);
    }
    // DLSS 5: asked of NGX at a size, as the game's first frame with it would
    nr_start();
    std::string nr_why = g_nr_why;
    if (g_nr_available) {
        const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        NrFrame f;
        f.w = f.rw = 1280, f.h = f.rh = 720;
        f.color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, f.w, f.h, D3D12_RESOURCE_FLAG_NONE, in);
        f.depth = texture(DXGI_FORMAT_R32_FLOAT, f.w, f.h, D3D12_RESOURCE_FLAG_NONE, in);
        f.motion = texture(DXGI_FORMAT_R16G16_FLOAT, f.w, f.h, D3D12_RESOURCE_FLAG_NONE, in);
        nr_why = "couldn't make its textures";
        g_nr_available = f.color && f.depth && f.motion && nr_make(f, nr_why);
        if (g_nr_available) {
            // a frame through it, then made again at another size while it's there: as the game changing DLSS's
            // size or quality with DLSS 5 on
            ID3D12GraphicsCommandList* l = begin();
            nr_set(f);
            const NVSDK_NGX_Result e = NVSDK_NGX_D3D12_EvaluateFeature(l, g_nr, g_nr_params, nullptr);
            finish(true);
            say("  DLSS 5: a frame at %ux%u: 0x%08X (the device 0x%08X)", f.w, f.h, e,
                static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
            NrFrame f2 = f;
            f2.w = f2.rw = 1920, f2.h = f2.rh = 1080;
            f2.color = texture(DXGI_FORMAT_R16G16B16A16_FLOAT, f2.w, f2.h, D3D12_RESOURCE_FLAG_NONE, in);
            f2.depth = texture(DXGI_FORMAT_R32_FLOAT, f2.w, f2.h, D3D12_RESOURCE_FLAG_NONE, in);
            f2.motion = texture(DXGI_FORMAT_R16G16_FLOAT, f2.w, f2.h, D3D12_RESOURCE_FLAG_NONE, in);
            nr_why = "couldn't make its textures again";
            g_nr_available = f2.color && f2.depth && f2.motion && nr_make(f2, nr_why);
            nr_drop();
            release(f2.color), release(f2.depth), release(f2.motion);
        }
        nr_drop();
        release(f.color), release(f.depth), release(f.motion);
    }
    if (g_nr_available)
        say("DLSS 5: yes");
    else
        say("DLSS 5: no (%s)", nr_why.c_str());
    NVSDK_NGX_D3D12_Shutdown1(g.dev);
    return true;
}

// --check's FSR, on any card: whether it runs - made for 1920x1080 to 3840x2160 and a frame through it, as the game's
// first would be
bool check_fsr()
{
    g_fsr_mode = true, g_who = "FSR";
    std::string why;
    if (!fsr_start(why)) {
        say("FSR: no (%s)", why.c_str());
        return false;
    }
    VanaDlssShared sh = {}; // what fsr_make and fsr_run read of the game's
    sh.in_w = sh.render_w = 1920, sh.in_h = sh.render_h = 1080, sh.out_w = 3840, sh.out_h = 2160;
    sh.depth_infinite = 1, sh.motion_scale_x = sh.motion_scale_y = 1.0f;
    sh.sr_near = 0.1f, sh.sr_fov = 1.0f, sh.sr_frame_ms = 16.7f, sh.reset = 1;
    const D3D12_RESOURCE_STATES in = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    g_in.color = texture(DXGI_FORMAT_R8G8B8A8_UNORM, sh.in_w, sh.in_h, D3D12_RESOURCE_FLAG_NONE, in);
    g_in.depth = texture(DXGI_FORMAT_R32_FLOAT, sh.in_w, sh.in_h, D3D12_RESOURCE_FLAG_NONE, in);
    g_in.motion = texture(DXGI_FORMAT_R16G16_FLOAT, sh.in_w, sh.in_h, D3D12_RESOURCE_FLAG_NONE, in);
    g_in.output = texture(DXGI_FORMAT_R8G8B8A8_UNORM, sh.out_w, sh.out_h, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_in.out_w = sh.out_w, g_in.out_h = sh.out_h;
    bool yes = false;
    if (!g_in.color || !g_in.depth || !g_in.motion || !g_in.output) {
        say("FSR: no (couldn't make its textures)");
    } else if (!fsr_make(&sh, why)) {
        say("FSR: no (%s)", why.c_str());
    } else {
        ID3D12GraphicsCommandList* l = begin();
        const ffxReturnCode_t r = fsr_run(l, &sh);
        finish(true);
        yes = r == FFX_API_RETURN_OK;
        if (yes)
            say("FSR: yes, %s (a frame from 1920x1080 to 3840x2160)", sh.sr_version);
        else
            say("FSR: no (%s made, but a frame through it failed: FSR API error %u; the device 0x%08X)", sh.sr_version,
                static_cast<unsigned>(r), static_cast<unsigned>(g.dev->GetDeviceRemovedReason()));
    }
    fsr_drop();
    release(g_in.color), release(g_in.depth), release(g_in.motion), release(g_in.output);
    return yes;
}

int check()
{
    std::string why;
    if (!make_device(nullptr, why)) {
        say("DLSS: no (%s)", why.c_str());
        say("FSR: no (%s)", why.c_str());
        return 1;
    }
    bool dlss = false;
    if (g_nvidia)
        dlss = check_dlss();
    else
        say("DLSS: no (it needs an NVIDIA RTX card)");
    const bool fsr = check_fsr();
    // AMD's frame generation: made for a 3840x2160 frame from a 1920x1080 scene, as the game's would be
    if (!amd_dll_here(L"amd_fidelityfx_framegeneration_dx12.dll")) {
        say("FSR frame generation: no (amd_fidelityfx_framegeneration_dx12.dll isn't beside VanaCore.Dlss.exe)");
    } else {
        VanaDlssShared f = {};
        f.fg_w = 3840, f.fg_h = 2160, f.in_w = 1920, f.in_h = 1080, f.depth_infinite = 1;
        f.fg_format = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (ffg_make(&f, why))
            say("FSR frame generation: yes, %s (2x)", f.fg_version);
        else
            say("FSR frame generation: no (%s)", why.c_str());
        ffg_drop();
    }
    return dlss || fsr ? 0 : 1;
}

// --- --hw: the graphics card's readings for the game ---------------------------------------------------------------
// NVIDIA's NVML (nvml.dll, put in System32 by the driver) is 64-bit only, which is why this program reads it for the
// 32-bit game: clock, temperature, power and its limit, how busy the card is, its memory, and the reasons NVML gives for
// the clocks being held down. Loaded by name: nothing of NVIDIA's is linked or shipped.

namespace hw {

using Ret = int; // nvmlReturn_t: 0 is success
using Dev = void*;
struct Util {
    unsigned gpu, memory;
};
struct Mem {
    unsigned long long total, free, used;
};
struct Api {
    HMODULE dll = nullptr;
    Ret(__cdecl* init)() = nullptr;
    Ret(__cdecl* by_index)(unsigned, Dev*) = nullptr;
    Ret(__cdecl* name)(Dev, char*, unsigned) = nullptr;
    Ret(__cdecl* clock)(Dev, int, unsigned*) = nullptr;
    Ret(__cdecl* max_clock)(Dev, int, unsigned*) = nullptr;
    Ret(__cdecl* temperature)(Dev, int, unsigned*) = nullptr;
    Ret(__cdecl* power)(Dev, unsigned*) = nullptr;
    Ret(__cdecl* power_limit)(Dev, unsigned*) = nullptr;
    Ret(__cdecl* utilization)(Dev, Util*) = nullptr;
    Ret(__cdecl* memory)(Dev, Mem*) = nullptr;
    Ret(__cdecl* throttle)(Dev, unsigned long long*) = nullptr;
};

template <typename T>
bool load(HMODULE dll, const char* name, T& out)
{
    out = reinterpret_cast<T>(GetProcAddress(dll, name));
    return out != nullptr;
}

int run(DWORD pid)
{
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    const std::wstring name = std::wstring(VANA_HW_MAP) + std::to_wstring(pid);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(VanaHwShared), name.c_str());
    if (!map || !game)
        return 2;
    auto* sh = static_cast<VanaHwShared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(VanaHwShared)));
    if (!sh)
        return 2;
    memset(sh, 0, sizeof *sh);
    sh->version = VANA_HW_VERSION;
    const auto say = [&](int state, const char* why) {
        snprintf(sh->message, sizeof sh->message, "%s", why);
        sh->state = state;
    };

    Api api;
    api.dll = LoadLibraryW(L"nvml.dll");
    if (!api.dll)
        api.dll = LoadLibraryW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    Dev dev = nullptr;
    if (!api.dll) {
        say(VANA_HW_UNAVAILABLE, "nvml.dll isn't on this machine (NVIDIA's driver puts it in System32)");
    } else if (!load(api.dll, "nvmlInit_v2", api.init) || !load(api.dll, "nvmlDeviceGetHandleByIndex_v2", api.by_index) ||
               !load(api.dll, "nvmlDeviceGetClockInfo", api.clock) || !load(api.dll, "nvmlDeviceGetTemperature", api.temperature)) {
        say(VANA_HW_UNAVAILABLE, "nvml.dll is missing functions this needs");
    } else if (api.init() != 0 || api.by_index(0, &dev) != 0 || !dev) {
        say(VANA_HW_UNAVAILABLE, "NVML found no NVIDIA graphics card");
    } else {
        load(api.dll, "nvmlDeviceGetName", api.name);
        load(api.dll, "nvmlDeviceGetMaxClockInfo", api.max_clock);
        load(api.dll, "nvmlDeviceGetPowerUsage", api.power);
        load(api.dll, "nvmlDeviceGetEnforcedPowerLimit", api.power_limit);
        load(api.dll, "nvmlDeviceGetUtilizationRates", api.utilization);
        load(api.dll, "nvmlDeviceGetMemoryInfo", api.memory);
        if (!load(api.dll, "nvmlDeviceGetCurrentClocksEventReasons", api.throttle))
            load(api.dll, "nvmlDeviceGetCurrentClocksThrottleReasons", api.throttle);
        if (api.name)
            api.name(dev, sh->name, sizeof sh->name);
        say(VANA_HW_RUNNING, "");
    }

    while (WaitForSingleObject(game, 1000) == WAIT_TIMEOUT) {
        if (sh->state != VANA_HW_RUNNING)
            continue;
        unsigned v = 0;
        if (api.clock(dev, 0, &v) == 0)
            sh->gpu_mhz = v; /* NVML_CLOCK_GRAPHICS */
        if (api.max_clock && api.max_clock(dev, 0, &v) == 0)
            sh->gpu_max_mhz = v;
        if (api.temperature(dev, 0, &v) == 0)
            sh->temp = static_cast<int32_t>(v);
        if (api.power && api.power(dev, &v) == 0)
            sh->power_mw = v;
        if (api.power_limit && api.power_limit(dev, &v) == 0)
            sh->limit_mw = v;
        Util u{};
        if (api.utilization && api.utilization(dev, &u) == 0)
            sh->busy = u.gpu;
        Mem m{};
        if (api.memory && api.memory(dev, &m) == 0)
            sh->vram_used = m.used, sh->vram_total = m.total;
        unsigned long long reasons = 0;
        if (api.throttle && api.throttle(dev, &reasons) == 0)
            sh->throttle = reasons;
        sh->tick = sh->tick + 1;
    }
    return 0;
}

} // namespace hw

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc >= 3 && wcscmp(argv[1], L"--hw") == 0)
        return hw::run(static_cast<DWORD>(wcstoul(argv[2], nullptr, 10)));
    if (argc >= 2 && wcscmp(argv[1], L"--check") == 0)
        return check();
    if (argc < 2) {
        fprintf(stderr, "VanaCore.Dlss.exe <game pid> [log file] | --check | --hw <game pid>\n");
        return 2;
    }
    g_quiet = true;
    // the log file is argv[2]; empty (the game's [logging] enabled=0): no file at all
    if (argc >= 3 && argv[2][0] != L'\0')
        g_log = _wfopen(argv[2], L"a");
    const DWORD pid = static_cast<DWORD>(wcstoul(argv[1], nullptr, 10));
    const int code = serve(pid, argc >= 3 ? argv[2] : nullptr);
    if (g_log)
        fclose(g_log);
    return code;
}
