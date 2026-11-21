#pragma once

#include <windows.h>

#include <string>

// What each plugin has allocated, for /vana stats and vana.plugins.loaded(). A plugin's DLL has its imports of the C
// runtime's heap (malloc, calloc, realloc, free, _recalloc, the _aligned_ ones, and operator new and delete where its
// runtime exports them), Windows' heaps (HeapAlloc, HeapReAlloc, HeapFree) and VirtualAlloc and VirtualFree pointed at
// counters of its own, from as it loads (before its DllMain, so its globals' allocations count). Each block it allocates
// is kept, its address and size, until it's freed, by it or by another plugin; what it frees that it didn't allocate goes
// through uncounted.
//
// Not counted: what another DLL allocates for it (its runtime's own, MSVCP140's streams; Direct3D's and the driver's for
// the textures and buffers it makes, which are mostly video memory), or what it allocated before it was counted (a plugin
// already in the process). VirtualAlloc counts the address space it reserves (in 64 KB steps, as Windows hands it out).
namespace vanacore::plugin_memory {

// From VanaCoreInstall: watches DLLs load, to count a plugin's from its start.
void install();

// Around a plugin's LoadLibrary, on the game's thread: `name` (its display name) is counted as its DLL loads, and loaded()
// counts `module` after if it wasn't (it was in the process already). loaded(nullptr, ...) after a load that failed.
void loading(const std::string& name, const std::string& path);
void loaded(HMODULE module, const std::string& name);

// After its FreeLibrary: what it still had allocated is let go of (the log says how much, what it leaked). A DLL that
// stays in the process (something else holds it) stays counted.
void unloaded(HMODULE module);

struct Usage {
    double heap_kb = 0.0;    // the C runtime's and Windows' heap blocks it has, not yet freed
    unsigned blocks = 0;
    double virtual_kb = 0.0; // address space it has reserved with VirtualAlloc
    double image_kb = 0.0;   // its DLL in memory
    double peak_kb = 0.0;    // the most heap_kb + virtual_kb has been
    int counters = 0;        // how many of its imports are counted (0: it imports none of them)
};
// False if `name` isn't counted (not loaded, or more plugins than there are counters have been loaded at once).
bool usage(const std::string& name, Usage& out);

} // namespace vanacore::plugin_memory
