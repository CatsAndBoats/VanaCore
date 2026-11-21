#include "imports.hpp"

#include "disasm.hpp"

#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace vanacore::imports {
namespace {

// ntdll's DLL load notifications (documented with LdrRegisterDllNotification; no SDK header).
struct DllNotificationData {
    ULONG flags;
    const UNICODE_STRING* full_name;
    const UNICODE_STRING* base_name;
    void* base;
    ULONG size;
};
constexpr ULONG kDllLoaded = 1;   // LDR_DLL_NOTIFICATION_REASON_LOADED
constexpr ULONG kDllUnloaded = 2; // LDR_DLL_NOTIFICATION_REASON_UNLOADED
using DllNotification = void(CALLBACK*)(ULONG reason, const DllNotificationData* data, void* context);
using LdrRegisterDllNotification = LONG(NTAPI*)(ULONG flags, DllNotification callback, void* context, void** cookie);

struct Watch {
    std::wstring name; // empty: every DLL
    void (*on_load)(HMODULE);
};

template <typename T>
T* at(HMODULE module, DWORD rva)
{
    return reinterpret_cast<T*>(reinterpret_cast<BYTE*>(module) + rva);
}

// Windows 7 sends the load notification before the loader binds the DLL's imports (Windows 8 and later, after), so
// an import hook placed then fails: FFXi.dll's CoCreateInstance went to the registry, which has no PlayOnline. Such
// a DLL's on_load calls wait for its entry point instead: a jump there runs them, puts the entry's bytes back and
// calls it, as the loader would have. All under the loader lock, which the notifications and entry points hold.
struct Deferred {
    HMODULE module;
    BYTE* entry;
    BYTE saved[5]; // what jump() overwrote
    std::vector<void (*)(HMODULE)> on_load;
};
std::vector<Deferred>* g_deferred = nullptr;

// False while the loader hasn't bound `module`'s imports: a slot then still holds what its name table does.
bool imports_bound(HMODULE module)
{
    const auto* nt = at<IMAGE_NT_HEADERS>(module, at<IMAGE_DOS_HEADER>(module, 0)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0)
        return true;
    for (auto* entry = at<IMAGE_IMPORT_DESCRIPTOR>(module, imports.VirtualAddress); entry->Name != 0; ++entry) {
        if (entry->OriginalFirstThunk == 0 || entry->FirstThunk == 0)
            continue;
        const ULONG_PTR slot = at<IMAGE_THUNK_DATA>(module, entry->FirstThunk)->u1.Function;
        if (slot != 0)
            return slot != at<IMAGE_THUNK_DATA>(module, entry->OriginalFirstThunk)->u1.Function;
    }
    return true;
}

BOOL WINAPI deferred_entry(HINSTANCE instance, DWORD reason, void* reserved)
{
    const auto found = std::find_if(g_deferred->begin(), g_deferred->end(),
                                    [instance](const Deferred& deferred) { return deferred.module == instance; });
    if (found == g_deferred->end())
        return TRUE; // not reached: only a recorded entry jumps here, and its record goes when it runs
    Deferred deferred = std::move(*found);
    g_deferred->erase(found);
    DWORD protect = 0;
    if (VirtualProtect(deferred.entry, sizeof(deferred.saved), PAGE_EXECUTE_READWRITE, &protect)) {
        std::memcpy(deferred.entry, deferred.saved, sizeof(deferred.saved));
        VirtualProtect(deferred.entry, sizeof(deferred.saved), protect, &protect);
        FlushInstructionCache(GetCurrentProcess(), deferred.entry, sizeof(deferred.saved));
    }
    for (const auto on_load : deferred.on_load)
        on_load(instance);
    using Entry = BOOL(WINAPI*)(HINSTANCE, DWORD, void*);
    return reinterpret_cast<Entry>(deferred.entry)(instance, reason, reserved);
}

// Calls on_load(module) when the loader calls module's entry point, its imports bound by then. False if the DLL has
// no entry point or it can't be patched.
bool defer(HMODULE module, void (*on_load)(HMODULE))
{
    if (g_deferred == nullptr)
        g_deferred = new std::vector<Deferred>;
    for (Deferred& deferred : *g_deferred)
        if (deferred.module == module) {
            deferred.on_load.push_back(on_load);
            return true;
        }
    const auto* nt = at<IMAGE_NT_HEADERS>(module, at<IMAGE_DOS_HEADER>(module, 0)->e_lfanew);
    if (nt->OptionalHeader.AddressOfEntryPoint == 0)
        return false;
    Deferred deferred{module, at<BYTE>(module, nt->OptionalHeader.AddressOfEntryPoint), {}, {on_load}};
    std::memcpy(deferred.saved, deferred.entry, sizeof(deferred.saved));
    if (!jump(deferred.entry, reinterpret_cast<void*>(&deferred_entry)))
        return false;
    g_deferred->push_back(std::move(deferred));
    return true;
}

void CALLBACK dll_notification(ULONG reason, const DllNotificationData* data, void* context)
{
    const auto module = static_cast<HMODULE>(data->base);
    if (reason == kDllUnloaded && g_deferred != nullptr) { // a load that failed before the entry point ran
        g_deferred->erase(std::remove_if(g_deferred->begin(), g_deferred->end(),
                                         [module](const Deferred& deferred) { return deferred.module == module; }),
                          g_deferred->end());
        return;
    }
    const auto* watch = static_cast<const Watch*>(context);
    const UNICODE_STRING& name = *data->base_name;
    if (reason == kDllLoaded &&
        (watch->name.empty() || (name.Length == watch->name.size() * sizeof(wchar_t) &&
                                 _wcsnicmp(name.Buffer, watch->name.c_str(), watch->name.size()) == 0)) &&
        (imports_bound(module) || !defer(module, watch->on_load)))
        watch->on_load(module);
}

bool watch(std::wstring name, void (*on_load)(HMODULE))
{
    const auto register_notification = reinterpret_cast<LdrRegisterDllNotification>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
    void* cookie = nullptr; // never unregistered: the watch lasts as long as the process
    return register_notification != nullptr &&
           register_notification(0, &dll_notification, new Watch{std::move(name), on_load}, &cookie) >= 0;
}

} // namespace

bool hook(HMODULE module, const char* dll, const char* function, void* replacement, void** original)
{
    const auto* nt = at<IMAGE_NT_HEADERS>(module, at<IMAGE_DOS_HEADER>(module, 0)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0)
        return false;

    // "#11" names an import by its ordinal, as FFXiMain.dll imports DirectSoundCreate8
    const WORD ordinal = function[0] == '#' ? static_cast<WORD>(std::atoi(function + 1)) : 0;

    // Without a name table (OriginalFirstThunk 0) the import is recognised by its bound address.
    const HMODULE exporter = GetModuleHandleA(dll);
    const FARPROC bound = exporter != nullptr ? GetProcAddress(exporter, ordinal != 0 ? MAKEINTRESOURCEA(ordinal) : function)
                                              : nullptr;

    for (auto* entry = at<IMAGE_IMPORT_DESCRIPTOR>(module, imports.VirtualAddress); entry->Name != 0; ++entry) {
        if (_stricmp(at<char>(module, entry->Name), dll) != 0)
            continue;
        const auto* name = entry->OriginalFirstThunk != 0 ? at<IMAGE_THUNK_DATA>(module, entry->OriginalFirstThunk)
                                                          : nullptr;
        for (auto* slot = at<IMAGE_THUNK_DATA>(module, entry->FirstThunk); slot->u1.Function != 0; ++slot) {
            const bool match =
                name != nullptr
                    ? (ordinal != 0 ? IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) && IMAGE_ORDINAL(name->u1.Ordinal) == ordinal
                                    : !IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal) &&
                                          std::strcmp(at<IMAGE_IMPORT_BY_NAME>(module, name->u1.AddressOfData)->Name,
                                                      function) == 0)
                    : bound != nullptr && slot->u1.Function == reinterpret_cast<ULONG_PTR>(bound);
            if (name != nullptr)
                ++name;
            if (!match)
                continue;
            if (slot->u1.Function == reinterpret_cast<ULONG_PTR>(replacement))
                return true;
            // Unbound slots still hold an RVA; the loader would overwrite the hook.
            if (slot->u1.Function < nt->OptionalHeader.SizeOfImage)
                return false;
            // Execute kept: some system DLLs put their import table in a code page.
            DWORD protect = 0;
            if (!VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), PAGE_EXECUTE_READWRITE, &protect))
                return false;
            *original = reinterpret_cast<void*>(slot->u1.Function);
            slot->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slot->u1.Function, sizeof(slot->u1.Function), protect, &protect);
            return true;
        }
    }
    return false;
}

bool hook_vtable(void* object, int index, void* replacement, void** original)
{
    void** slot = *static_cast<void***>(object) + index;
    if (*slot == replacement)
        return true;
    if (*original != nullptr && *original != *slot)
        return false;
    DWORD protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &protect))
        return false;
    *original = *slot;
    *slot = replacement;
    VirtualProtect(slot, sizeof(void*), protect, &protect);
    return true;
}

bool jump(void* function, void* replacement)
{
    auto* code = static_cast<BYTE*>(function);
    DWORD protect = 0;
    if (!VirtualProtect(code, 5, PAGE_EXECUTE_READWRITE, &protect))
        return false;
    code[0] = 0xE9; // jmp rel32
    const auto distance = static_cast<BYTE*>(replacement) - (code + 5);
    std::memcpy(code + 1, &distance, 4);
    VirtualProtect(code, 5, protect, &protect);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
    return true;
}

void* detour(void* function, size_t prologue, void* replacement)
{
    if (prologue < 5 || prologue > 32)
        return nullptr;
    // The trampoline: the prologue, then a jump to the instruction after it.
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, prologue + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
        return nullptr;
    auto* code = static_cast<BYTE*>(function);
    std::memcpy(trampoline, code, prologue);
    trampoline[prologue] = 0xE9;
    const auto back = (code + prologue) - (trampoline + prologue + 5);
    std::memcpy(trampoline + prologue + 1, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), trampoline, prologue + 5);
    if (!jump(function, replacement)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    return trampoline;
}

void* detour_auto(void* function, void* replacement, size_t* stolen_out)
{
    auto* code = static_cast<BYTE*>(function);
    // Whole instructions, at least 5 bytes: how much of the start the jump will take.
    disasm::Instruction steps[8];
    size_t count = 0;
    size_t stolen = 0;
    while (stolen < 5) {
        if (count == 8 || !disasm::decode(code + stolen, 16, reinterpret_cast<std::uint32_t>(code + stolen), steps[count]))
            return nullptr;
        stolen += steps[count++].length;
    }
    // The trampoline: those instructions, branches re-aimed (a short jump or conditional becomes a
    // near one), then a jump to the rest of the function.
    auto* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr)
        return nullptr;
    size_t at = 0;
    const auto rel32 = [&](size_t from, std::uint32_t target) {
        const auto distance = static_cast<std::int32_t>(target - reinterpret_cast<std::uint32_t>(trampoline + from + 4));
        std::memcpy(trampoline + from, &distance, 4);
    };
    size_t offset = 0;
    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
        const disasm::Instruction& step = steps[i];
        const BYTE* bytes = code + offset;
        if (step.has_target) {
            const auto start = reinterpret_cast<std::uint32_t>(code);
            if (step.target >= start && step.target < start + stolen) {
                ok = false; // into the bytes being replaced
            } else if (bytes[0] == 0xE8 || bytes[0] == 0xE9) {
                trampoline[at++] = bytes[0];
                rel32(at, step.target);
                at += 4;
            } else if (bytes[0] == 0xEB) {
                trampoline[at++] = 0xE9;
                rel32(at, step.target);
                at += 4;
            } else if (bytes[0] >= 0x70 && bytes[0] <= 0x7F) {
                trampoline[at++] = 0x0F;
                trampoline[at++] = static_cast<BYTE>(0x80 | (bytes[0] & 0x0F));
                rel32(at, step.target);
                at += 4;
            } else if (bytes[0] == 0x0F && bytes[1] >= 0x80 && bytes[1] <= 0x8F) {
                trampoline[at++] = 0x0F;
                trampoline[at++] = bytes[1];
                rel32(at, step.target);
                at += 4;
            } else {
                ok = false; // loop, jecxz: no near form
            }
        } else {
            std::memcpy(trampoline + at, bytes, step.length);
            at += step.length;
        }
        offset += step.length;
    }
    if (!ok) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    trampoline[at++] = 0xE9;
    rel32(at, reinterpret_cast<std::uint32_t>(code + stolen));
    at += 4;
    FlushInstructionCache(GetCurrentProcess(), trampoline, at);
    if (!jump(function, replacement)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    if (stolen_out != nullptr)
        *stolen_out = stolen;
    return trampoline;
}

bool is_windows_module(HMODULE module)
{
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    return length == 0 || GetModuleFileNameW(module, path, MAX_PATH) == 0 ||
           (_wcsnicmp(path, windows, length) == 0 && path[length] == L'\\');
}

bool when_loaded(const wchar_t* name, void (*on_load)(HMODULE module))
{
    if (const HMODULE module = GetModuleHandleW(name)) {
        on_load(module);
        return true;
    }
    return watch(name, on_load);
}

bool for_each_module(void (*on_load)(HMODULE module))
{
    // Watch first, then list: a DLL loading in between is seen twice, which hook() tolerates.
    if (!watch(L"", on_load))
        return false;
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) {
        const DWORD count = bytes / sizeof(HMODULE) < 1024 ? bytes / sizeof(HMODULE) : 1024;
        for (DWORD i = 0; i < count; ++i)
            on_load(modules[i]);
    }
    return true;
}

} // namespace vanacore::imports
