#include "plugin_memory.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <utility>

#include "imports.hpp"
#include "log.hpp"

namespace vanacore::plugin_memory {
namespace {

constexpr int kSlots = 32;               // plugins counted at once
constexpr SIZE_T kGranularity = 0x10000; // VirtualAlloc hands out address space in these

// The functions counted
enum Fn {
    kMalloc,
    kCalloc,
    kRealloc,
    kRecalloc,
    kFree,
    kAlignedMalloc,
    kAlignedRealloc,
    kAlignedFree,
    kNew,
    kNewArray,
    kDelete,
    kDeleteArray,
    kHeapAlloc,
    kHeapReAlloc,
    kHeapFree,
    kVirtualAlloc,
    kVirtualFree,
    kFnCount
};

// ... by the names a DLL imports them by (operator new and delete from the runtimes that export them: MSVCR100 and the
// like; a newer one's are in the plugin, and call its malloc and free)
constexpr std::pair<const char*, Fn> kNames[] = {
    {"malloc", kMalloc},
    {"calloc", kCalloc},
    {"realloc", kRealloc},
    {"_recalloc", kRecalloc},
    {"free", kFree},
    {"_aligned_malloc", kAlignedMalloc},
    {"_aligned_realloc", kAlignedRealloc},
    {"_aligned_free", kAlignedFree},
    {"??2@YAPAXI@Z", kNew},
    {"??_U@YAPAXI@Z", kNewArray},
    {"??3@YAXPAX@Z", kDelete},
    {"??_V@YAXPAX@Z", kDeleteArray},
    {"HeapAlloc", kHeapAlloc},
    {"HeapReAlloc", kHeapReAlloc},
    {"HeapFree", kHeapFree},
    {"VirtualAlloc", kVirtualAlloc},
    {"VirtualFree", kVirtualFree},
};

// A plugin counted: its DLL, where its imports went, and what it has. Each slot has counters of its own (functions made
// for it), so a call knows whose it is without asking where it came from.
struct Slot {
    HMODULE module = nullptr;       // nullptr: free
    std::string name;               // its display name; "" once it's unloaded and its DLL stayed (counted, not shown)
    void* original[kFnCount] = {};  // kept after it goes: a pointer to a counter it handed out still has somewhere to go
    bool hooked[kFnCount] = {};
    int counters = 0;
    DWORD image = 0;
    long long heap = 0, virtual_bytes = 0, peak = 0; // under g_lock
    unsigned blocks = 0;
};
Slot g_slots[kSlots];
int g_next = 0; // where a free slot is looked for first: one is used again only after the others

// Every counted block, by address
struct Block {
    SIZE_T size = 0;
    int slot = 0;
    bool is_virtual = false; // VirtualAlloc's address space, not a heap block
};
SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<const void*, Block>* g_blocks = nullptr; // never freed: plugins free as the process ends

// The plugin loading now (loading() to loaded()): its DLL's file name, and its name
std::string g_pending_file;
std::string g_pending_name;
DWORD g_pending_thread = 0;

// Under g_lock
void add(const Block& block)
{
    Slot& s = g_slots[block.slot];
    if (block.is_virtual) {
        s.virtual_bytes += static_cast<long long>(block.size);
    } else {
        s.heap += static_cast<long long>(block.size);
        ++s.blocks;
    }
    s.peak = std::max(s.peak, s.heap + s.virtual_bytes);
}

void take(const Block& block)
{
    Slot& s = g_slots[block.slot];
    if (block.is_virtual) {
        s.virtual_bytes -= static_cast<long long>(block.size);
    } else {
        s.heap -= static_cast<long long>(block.size);
        --s.blocks;
    }
}

void counted(int slot, const void* p, SIZE_T size, bool is_virtual)
{
    if (p == nullptr || g_blocks == nullptr)
        return;
    const Block block{size, slot, is_virtual};
    AcquireSRWLockExclusive(&g_lock);
    try {
        const auto [it, fresh] = g_blocks->try_emplace(p, block);
        if (!fresh) { // freed where no counter saw it: the address is this block's now
            take(it->second);
            it->second = block;
        }
        add(block);
    } catch (...) {
        // no memory for its record: it goes uncounted
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Takes `p` out of the record before it's freed (another thread can be handed its address the moment it is), `was` what it
// was, to put back if the free fails. False: it wasn't counted (as that kind).
bool forget(const void* p, bool is_virtual, Block& was)
{
    if (p == nullptr || g_blocks == nullptr)
        return false;
    bool found = false;
    AcquireSRWLockExclusive(&g_lock);
    const auto it = g_blocks->find(p);
    if (it != g_blocks->end() && it->second.is_virtual == is_virtual) {
        was = it->second;
        take(was);
        g_blocks->erase(it);
        found = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

template <typename Function>
Function original(int slot, Fn fn)
{
    return reinterpret_cast<Function>(g_slots[slot].original[fn]);
}

using Malloc = void*(__cdecl*)(size_t);
using Calloc = void*(__cdecl*)(size_t, size_t);
using Realloc = void*(__cdecl*)(void*, size_t);
using Recalloc = void*(__cdecl*)(void*, size_t, size_t);
using Free = void(__cdecl*)(void*);
using AlignedMalloc = void*(__cdecl*)(size_t, size_t);
using AlignedRealloc = void*(__cdecl*)(void*, size_t, size_t);

// ---- each slot's counters ------------------------------------------------------------------------------------------

template <int S, Fn F>
void* __cdecl allocate(size_t size)
{
    void* p = original<Malloc>(S, F)(size);
    counted(S, p, size, false);
    return p;
}

template <int S, Fn F>
void __cdecl release(void* p)
{
    Block was;
    forget(p, false, was);
    original<Free>(S, F)(p);
}

template <int S>
void* __cdecl allocate_zeroed(size_t count, size_t size)
{
    void* p = original<Calloc>(S, kCalloc)(count, size);
    counted(S, p, count * size, false);
    return p;
}

template <int S>
void* __cdecl allocate_aligned(size_t size, size_t alignment)
{
    void* p = original<AlignedMalloc>(S, kAlignedMalloc)(size, alignment);
    counted(S, p, size, false);
    return p;
}

// A block for another: the new one counted, the old one kept when it fails (freed when it was asked for 0 bytes)
template <typename Call>
void* reallocate(int slot, void* p, size_t size, Call call)
{
    Block was;
    const bool had = forget(p, false, was);
    void* moved = call();
    if (moved != nullptr)
        counted(slot, moved, size, false);
    else if (had && size != 0)
        counted(was.slot, p, was.size, false);
    return moved;
}

template <int S>
void* __cdecl reallocate_plain(void* p, size_t size)
{
    return reallocate(S, p, size, [&] { return original<Realloc>(S, kRealloc)(p, size); });
}

template <int S>
void* __cdecl reallocate_zeroed(void* p, size_t count, size_t size)
{
    return reallocate(S, p, count * size, [&] { return original<Recalloc>(S, kRecalloc)(p, count, size); });
}

template <int S>
void* __cdecl reallocate_aligned(void* p, size_t size, size_t alignment)
{
    return reallocate(S, p, size, [&] { return original<AlignedRealloc>(S, kAlignedRealloc)(p, size, alignment); });
}

template <int S>
LPVOID WINAPI heap_allocate(HANDLE heap, DWORD flags, SIZE_T size)
{
    LPVOID p = original<decltype(&HeapAlloc)>(S, kHeapAlloc)(heap, flags, size);
    counted(S, p, size, false);
    return p;
}

template <int S>
LPVOID WINAPI heap_reallocate(HANDLE heap, DWORD flags, LPVOID p, SIZE_T size)
{
    Block was;
    const bool had = forget(p, false, was);
    LPVOID moved = original<decltype(&HeapReAlloc)>(S, kHeapReAlloc)(heap, flags, p, size);
    const DWORD error = GetLastError();
    if (moved != nullptr)
        counted(S, moved, size, false);
    else if (had)
        counted(was.slot, p, was.size, false);
    SetLastError(error);
    return moved;
}

template <int S>
BOOL WINAPI heap_release(HANDLE heap, DWORD flags, LPVOID p)
{
    Block was;
    const bool had = forget(p, false, was);
    const BOOL freed = original<decltype(&HeapFree)>(S, kHeapFree)(heap, flags, p);
    if (!freed && had) {
        const DWORD error = GetLastError();
        counted(was.slot, p, was.size, false);
        SetLastError(error);
    }
    return freed;
}

template <int S>
LPVOID WINAPI virtual_allocate(LPVOID at, SIZE_T size, DWORD type, DWORD protect)
{
    LPVOID p = original<decltype(&VirtualAlloc)>(S, kVirtualAlloc)(at, size, type, protect);
    // address space it reserved: not a commit in a reservation it has, a reset or the like
    if (p != nullptr && (at == nullptr || (type & MEM_RESERVE) != 0)) {
        const DWORD error = GetLastError();
        counted(S, p, (size + kGranularity - 1) & ~(kGranularity - 1), true);
        SetLastError(error);
    }
    return p;
}

template <int S>
BOOL WINAPI virtual_release(LPVOID at, SIZE_T size, DWORD type)
{
    Block was;
    const bool had = (type & MEM_RELEASE) != 0 && forget(at, true, was);
    const BOOL freed = original<decltype(&VirtualFree)>(S, kVirtualFree)(at, size, type);
    if (!freed && had) {
        const DWORD error = GetLastError();
        counted(was.slot, at, was.size, true);
        SetLastError(error);
    }
    return freed;
}

using Table = std::array<void*, kFnCount>;

template <int S>
Table table()
{
    Table t{};
    t[kMalloc] = reinterpret_cast<void*>(&allocate<S, kMalloc>);
    t[kCalloc] = reinterpret_cast<void*>(&allocate_zeroed<S>);
    t[kRealloc] = reinterpret_cast<void*>(&reallocate_plain<S>);
    t[kRecalloc] = reinterpret_cast<void*>(&reallocate_zeroed<S>);
    t[kFree] = reinterpret_cast<void*>(&release<S, kFree>);
    t[kAlignedMalloc] = reinterpret_cast<void*>(&allocate_aligned<S>);
    t[kAlignedRealloc] = reinterpret_cast<void*>(&reallocate_aligned<S>);
    t[kAlignedFree] = reinterpret_cast<void*>(&release<S, kAlignedFree>);
    t[kNew] = reinterpret_cast<void*>(&allocate<S, kNew>);
    t[kNewArray] = reinterpret_cast<void*>(&allocate<S, kNewArray>);
    t[kDelete] = reinterpret_cast<void*>(&release<S, kDelete>);
    t[kDeleteArray] = reinterpret_cast<void*>(&release<S, kDeleteArray>);
    t[kHeapAlloc] = reinterpret_cast<void*>(&heap_allocate<S>);
    t[kHeapReAlloc] = reinterpret_cast<void*>(&heap_reallocate<S>);
    t[kHeapFree] = reinterpret_cast<void*>(&heap_release<S>);
    t[kVirtualAlloc] = reinterpret_cast<void*>(&virtual_allocate<S>);
    t[kVirtualFree] = reinterpret_cast<void*>(&virtual_release<S>);
    return t;
}

template <int... S>
std::array<Table, kSlots> tables(std::integer_sequence<int, S...>)
{
    return {table<S>()...};
}

const std::array<Table, kSlots>& counters()
{
    static const std::array<Table, kSlots> all = tables(std::make_integer_sequence<int, kSlots>{});
    return all;
}

// ---- a plugin's imports --------------------------------------------------------------------------------------------

template <typename T>
T* at(HMODULE module, DWORD rva)
{
    return reinterpret_cast<T*>(reinterpret_cast<BYTE*>(module) + rva);
}

Fn which(const char* name)
{
    for (const auto& [import, fn] : kNames)
        if (std::strcmp(import, name) == 0)
            return fn;
    return kFnCount;
}

// Points slot `index`'s DLL's imports of the counted functions at the slot's counters. One imported twice (from two
// runtimes) is counted through the first.
void patch(int index)
{
    Slot& s = g_slots[index];
    const HMODULE module = s.module;
    const auto* nt = at<IMAGE_NT_HEADERS>(module, at<IMAGE_DOS_HEADER>(module, 0)->e_lfanew);
    s.image = nt->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0)
        return;
    const Table& mine = counters()[index];
    for (const auto* entry = at<IMAGE_IMPORT_DESCRIPTOR>(module, imports.VirtualAddress); entry->Name != 0; ++entry) {
        if (entry->OriginalFirstThunk == 0)
            continue; // no name table to tell its imports by
        const auto* name = at<IMAGE_THUNK_DATA>(module, entry->OriginalFirstThunk);
        for (auto* iat = at<IMAGE_THUNK_DATA>(module, entry->FirstThunk); iat->u1.Function != 0; ++iat, ++name) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal))
                continue;
            const Fn fn = which(at<IMAGE_IMPORT_BY_NAME>(module, name->u1.AddressOfData)->Name);
            if (fn == kFnCount || s.hooked[fn] || iat->u1.Function < nt->OptionalHeader.SizeOfImage) // (not bound yet)
                continue;
            DWORD protect = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, PAGE_EXECUTE_READWRITE, &protect))
                continue;
            s.original[fn] = reinterpret_cast<void*>(iat->u1.Function); // before the counter can be called
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(mine[fn]);
            VirtualProtect(&iat->u1.Function, sizeof iat->u1.Function, protect, &protect);
            s.hooked[fn] = true;
            ++s.counters;
        }
    }
}

int find(HMODULE module)
{
    for (int i = 0; i < kSlots; ++i)
        if (g_slots[i].module == module)
            return i;
    return -1;
}

// Counts `module` as plugin `name` from now. Under the loader lock when it comes as its DLL loads.
void start(HMODULE module, const std::string& name)
{
    if (const int index = find(module); index >= 0) {
        g_slots[index].name = name; // its DLL stayed after an unload, and it's loaded again
        return;
    }
    int index = -1;
    for (int i = 0; i < kSlots && index < 0; ++i)
        if (g_slots[(g_next + i) % kSlots].module == nullptr)
            index = (g_next + i) % kSlots;
    if (index < 0) {
        log::write(L"Plugins: %hs's memory isn't counted: %d plugins are counted already", name.c_str(), kSlots);
        return;
    }
    g_next = (index + 1) % kSlots;
    Slot& s = g_slots[index];
    AcquireSRWLockExclusive(&g_lock);
    s.heap = s.virtual_bytes = s.peak = 0;
    s.blocks = 0;
    ReleaseSRWLockExclusive(&g_lock);
    std::fill(std::begin(s.hooked), std::end(s.hooked), false);
    s.counters = 0;
    s.module = module;
    s.name = name;
    patch(index);
}

const char* file_part(const char* path)
{
    const char* slash = std::strrchr(path, '\\');
    const char* other = std::strrchr(path, '/');
    if (other != nullptr && (slash == nullptr || other > slash))
        slash = other;
    return slash != nullptr ? slash + 1 : path;
}

// Each DLL as it loads (imports.hpp): the plugin being loaded is counted from before its DllMain
void on_module(HMODULE module)
{
    if (g_pending_file.empty() || GetCurrentThreadId() != g_pending_thread)
        return;
    char path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameA(module, path, MAX_PATH);
    if (length != 0 && length < MAX_PATH && _stricmp(file_part(path), g_pending_file.c_str()) == 0)
        start(module, g_pending_name);
}

} // namespace

void install()
{
    if (g_blocks != nullptr)
        return;
    g_blocks = new std::unordered_map<const void*, Block>;
    counters();
    if (!imports::for_each_module(&on_module))
        log::write(L"Plugins: can't watch DLLs load: a plugin's memory is counted from when its DLL has loaded");
}

void loading(const std::string& name, const std::string& path)
{
    g_pending_file = file_part(path.c_str());
    g_pending_name = name;
    g_pending_thread = GetCurrentThreadId();
}

void loaded(HMODULE module, const std::string& name)
{
    g_pending_file.clear();
    g_pending_name.clear();
    if (module != nullptr && g_blocks != nullptr)
        start(module, name);
}

void unloaded(HMODULE module)
{
    const int index = find(module);
    if (index < 0)
        return;
    Slot& s = g_slots[index];
    HMODULE still = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(module), &still) &&
        still == module) {
        // something else holds its DLL: its imports still come to the counters, so the slot stays its
        log::write(L"Plugins: %hs's DLL stayed in the process after it unloaded: its memory stays counted", s.name.c_str());
        s.name.clear();
        return;
    }
    long long heap = 0, virtual_bytes = 0;
    unsigned blocks = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (auto it = g_blocks->begin(); it != g_blocks->end();) {
        if (it->second.slot == index)
            it = g_blocks->erase(it);
        else
            ++it;
    }
    heap = s.heap, virtual_bytes = s.virtual_bytes, blocks = s.blocks;
    s.heap = s.virtual_bytes = s.peak = 0;
    s.blocks = 0;
    ReleaseSRWLockExclusive(&g_lock);
    if (heap > 0 || virtual_bytes > 0)
        log::write(L"Plugins: %hs left %.1f KB in %u blocks allocated as it unloaded (and %.0f KB of address space reserved)",
                   s.name.c_str(), static_cast<double>(heap) / 1024.0, blocks, static_cast<double>(virtual_bytes) / 1024.0);
    s.module = nullptr;
    s.name.clear();
}

bool usage(const std::string& name, Usage& out)
{
    for (const Slot& s : g_slots) {
        if (s.module == nullptr || s.name.empty() || _stricmp(s.name.c_str(), name.c_str()) != 0)
            continue;
        AcquireSRWLockShared(&g_lock);
        out.heap_kb = static_cast<double>(s.heap) / 1024.0;
        out.blocks = s.blocks;
        out.virtual_kb = static_cast<double>(s.virtual_bytes) / 1024.0;
        out.peak_kb = static_cast<double>(s.peak) / 1024.0;
        ReleaseSRWLockShared(&g_lock);
        out.image_kb = static_cast<double>(s.image) / 1024.0;
        out.counters = s.counters;
        return true;
    }
    return false;
}

} // namespace vanacore::plugin_memory
