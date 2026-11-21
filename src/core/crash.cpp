#include "crash.hpp"

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>

#include <vanacore/log_file.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "imports.hpp"
#include "log.hpp"
#include "shutdown.hpp"

namespace vanacore::crash {
namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_next = nullptr; // the game's handler, called after VanaCore's
decltype(&SetUnhandledExceptionFilter) g_set_filter = nullptr;
std::atomic<bool> g_crashing{false}; // the first crash gets the full report
std::atomic<ULONGLONG> g_shutdown_at{0};   // when the game was told to /shutdown (GetTickCount64), 0 never
constexpr ULONGLONG kShutdownWindow = 120'000; // its countdown and teardown

int g_dumps = 0;
std::wstring g_logs; // where dumps go

// From dbghelp.dll, loaded up front: nothing should be loaded once the game has crashed.
decltype(&MiniDumpWriteDump) g_write_dump = nullptr;
decltype(&StackWalk64) g_stack_walk = nullptr;

// The report is written by a thread of its own: the crashed thread's stack may be nearly gone
// (a stack overflow), and walking stacks and writing dumps want plenty.
HANDLE g_request = nullptr;
HANDLE g_done = nullptr;
EXCEPTION_POINTERS* g_exception = nullptr;
DWORD g_thread_id = 0;

// The DLLs other than Windows' own: path, base address, size and build time (from the PE header).
void log_modules()
{
    HMODULE modules[512];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    int windows = 0;
    const DWORD count = bytes / sizeof(HMODULE) < 512 ? bytes / sizeof(HMODULE) : 512;
    for (DWORD i = 0; i < count; ++i) {
        if (imports::is_windows_module(modules[i])) {
            ++windows;
            continue;
        }
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(modules[i], path, MAX_PATH);
        MODULEINFO info{};
        K32GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
        const auto* base = reinterpret_cast<const BYTE*>(modules[i]);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        const __time64_t built = nt->FileHeader.TimeDateStamp;
        tm time{};
        wchar_t when[32] = L"?";
        if (_gmtime64_s(&time, &built) == 0)
            wcsftime(when, 32, L"%Y-%m-%d %H:%M:%S UTC", &time);
        log::write_from_crash(L"CRASH: module %ls at 0x%p, %lu bytes, built %ls", path, modules[i], info.SizeOfImage,
                              when);
    }
    log::write_from_crash(L"CRASH: plus %d Windows modules", windows);
}

DWORD64 CALLBACK module_base(HANDLE, DWORD64 address)
{
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(address)), &module)
               ? reinterpret_cast<DWORD64>(module)
               : 0;
}

PVOID CALLBACK no_function_table(HANDLE, DWORD64)
{
    return nullptr; // no symbols: frames are followed through EBP
}

bool is_code(ULONG_PTR address)
{
    MEMORY_BASIC_INFORMATION info{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != 0 &&
           info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
           (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Committed executable memory, a DLL's or made as the game runs; its region in `region`.
bool executable(std::uintptr_t address, MEMORY_BASIC_INFORMATION& region)
{
    return VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != 0 && region.State == MEM_COMMIT &&
           (region.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0 &&
           (region.Protect & PAGE_GUARD) == 0;
}

// Does a call end at `address` (in `region`, so the bytes before it can be read)? E8 and a 32-bit offset, or FF /2: a
// call through a register or memory, 2 to 7 bytes with its ModRM, SIB and displacement.
bool after_call(std::uintptr_t address, const MEMORY_BASIC_INFORMATION& region)
{
    const std::uintptr_t before = address - reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    const auto* code = reinterpret_cast<const std::uint8_t*>(address);
    if (before >= 5 && code[-5] == 0xE8)
        return true;
    for (std::uintptr_t length = 2; length <= 7 && length <= before; ++length) {
        const std::uint8_t* call = code - length;
        if (call[0] != 0xFF || ((call[1] >> 3) & 7) != 2)
            continue;
        const int mod = call[1] >> 6;
        const int rm = call[1] & 7;
        std::uintptr_t size = 2;
        if (mod != 3 && rm == 4)
            ++size; // a SIB byte
        if (mod == 1)
            size += 1;
        else if (mod == 2 || (mod == 0 && rm == 5) || (mod == 0 && rm == 4 && length >= 3 && (call[2] & 7) == 5))
            size += 4;
        if (size == length)
            return true;
    }
    return false;
}

void gpu_line(const char* line)
{
    log::write_from_crash(L"CRASH: %hs", line);
}

// Everything after the first CRASH line; runs on the report thread.
void report(const EXCEPTION_POINTERS& exception, DWORD thread_id)
{
    // The graphics card lost first (a driver timeout): the game often crashes on what follows. Why, and with
    // [logging] gpu where the card stopped - unless Present or a fence wait has logged it already.
    vanacore_gfx_gpu_lost(&gpu_line);
    const CONTEXT& c = *exception.ContextRecord;
    log::write_from_crash(L"CRASH: registers EIP=%08lX EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX "
                          L"EBP=%08lX ESP=%08lX EFLAGS=%08lX",
                          c.Eip, c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi, c.Ebp, c.Esp, c.EFlags);

    // The call stack, walked from the crash's registers (EBP frames: FFXI ships no symbols).
    if (g_stack_walk != nullptr) {
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, thread_id);
        CONTEXT walk = c;
        STACKFRAME64 frame{};
        frame.AddrPC = {c.Eip, 0, AddrModeFlat};
        frame.AddrFrame = {c.Ebp, 0, AddrModeFlat};
        frame.AddrStack = {c.Esp, 0, AddrModeFlat};
        std::wstring line;
        int count = 0;
        for (; count < 48 && g_stack_walk(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), thread, &frame, &walk, nullptr,
                                         &no_function_table, &module_base, nullptr);
             ++count) {
            if (frame.AddrPC.Offset == 0)
                break;
            line += L" #" + std::to_wstring(count) + L" " +
                    describe_address(reinterpret_cast<const void*>(static_cast<ULONG_PTR>(frame.AddrPC.Offset)));
            if ((count + 1) % 8 == 0) {
                log::write_from_crash(L"CRASH: stack%ls", line.c_str());
                line.clear();
            }
        }
        if (!line.empty())
            log::write_from_crash(L"CRASH: stack%ls", line.c_str());
        if (thread != nullptr)
            CloseHandle(thread);
    }

    // Code addresses on the stack: return addresses the walk above can miss (frames without EBP).
    std::wstring found;
    int hits = 0;
    for (ULONG_PTR at = c.Esp; hits < 32 && at < c.Esp + 4096; at += 4) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE)) == 0 ||
            (info.Protect & PAGE_GUARD) != 0)
            break;
        const ULONG_PTR value = *reinterpret_cast<const ULONG_PTR*>(at);
        if (!is_code(value))
            continue;
        found += L" [esp+" + std::to_wstring(at - c.Esp) + L"] " + describe_address(reinterpret_cast<const void*>(value));
        if (++hits % 8 == 0) {
            log::write_from_crash(L"CRASH: code on the stack%ls", found.c_str());
            found.clear();
        }
    }
    if (!found.empty())
        log::write_from_crash(L"CRASH: code on the stack%ls", found.c_str());

    log_modules();

    if (g_write_dump == nullptr) {
        log::write_from_crash(L"CRASH: no dump: dbghelp.dll didn't load");
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\crash-%04u-%02u-%02u_%02u-%02u-%02u-pid%lu.dmp", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, GetCurrentProcessId());
    const std::wstring path = g_logs + name;
    CreateDirectoryW(g_logs.c_str(), nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        log::write_from_crash(L"CRASH: couldn't create %ls (error %lu)", path.c_str(), GetLastError());
        return;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(
        g_dumps >= 2 ? MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithUnloadedModules |
                           MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                           MiniDumpIgnoreInaccessibleMemory
                     : MiniDumpWithDataSegs | MiniDumpWithCodeSegs | MiniDumpWithHandleData |
                           MiniDumpWithUnloadedModules | MiniDumpWithIndirectlyReferencedMemory |
                           MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo |
                           MiniDumpIgnoreInaccessibleMemory);
    MINIDUMP_EXCEPTION_INFORMATION info{thread_id, const_cast<EXCEPTION_POINTERS*>(&exception), FALSE};
    const BOOL written = g_write_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &info, nullptr, nullptr);
    const DWORD error = GetLastError();
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    CloseHandle(file);
    if (written)
        log::write_from_crash(L"CRASH: dump written to %ls (%.1f MB)", path.c_str(),
                              static_cast<double>(size.QuadPart) / (1024.0 * 1024.0));
    else
        log::write_from_crash(L"CRASH: writing %ls failed (0x%08lX)", path.c_str(), error);
}

DWORD WINAPI report_thread(void*)
{
    WaitForSingleObject(g_request, INFINITE);
    report(*g_exception, g_thread_id);
    SetEvent(g_done);
    return 0;
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    const wchar_t* name = describe_status(record.ExceptionCode);
    const std::wstring where = describe_address(record.ExceptionAddress);
    // the game quitting (/shutdown): its own teardown faults, every time - noted, not reported
    const ULONGLONG asked = g_shutdown_at.load();
    if (asked != 0 && GetTickCount64() - asked < kShutdownWindow) {
        log::write_from_crash(L"Shutdown: the game faulted while quitting (%ls 0x%08lX at %ls): no crash report or dump; "
                              L"it ends as quitting normally does",
                              name != nullptr ? name : L"exception", record.ExceptionCode, where.c_str());
        // addons and plugins unloaded first (their settings saved), if the game's window hadn't closed yet
        shutdown::run_bounded(L"the game faulted while quitting", 3000);
        // It was quitting anyway: exit code 0, so VanaCore.exe sees the game end, not crash.
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        log::write_from_crash(L"CRASH: the game crashed: access violation (0x%08lX) at %ls %ls 0x%p (thread %lu)",
                              record.ExceptionCode, where.c_str(),
                              kind == 0 ? L"reading" : kind == 1 ? L"writing" : L"executing",
                              reinterpret_cast<void*>(record.ExceptionInformation[1]), GetCurrentThreadId());
    } else {
        log::write_from_crash(L"CRASH: the game crashed: %ls (0x%08lX) at %ls (thread %lu)",
                              name != nullptr ? name : L"unhandled exception", record.ExceptionCode, where.c_str(),
                              GetCurrentThreadId());
    }
    // The full report once: another thread crashing meanwhile just gets its line above.
    if (g_dumps > 0 && !g_crashing.exchange(true)) {
        g_exception = info;
        g_thread_id = GetCurrentThreadId();
        if (g_request != nullptr) {
            SetEvent(g_request);
            WaitForSingleObject(g_done, g_dumps >= 2 ? 300'000 : 60'000);
        } else {
            report(*info, GetCurrentThreadId());
        }
    }
    return g_next != nullptr ? g_next(info) : EXCEPTION_CONTINUE_SEARCH;
}

// Stands in for the game's SetUnhandledExceptionFilter: its handler goes behind VanaCore's.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI set_filter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
{
    const LPTOP_LEVEL_EXCEPTION_FILTER previous = g_next;
    g_next = filter;
    return previous;
}

void on_module(HMODULE module)
{
    if (!imports::is_windows_module(module))
        imports::hook(module, "KERNEL32.dll", "SetUnhandledExceptionFilter", &set_filter, g_set_filter);
}

} // namespace

bool cpp_exception_message(const EXCEPTION_RECORD& record, char* out, std::size_t size)
{
    static_assert(sizeof(void*) == 4, "the exception's type information is read as 32-bit pointers");
    if (out == nullptr || size == 0)
        return false;
    out[0] = '\0';
    if (record.ExceptionCode != kCppException || record.NumberParameters < 3)
        return false;
    const ULONG_PTR magic = record.ExceptionInformation[0];
    if (magic != 0x19930520 && magic != 0x19930521 && magic != 0x19930522)
        return false;
    __try {
        // The thrown object; its ThrowInfo; that's CatchableTypeArray: each type it can be caught
        // as, with where that base sits in the object (mdisp).
        const auto* object = reinterpret_cast<const BYTE*>(record.ExceptionInformation[1]);
        const auto* info = reinterpret_cast<const DWORD*>(record.ExceptionInformation[2]);
        const auto* types = reinterpret_cast<const DWORD*>(info[3]);
        for (DWORD i = 0; i < types[0] && i < 64; ++i) {
            const auto* type = reinterpret_cast<const DWORD*>(types[1 + i]);
            const char* name = reinterpret_cast<const char*>(type[1]) + 8; // TypeDescriptor::name
            if (std::strcmp(name, ".?AVexception@std@@") != 0)
                continue;
            const BYTE* exception = object + static_cast<int>(type[2]);
            using What = const char*(__thiscall*)(const void*);
            const auto what = reinterpret_cast<What>((*reinterpret_cast<void* const* const*>(exception))[1]);
            const char* text = what(exception);
            if (text == nullptr)
                return false;
            strncpy_s(out, size, text, _TRUNCATE);
            return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = '\0';
    }
    return false;
}

int callers(const CONTEXT& context, std::uintptr_t* out, int max)
{
    MEMORY_BASIC_INFORMATION stack{};
    if (out == nullptr || VirtualQuery(reinterpret_cast<const void*>(context.Esp), &stack, sizeof(stack)) == 0 ||
        stack.State != MEM_COMMIT || (stack.Protect & PAGE_GUARD) != 0)
        return 0;
    // the stack's committed part from ESP up, as far as 2 KB
    const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(stack.BaseAddress) + stack.RegionSize;
    const std::uintptr_t end = top < context.Esp + 2048 ? top : context.Esp + 2048;
    int count = 0;
    __try {
        for (std::uintptr_t at = context.Esp; count < max && at + 4 <= end; at += 4) {
            const std::uintptr_t value = *reinterpret_cast<const std::uintptr_t*>(at);
            MEMORY_BASIC_INFORMATION code{};
            if (executable(value, code) && after_call(value, code))
                out[count++] = value;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // code unmapped as it was read (LuaJIT's, flushed): what was found stands
    }
    return count;
}

void shutting_down()
{
    g_shutdown_at = GetTickCount64();
    log::write(L"Crash: /shutdown: while the game quits, a fault in its teardown is noted in the log, not dumped");
}

void install(const wchar_t* install_dir, int dumps)
{
    g_dumps = dumps;
    // the DLSS helper (dlss\VanaCore.Dlss.exe, started from this process: it has this environment) reports its own crashes
    // the same way, into this log
    SetEnvironmentVariableW(L"VANA_CRASHDUMPS", std::to_wstring(dumps).c_str());
    g_logs = std::wstring(install_dir) + L"\\logs";
    if (dumps > 0) {
        if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
            g_write_dump = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
            g_stack_walk = reinterpret_cast<decltype(&StackWalk64)>(GetProcAddress(dbghelp, "StackWalk64"));
        }
        g_request = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        g_done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (g_request == nullptr || g_done == nullptr ||
            CloseHandle(CreateThread(nullptr, 0, &report_thread, nullptr, 0, nullptr)) == FALSE)
            g_request = nullptr; // report on the crashed thread instead
    }
    g_next = SetUnhandledExceptionFilter(&on_crash);
    imports::for_each_module(&on_module);
}

} // namespace vanacore::crash
