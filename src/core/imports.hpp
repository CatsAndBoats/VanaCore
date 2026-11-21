#pragma once

#include <windows.h>

namespace vanacore::imports {

// Points `module`'s import of dll!function at `replacement`, storing the previous target in
// `original` first. Returns false (leaving `original` alone) if the module doesn't import it or
// the loader hasn't bound its imports yet. Hooking the same import twice is a no-op. `function` "#11" names an import by its
// ordinal (FFXiMain.dll's DirectSoundCreate8 in DSOUND.dll).
bool hook(HMODULE module, const char* dll, const char* function, void* replacement, void** original);

template <typename Function>
bool hook(HMODULE module, const char* dll, const char* function, Function replacement, Function& original)
{
    return hook(module, dll, function, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
}

// Points vtable slot `index` of COM object `object` at `replacement`, storing the previous entry in
// `original` first. The table is shared by every object of the class. Returns false if it can't
// be written, or if `original` already holds a different class's entry (one original per hook).
bool hook_vtable(void* object, int index, void* replacement, void** original);

template <typename Function>
bool hook_vtable(void* object, int index, Function replacement, Function& original)
{
    return hook_vtable(object, index, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
}

// Overwrites the first 5 bytes of `function` with a jump to `replacement`, which then stands in
// for it completely (the original isn't callable afterwards). Only patch code no other thread can
// be running.
bool jump(void* function, void* replacement);

// Sends every call of `function` to `replacement`, and returns a trampoline that runs the original:
// its first `prologue` bytes, then a jump back into it. The prologue must be whole instructions,
// at least 5 bytes, that run the same from anywhere (no relative jumps or calls); check the bytes
// before calling. nullptr if it can't be done. Only patch code no other thread can be running.
void* detour(void* function, size_t prologue, void* replacement);

template <typename Function>
bool detour(Function function, size_t prologue, Function replacement, Function& original)
{
    void* trampoline = detour(reinterpret_cast<void*>(function), prologue, reinterpret_cast<void*>(replacement));
    if (trampoline != nullptr)
        original = reinterpret_cast<Function>(trampoline);
    return trampoline != nullptr;
}

// detour() for code VanaCore hasn't seen: takes whole instructions to cover 5 bytes (decoded with
// disasm), and re-aims any relative jump or call among them (another program's hook, or a short
// jump, at the start of a system function). nullptr if it can't decode them or a branch leads
// into the bytes it takes. The function is left alone then.
void* detour_auto(void* function, void* replacement, size_t* stolen = nullptr);

template <typename Function>
bool detour_auto(Function function, Function replacement, Function& original)
{
    void* trampoline = detour_auto(reinterpret_cast<void*>(function), reinterpret_cast<void*>(replacement));
    if (trampoline != nullptr)
        original = reinterpret_cast<Function>(trampoline);
    return trampoline != nullptr;
}

// True for modules under the Windows folder (system DLLs), which VanaCore's import hooks skip.
bool is_windows_module(HMODULE module);

// Calls on_load(module) once the DLL with file name `name` (e.g. L"FFXiMain.dll") is in the
// process: now if it already is, otherwise as it loads, after its imports are bound and before
// its DllMain runs (under the loader lock). Call from VanaCoreInstall.
bool when_loaded(const wchar_t* name, void (*on_load)(HMODULE module));

// Calls on_load(module) for every module already in the process, then for every DLL as it loads
// (as when_loaded does). A module can be passed more than once. Call from VanaCoreInstall.
bool for_each_module(void (*on_load)(HMODULE module));

} // namespace vanacore::imports
