#include "sampler.hpp"

#include <windows.h>

#include <dbghelp.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.hpp"

#pragma comment(lib, "winmm.lib")   // timeBeginPeriod
#pragma comment(lib, "dbghelp.lib") // UnDecorateSymbolName

namespace vanacore::sampler {
namespace {

constexpr unsigned kRing = 8192;      // samples kept: 8 s at a millisecond each
constexpr int kDepth = 40;            // calls kept of a sample's stack, innermost first
constexpr SIZE_T kStackBytes = 8192;  // of the stack, from the stack pointer up, looked through for calls
constexpr std::uint32_t kFfxiBase = 0x10000000;

struct Sample {
    std::int64_t time = 0; // when, by QueryPerformanceCounter
    std::uint32_t eip = 0;
    std::uint32_t depth = 0;
    std::uint32_t calls[kDepth] = {}; // return addresses in FFXiMain.dll, innermost first
};

std::wstring g_data_dir;
std::atomic<bool> g_run{false};
HANDLE g_thread = nullptr;
DWORD g_target_id = 0;

Sample g_ring[kRing];
std::atomic<unsigned> g_head{0}; // samples written, ever

// FFXiMain.dll: where it is, its code, its function starts (RVAs, sorted) and the names data\ffximain_labels.txt gives some
struct Label {
    std::string name;
    std::string group;
};
std::uint32_t g_base = 0;
std::vector<std::pair<std::uint32_t, std::uint32_t>> g_code; // executable sections (addresses)
bool g_tables_loaded = false;
struct Function {
    std::uint32_t start = 0; // an RVA
    std::uint32_t end = 0;   // the first after it; 0: not known, it runs to the next start
};
std::vector<Function> g_functions; // sorted by start
std::unordered_map<std::uint32_t, Label> g_labels;

// VanaCore.Core.dll's own functions, from VanaCore.Core.map (the linker's, beside it when it's there): a sample in the
// DLL is put in the function it's in, so what VanaCore does with a frame (Direct3D 8 turned into Direct3D 12) shows
// by name. [name, RVA], sorted; the names as the linker has them.
struct Symbol {
    std::uint32_t rva = 0;
    std::string name;
};
std::wstring g_install_dir;
std::vector<Symbol> g_own;
std::uintptr_t g_own_base = 0;
std::uintptr_t g_own_end = 0;
bool g_own_loaded = false;

std::string narrow(const std::wstring& text)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string trim(std::string text)
{
    const auto space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(static_cast<unsigned char>(text.back())))
        text.pop_back();
    size_t start = 0;
    while (start < text.size() && space(static_cast<unsigned char>(text[start])))
        ++start;
    return text.substr(start);
}

// ---- finding the calls on a stack -----------------------------------------------------------------------------------

bool in_code(std::uint32_t address)
{
    for (const auto& [low, high] : g_code)
        if (address >= low + 8 && address < high)
            return true;
    return false;
}

// The function an address (not an RVA) is in, or null: the last that starts at or before it, if it reaches it
const Function* find_function(std::uint32_t address)
{
    const std::uint32_t rva = address - g_base;
    auto it = std::upper_bound(g_functions.begin(), g_functions.end(), rva, [](std::uint32_t value, const Function& f) { return value < f.start; });
    if (it == g_functions.begin())
        return nullptr;
    --it;
    if (it->end != 0 ? rva >= it->end : rva - it->start >= 0x4000)
        return nullptr;
    return &*it;
}

bool is_function_start(std::uint32_t address)
{
    const std::uint32_t rva = address - g_base;
    const auto it = std::lower_bound(g_functions.begin(), g_functions.end(), rva, [](const Function& f, std::uint32_t value) { return f.start < value; });
    return it != g_functions.end() && it->start == rva;
}

// A call: where it goes when that's written in it (direct), and whether it looks like one at all
struct Call {
    bool plausible = false;
    bool direct = false;
    std::uint32_t target = 0;
};

// A value off the stack that's the return address of a call: it's in the game's code, with a call instruction just
// before it. A direct call (E8) must go where a function starts, which weeds out most of what only looks like one.
Call inspect(std::uint32_t address)
{
    Call call;
    if (!in_code(address))
        return call;
    const auto* b = reinterpret_cast<const std::uint8_t*>(address);
    if (b[-5] == 0xE8) {
        const std::int32_t relative = *reinterpret_cast<const std::int32_t*>(b - 4);
        call.direct = true;
        call.target = address + static_cast<std::uint32_t>(relative);
        call.plausible = g_functions.empty() ? in_code(call.target) : is_function_start(call.target);
        return call;
    }
    call.plausible = (b[-2] == 0xFF && ((b[-1] & 0xF8) == 0xD0 || (b[-1] & 0xF8) == 0x10)) || // call reg, call [reg]
                     (b[-3] == 0xFF && ((b[-2] & 0xF8) == 0x50 || b[-2] == 0x14)) ||         // call [reg+disp8], call [sib]
                     (b[-6] == 0xFF && ((b[-5] & 0xF8) == 0x90 || b[-5] == 0x15));           // call [reg+disp32], call [abs]
    return call;
}

// Where a call to `target` ends up, through the jumps of a thunk
std::uint32_t follow(std::uint32_t target)
{
    for (int hop = 0; hop < 4 && in_code(target); ++hop) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(target);
        if (b[0] != 0xE9)
            break;
        target = target + 5 + *reinterpret_cast<const std::uint32_t*>(b + 1);
    }
    return target;
}

// ---- the sampling thread ------------------------------------------------------------------------------------------

DWORD WINAPI sample_thread(LPVOID)
{
    timeBeginPeriod(1); // a Sleep(1) of about a millisecond
    HANDLE target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_target_id);
    const HANDLE self = GetCurrentProcess();
    alignas(4) std::uint8_t stack[kStackBytes];
    while (g_run.load(std::memory_order_relaxed) && target != nullptr) {
        Sleep(1);
        // While the thread is stopped (it may be inside the heap, a lock, anything) nothing here allocates, locks or
        // logs: its context and the top of its stack are copied, and it goes on.
        if (SuspendThread(target) == static_cast<DWORD>(-1))
            continue;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        SIZE_T got = 0;
        const bool ok = GetThreadContext(target, &context) != FALSE;
        if (ok)
            ReadProcessMemory(self, reinterpret_cast<LPCVOID>(context.Esp), stack, kStackBytes, &got);
        ResumeThread(target);
        if (!ok)
            continue;
        LARGE_INTEGER when;
        QueryPerformanceCounter(&when);
        const unsigned at = g_head.load(std::memory_order_relaxed);
        Sample& sample = g_ring[at % kRing];
        sample.time = when.QuadPart;
        sample.eip = static_cast<std::uint32_t>(context.Eip);
        sample.depth = 0;
        // Up the stack from the stack pointer, each return address that fits: a direct call must go to the function
        // the frame above it is in (through a thunk's jump), which is what weeds out the stale ones; one through a
        // register or a table (a virtual call) can't be checked, so it just can't be from the function it's already in.
        const Function* here = in_code(sample.eip) ? find_function(sample.eip) : nullptr;
        std::uint32_t current = here != nullptr ? g_base + here->start : 0;
        for (SIZE_T i = 0; i + 4 <= got && sample.depth < kDepth; i += 4) {
            std::uint32_t value;
            std::memcpy(&value, stack + i, sizeof value);
            const Call call = inspect(value);
            if (!call.plausible)
                continue;
            if (call.direct && current != 0 && call.target != current && follow(call.target) != current)
                continue;
            const Function* in = find_function(value);
            if (!call.direct && current != 0 && in != nullptr && g_base + in->start == current)
                continue;
            if (sample.depth > 0 && sample.calls[sample.depth - 1] == value)
                continue;
            sample.calls[sample.depth++] = value;
            current = in != nullptr ? g_base + in->start : 0;
        }
        g_head.store(at + 1, std::memory_order_release);
    }
    if (target != nullptr)
        CloseHandle(target);
    timeEndPeriod(1);
    return 0;
}

// ---- the tables ---------------------------------------------------------------------------------------------------------

// The functions in the linker's map: its "Publics by Value" lines (segment:offset, name, Rva+Base, f for a function, object)
void load_own_map()
{
    if (g_own_loaded)
        return;
    g_own_loaded = true;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&load_own_map), &self);
    if (self == nullptr)
        return;
    g_own_base = reinterpret_cast<std::uintptr_t>(self);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(self) + dos->e_lfanew);
    g_own_end = g_own_base + nt->OptionalHeader.SizeOfImage;
    std::ifstream file(g_install_dir + L"VanaCore.Core.map");
    if (!file)
        return;
    std::uint32_t preferred = 0;
    std::string line;
    bool publics = false;
    while (std::getline(file, line)) {
        if (line.find("Preferred load address is") != std::string::npos) {
            const size_t at = line.find("is ");
            preferred = static_cast<std::uint32_t>(std::strtoul(line.c_str() + at + 3, nullptr, 16));
            continue;
        }
        if (line.find("Publics by Value") != std::string::npos) {
            publics = true;
            continue;
        }
        if (!publics || line.size() < 20 || line.compare(1, 5, "0001:") != 0)
            continue; // code is segment 1
        const size_t name_start = line.find_first_not_of(' ', 15);
        if (name_start == std::string::npos)
            continue;
        const size_t name_end = line.find(' ', name_start);
        if (name_end == std::string::npos)
            continue;
        const size_t value_start = line.find_first_not_of(' ', name_end);
        if (value_start == std::string::npos)
            continue;
        const unsigned long value = std::strtoul(line.c_str() + value_start, nullptr, 16);
        if (value < preferred)
            continue;
        Symbol symbol;
        symbol.rva = static_cast<std::uint32_t>(value - preferred);
        symbol.name = line.substr(name_start, name_end - name_start);
        g_own.push_back(std::move(symbol));
    }
    std::sort(g_own.begin(), g_own.end(), [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
    log::write(L"Sampler: %zu functions of VanaCore.Core.dll from its map", g_own.size());
}

// The function an address in VanaCore.Core.dll is in (an index into g_own), or -1
int own_function(std::uintptr_t address)
{
    if (g_own.empty() || address < g_own_base || address >= g_own_end)
        return -1;
    const auto rva = static_cast<std::uint32_t>(address - g_own_base);
    auto it = std::upper_bound(g_own.begin(), g_own.end(), rva, [](std::uint32_t value, const Symbol& s) { return value < s.rva; });
    if (it == g_own.begin())
        return -1;
    --it;
    return rva - it->rva < 0x4000 ? static_cast<int>(it - g_own.begin()) : -1;
}

std::string own_name(int index)
{
    const std::string& decorated = g_own[static_cast<size_t>(index)].name;
    char text[512];
    if (decorated.empty() || UnDecorateSymbolName(decorated.c_str(), text, sizeof text, UNDNAME_NAME_ONLY) == 0)
        return "[VanaCore] " + decorated;
    std::string name = text;
    if (name.size() > 1 && name[0] == '_' && decorated[0] != '?')
        name.erase(0, 1);
    return "[VanaCore] " + name;
}

void load_tables()
{
    if (g_tables_loaded)
        return;
    g_tables_loaded = true;
    load_own_map();
    if (g_data_dir.empty())
        return;
    {
        std::ifstream file(g_data_dir + L"ffximain_functions.bin", std::ios::binary | std::ios::ate);
        if (file) {
            const std::streamsize size = file.tellg();
            file.seekg(0);
            g_functions.resize(static_cast<size_t>(size) / sizeof(Function));
            file.read(reinterpret_cast<char*>(g_functions.data()), static_cast<std::streamsize>(g_functions.size() * sizeof(Function)));
            std::sort(g_functions.begin(), g_functions.end(), [](const Function& a, const Function& b) { return a.start < b.start; });
        }
    }
    {
        std::ifstream file(g_data_dir + L"ffximain_labels.txt");
        std::string line;
        while (std::getline(file, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos)
                line.resize(hash);
            line = trim(line);
            if (line.empty())
                continue;
            char* end = nullptr;
            const unsigned long va = std::strtoul(line.c_str(), &end, 16);
            if (end == line.c_str() || va < kFfxiBase)
                continue;
            std::string rest = trim(end);
            Label label;
            const size_t bar = rest.find('|');
            if (bar != std::string::npos) {
                label.group = trim(rest.substr(bar + 1));
                rest.resize(bar);
            }
            label.name = trim(rest);
            if (!label.name.empty())
                g_labels[static_cast<std::uint32_t>(va - kFfxiBase)] = label;
        }
    }
    log::write(L"Sampler: %zu function starts, %zu labels", g_functions.size(), g_labels.size());
}

// The function an FFXiMain.dll RVA is in: its start, from the table, else the 256-byte place it's in (known: false)
std::uint32_t function_start(std::uint32_t rva, bool& known)
{
    known = false;
    if (const Function* function = find_function(g_base + rva)) {
        known = true;
        return function->start;
    }
    return rva & ~0xFFu;
}

// A path's element: an FFXiMain.dll function (its start's RVA), a place in it (that, and kPlace), or the module a
// sample was in outside it (kModule and its number)
constexpr std::uint64_t kPlace = 1ull << 33;
constexpr std::uint64_t kModule = 1ull << 34;
constexpr std::uint64_t kOwn = 1ull << 35; // a function of VanaCore.Core.dll (kOwn and its index in g_own)

std::string hex_name(const char* prefix, unsigned value)
{
    char text[40];
    std::snprintf(text, sizeof text, "%s%08X", prefix, value);
    return text;
}

std::string function_name(std::uint32_t start)
{
    const auto label = g_labels.find(start);
    const std::string fun = hex_name("FUN_", kFfxiBase + start);
    return label != g_labels.end() ? label->second.name + " (" + fun + ")" : fun;
}

// ---- the call tree ------------------------------------------------------------------------------------------------------

struct TrieNode {
    std::uint64_t key = 0;
    unsigned samples = 0;
    std::map<std::uint64_t, int> kids;
};

void build_tree(const std::vector<TrieNode>& trie, int index, Share& out, const std::function<std::string(std::uint64_t)>& name,
                double total, unsigned min_samples, int depth)
{
    std::vector<int> kids;
    for (const auto& [key, child] : trie[static_cast<size_t>(index)].kids)
        if (trie[static_cast<size_t>(child)].samples >= min_samples)
            kids.push_back(child);
    std::sort(kids.begin(), kids.end(),
              [&](int a, int b) { return trie[static_cast<size_t>(a)].samples > trie[static_cast<size_t>(b)].samples; });
    if (kids.size() > 14)
        kids.resize(14);
    for (const int child : kids) {
        Share share;
        share.name = name(trie[static_cast<size_t>(child)].key);
        share.samples = trie[static_cast<size_t>(child)].samples;
        share.share = static_cast<double>(share.samples) / total;
        if (depth < 28)
            build_tree(trie, child, share, name, total, min_samples, depth + 1);
        out.children.push_back(std::move(share));
    }
}

// ---- the recording ------------------------------------------------------------------------------------------------------

// The modules samples were in: by handle, with a name and the range of its image; each with a number, its place in
// g_module_names
struct Module {
    std::string name;
    std::uintptr_t low = 0, high = 0;
    int id = 0;
};
std::map<HMODULE, Module> g_modules;
std::vector<std::string> g_module_names;

const Module& module_of(std::uintptr_t address)
{
    for (const auto& [handle, entry] : g_modules)
        if (handle != nullptr && address >= entry.low && address < entry.high)
            return entry;
    HMODULE handle = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(address), &handle);
    auto found = g_modules.find(handle);
    if (found == g_modules.end()) {
        Module entry;
        wchar_t path[MAX_PATH] = {};
        if (handle != nullptr && GetModuleFileNameW(handle, path, MAX_PATH) != 0) {
            const wchar_t* name = wcsrchr(path, L'\\');
            entry.name = narrow(name != nullptr ? name + 1 : path);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(handle);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(handle) + dos->e_lfanew);
            entry.low = reinterpret_cast<std::uintptr_t>(handle);
            entry.high = entry.low + nt->OptionalHeader.SizeOfImage;
        } else {
            entry.name = "(code outside any module)";
        }
        entry.id = static_cast<int>(g_module_names.size());
        g_module_names.push_back(entry.name);
        found = g_modules.emplace(handle, std::move(entry)).first;
    }
    return found->second;
}

// What a sample says: the path to it (the calls from the outermost in, then where the thread was), the module it was
// in, the function it was in itself, and its group
struct Seen {
    std::vector<std::uint64_t> path;
    int module = 0;
    bool has_hot = false;
    std::uint64_t hot = 0;
    const std::string* group = nullptr;
};

void describe(const Sample& sample, Seen& seen)
{
    const std::uintptr_t ffxi = g_base;
    const Module& where = module_of(sample.eip);
    seen.module = where.id;
    seen.has_hot = false;
    seen.group = nullptr;
    seen.path.clear();
    const auto add = [&](std::uint64_t key) {
        if (seen.path.empty() || seen.path.back() != key)
            seen.path.push_back(key);
    };
    const auto function_key = [&](std::uint32_t address) -> std::uint64_t {
        bool known = false;
        const std::uint32_t start = function_start(address - g_base, known);
        return known ? start : (start | kPlace);
    };
    for (int c = static_cast<int>(sample.depth) - 1; c >= 0; --c)
        add(function_key(sample.calls[c]));
    if (ffxi != 0 && where.low == ffxi) {
        const std::uint64_t leaf = function_key(sample.eip);
        add(leaf);
        if ((leaf & kPlace) == 0) {
            seen.has_hot = true;
            seen.hot = leaf;
        }
    } else if (const int own = own_function(sample.eip); own >= 0) {
        add(kOwn | static_cast<std::uint64_t>(own));
        seen.has_hot = true;
        seen.hot = kOwn | static_cast<std::uint64_t>(own);
    } else {
        add(kModule | static_cast<std::uint64_t>(where.id));
    }
    // the group: the innermost labelled function with one
    for (auto it = seen.path.rbegin(); it != seen.path.rend() && seen.group == nullptr; ++it) {
        if ((*it & (kPlace | kModule | kOwn)) != 0)
            continue;
        const auto label = g_labels.find(static_cast<std::uint32_t>(*it));
        if (label != g_labels.end() && !label->second.group.empty())
            seen.group = &label->second.group;
    }
}

// The samples of a recording, counted: a call tree of their paths, by group, by function and by module
struct Tally {
    unsigned samples = 0;
    std::vector<TrieNode> trie = std::vector<TrieNode>(1);
    std::map<std::string, unsigned> groups;
    std::map<std::uint64_t, unsigned> hot;
    std::map<int, unsigned> modules; // by number
};
Tally g_all;  // every sample of the recording
Tally g_slow; // those in its slow frames
Tally g_dip;  // those in the last take()'s dip frames: the frames under 15 fps
Tally g_look; // all of the last take()'s
unsigned g_read = 0; // samples taken into them

void count(Tally& t, const Seen& seen)
{
    ++t.samples;
    ++t.modules[seen.module];
    if (seen.has_hot)
        ++t.hot[seen.hot];
    ++t.groups[seen.group != nullptr ? *seen.group : std::string("(not labelled yet)")];
    int at = 0;
    ++t.trie[0].samples;
    for (const std::uint64_t key : seen.path) {
        auto child = t.trie[static_cast<size_t>(at)].kids.find(key);
        if (child == t.trie[static_cast<size_t>(at)].kids.end()) {
            const int made = static_cast<int>(t.trie.size());
            t.trie.push_back({key, 0, {}});
            child = t.trie[static_cast<size_t>(at)].kids.emplace(key, made).first;
        }
        at = child->second;
        ++t.trie[static_cast<size_t>(at)].samples;
    }
}

} // namespace

void install(const wchar_t* install_dir)
{
    g_install_dir = install_dir != nullptr ? install_dir : L"";
    if (!g_install_dir.empty() && g_install_dir.back() != L'\\')
        g_install_dir += L'\\';
    g_data_dir = install_dir != nullptr ? install_dir : L"";
    if (!g_data_dir.empty() && g_data_dir.back() != L'\\')
        g_data_dir += L'\\';
    g_data_dir += L"addons\\vanacore\\vana\\data\\";
}

bool start()
{
    if (g_run.load())
        return true;
    load_tables();
    // where FFXiMain.dll's code is, for telling a return address from any other number on the stack
    g_code.clear();
    const auto module = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"FFXiMain.dll"));
    g_base = static_cast<std::uint32_t>(module);
    if (module != 0) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && section->Misc.VirtualSize > 0)
                g_code.emplace_back(g_base + section->VirtualAddress, g_base + section->VirtualAddress + section->Misc.VirtualSize);
    }
    g_target_id = GetCurrentThreadId();
    g_head = 0;
    g_read = 0;
    g_all = {};
    g_slow = {};
    g_run = true;
    g_thread = CreateThread(nullptr, 0, &sample_thread, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        g_run = false;
        return false;
    }
    log::write(L"Sampler: looking at thread %lu about every millisecond (FFXiMain.dll at %08X, %zu code sections)", g_target_id, g_base,
               g_code.size());
    return true;
}

void stop()
{
    if (!g_run.exchange(false))
        return;
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    log::write(L"Sampler: stopped");
}

bool running()
{
    return g_run.load();
}

void take(const std::vector<Span>& slow, long long until, const std::vector<Span>& dip)
{
    load_tables();
    if (g_dip.samples > 0)
        g_dip = {};
    g_look = {};
    const unsigned head = g_head.load(std::memory_order_acquire);
    if (head - g_read > kRing - 1)
        g_read = head - (kRing - 1); // more than the ring holds since the last look: the latest (not the slot being written)
    Seen seen;
    size_t s = 0, d = 0;
    for (; g_read != head; ++g_read) {
        const Sample& sample = g_ring[g_read % kRing];
        if (sample.time >= until)
            break; // the next look's
        describe(sample, seen);
        count(g_all, seen);
        count(g_look, seen);
        while (s < slow.size() && slow[s].end <= sample.time)
            ++s;
        if (s < slow.size() && sample.time >= slow[s].start)
            count(g_slow, seen);
        while (d < dip.size() && dip[d].end <= sample.time)
            ++d;
        if (d < dip.size() && sample.time >= dip[d].start)
            count(g_dip, seen);
    }
}

namespace {

Result make_result(const Tally& t)
{
    Result result;
    result.have_functions = !g_functions.empty();
    result.samples = t.samples;
    if (t.samples == 0)
        return result;
    const double total = static_cast<double>(t.samples);

    const std::function<std::string(std::uint64_t)> name_of = [&](std::uint64_t key) -> std::string {
        if ((key & kOwn) != 0)
            return own_name(static_cast<int>(key & 0xFFFFFF));
        if ((key & kModule) != 0)
            return "[" + g_module_names[static_cast<size_t>(key & 0xFFFF)] + "]";
        if ((key & kPlace) != 0)
            return hex_name("FFXiMain.dll+0x", static_cast<std::uint32_t>(key));
        return function_name(static_cast<std::uint32_t>(key));
    };
    result.tree.name = "Call tree";
    result.tree.samples = result.samples;
    result.tree.share = 1.0;
    build_tree(t.trie, 0, result.tree, name_of, total, std::max(2u, static_cast<unsigned>(result.samples * 0.003)), 0);

    for (const auto& [name, samples] : t.groups) {
        Share share;
        share.name = name;
        share.samples = samples;
        share.share = static_cast<double>(samples) / total;
        result.groups.push_back(std::move(share));
    }
    std::sort(result.groups.begin(), result.groups.end(), [](const Share& a, const Share& b) { return a.samples > b.samples; });

    for (const auto& [key, samples] : t.hot) {
        Share share;
        share.name = (key & kOwn) != 0 ? own_name(static_cast<int>(key & 0xFFFFFF)) : function_name(static_cast<std::uint32_t>(key));
        share.samples = samples;
        share.share = static_cast<double>(samples) / total;
        result.hot.push_back(std::move(share));
    }
    std::sort(result.hot.begin(), result.hot.end(), [](const Share& a, const Share& b) { return a.samples > b.samples; });
    if (result.hot.size() > 40)
        result.hot.resize(40);

    for (const auto& [id, samples] : t.modules) {
        Share share;
        share.name = g_module_names[static_cast<size_t>(id)];
        share.samples = samples;
        share.share = static_cast<double>(samples) / total;
        result.modules.push_back(std::move(share));
    }
    std::sort(result.modules.begin(), result.modules.end(), [](const Share& a, const Share& b) { return a.samples > b.samples; });
    return result;
}

} // namespace

Result result(bool slow)
{
    load_tables();
    return make_result(slow ? g_slow : g_all);
}

Result look()
{
    load_tables();
    return make_result(g_look);
}

Result dip()
{
    load_tables();
    return make_result(g_dip);
}

} // namespace vanacore::sampler
