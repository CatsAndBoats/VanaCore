#include "host.hpp"

#include <windows.h>

#include <vanacore/log_file.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "../../chat.hpp"
#include "../../crash.hpp"
#include "../../d3d8.hpp"
#include "../../imports.hpp"
#include "../../log.hpp"
#include "../../plugin_fault.hpp"
#include "../../plugin_memory.hpp"
#include "../../stats.hpp"
#include "../plugin_move.hpp"
#include "abi.hpp"
#include "objects.hpp"

namespace vanacore::windower {
namespace {

struct Plugin {
    std::string key;     // lower case: its command, //<key> args
    std::string display; // as its file spells it
    std::string path;
    HMODULE module = nullptr;
    abi::Plugin* object = nullptr;         // Windower 4's interface
    abi::legacy::Plugin* legacy = nullptr; // or Windower 3's
    std::vector<std::string> commands;     // more names it answers to (lower case), from Windower 3's GetInfo
    std::uint32_t interface_version = 0;
    std::string name;
    std::string author;
    bool crashed = false;   // faulted: never called again, and its DLL stays loaded
    bool unloading = false; // asked to unload while plugins were being called
};

std::vector<std::unique_ptr<Plugin>> g_plugins; // load order
std::string g_install;
std::wstring g_root;         // VanaCore's folder, wide, with a trailing backslash
std::wstring g_plugin_folder; // <VanaCore>\plugins\windower\ (lower case)
std::set<HMODULE> g_redirected; // plugin DLLs whose file access is redirected
DWORD g_thread = 0;
std::string g_calling;
int g_depth = 0;
bool g_closing = false; // the game is closing (shutdown): plugins' DLLs stay loaded (one may have hooked the window)

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::wstring lower(std::wstring_view text)
{
    std::wstring out(text);
    for (wchar_t& c : out)
        c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

// a fault in a plugin's code (plugin_fault.hpp)
using plugin_fault::describe;
using plugin_fault::Fault;
using plugin_fault::guarded;

void say_error(const std::string& text)
{
    chat::write(chat::kDefaultMode, chat::header() + chat::colour(chat::kError, text), "VanaCore");
}

// Calls into `plugin` (as the plugin being called), guarded: a fault disables it.
template <typename Call>
bool call_plugin(Plugin& plugin, const char* what, Call call)
{
    if (plugin.crashed)
        return false;
    const std::string outer = g_calling;
    g_calling = plugin.display;
    ++g_depth;
    Fault fault;
    bool ok = false;
    {
        const stats::PluginScope timing(plugin.display, what, g_thread != 0 && GetCurrentThreadId() == g_thread);
        ok = guarded(call, fault);
    }
    --g_depth;
    g_calling = outer;
    if (!ok && fault.code == crash::kCppException) {
        // Thrown and not caught (FFXIDB on "//ffxidb pos o 1"): its frames were unwound, as a
        // catch in Windower would; it carries on.
        const std::string reason = fault.message[0] != '\0' ? fault.message : "a C++ exception";
        log::write(L"Windower plugins: %hs failed in %hs: %hs (it carries on)", plugin.display.c_str(), what, reason.c_str());
        say_error("(Windower Plugin) " + plugin.display + " failed in " + what + ": " + reason);
        return false;
    }
    if (!ok) {
        plugin.crashed = true;
        const std::string details = describe(fault);
        log::write(L"Windower plugins: %hs crashed in %hs: %hs; it's disabled (its DLL stays loaded)", plugin.display.c_str(), what,
                   details.c_str());
        say_error("(Windower Plugin) " + plugin.display + " crashed (" + details + "): disabled until the game restarts");
    }
    return ok;
}

Plugin* find(std::string_view name)
{
    const std::string key = lower(name);
    for (auto& plugin : g_plugins)
        if (plugin->key == key && !plugin->unloading)
            return plugin.get();
    return nullptr;
}

Plugin* find_command(std::string_view word)
{
    const std::string key = lower(word);
    for (auto& plugin : g_plugins) {
        if (plugin->unloading)
            continue;
        if (plugin->key == key || std::find(plugin->commands.begin(), plugin->commands.end(), key) != plugin->commands.end())
            return plugin.get();
    }
    return nullptr;
}

std::vector<Plugin*> snapshot()
{
    std::vector<Plugin*> out;
    out.reserve(g_plugins.size());
    for (auto& plugin : g_plugins)
        if (!plugin->crashed && !plugin->unloading)
            out.push_back(plugin.get());
    return out;
}

void remove(Plugin& plugin)
{
    // Unload is a Windower 4 plugin's end: it deletes itself (FFXIDB's calls its own destructor),
    // as Windower never does. Windower 3's have Release for that.
    if (!plugin.crashed) {
        if (plugin.legacy != nullptr) {
            call_plugin(plugin, "Unload", [&] { plugin.legacy->Unload(); });
            call_plugin(plugin, "Release", [&] { plugin.legacy->Release(); });
        } else {
            call_plugin(plugin, "Unload", [&] { plugin.object->Unload(); });
        }
    }
    legacy_release(plugin.display);
    release_objects(plugin.display);
    if (!plugin.crashed && g_closing) {
        // the game is closing: its DLL stays loaded
    } else if (!plugin.crashed) {
        Fault fault;
        auto free = [&] { FreeLibrary(plugin.module); };
        g_redirected.erase(plugin.module);
        if (!guarded(free, fault))
            log::write(L"Windower plugins: %hs crashed while its DLL unloaded: %hs", plugin.display.c_str(), describe(fault).c_str());
        plugin_memory::unloaded(plugin.module);
    } else {
        log::write(L"Windower plugins: %hs crashed earlier: its DLL stays loaded", plugin.display.c_str());
    }
    for (auto it = g_plugins.begin(); it != g_plugins.end(); ++it) {
        if (it->get() == &plugin) {
            g_plugins.erase(it);
            break;
        }
    }
}

void process_unloads()
{
    if (g_depth != 0)
        return;
    for (size_t i = 0; i < g_plugins.size();) {
        if (g_plugins[i]->unloading)
            remove(*g_plugins[i]);
        else
            ++i;
    }
}

std::string text_of(const char* text)
{
    return text != nullptr ? std::string(text, strnlen(text, 256)) : std::string();
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// File access: <VanaCore>\plugins\<x> is <VanaCore>\plugins\windower\<x> for Windower plugins.

std::wstring wide(const char* text)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring out(length > 1 ? static_cast<size_t>(length) : 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), length);
    out.resize(out.size() - 1);
    return out;
}

std::string narrow(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(length > 1 ? static_cast<size_t>(length) : 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.resize(out.size() - 1);
    return out;
}

// The first redirections, in the log (a plugin not finding its files shows here).
void note(const std::wstring& from, const std::wstring& to)
{
    static std::mutex lock;
    static std::set<std::wstring> seen;
    {
        const std::lock_guard guard(lock);
        if (seen.size() >= 32 || !seen.insert(from).second)
            return;
    }
    log::write(L"Windower plugins: %ls is %ls", from.c_str(), to.c_str());
}

// A path as the plugin gave it, and where it really is.
struct WidePath {
    explicit WidePath(const wchar_t* path) : given(path)
    {
        if (path != nullptr) {
            redirected = redirect(path);
            changed = redirected != path;
            if (changed)
                note(path, redirected);
        }
    }
    const wchar_t* get() const { return changed ? redirected.c_str() : given; }
    const wchar_t* given;
    std::wstring redirected;
    bool changed = false;
};

struct NarrowPath {
    explicit NarrowPath(const char* path) : given(path)
    {
        if (path != nullptr) {
            const std::wstring from = wide(path);
            const std::wstring out = redirect(from);
            if (out != from) {
                redirected = narrow(out);
                changed = true;
                note(from, out);
            }
        }
    }
    const char* get() const { return changed ? redirected.c_str() : given; }
    const char* given;
    std::string redirected;
    bool changed = false;
};

#define ORIGINAL(name) decltype(&::name) g_##name = &::name

ORIGINAL(CreateFileA);
ORIGINAL(CreateFileW);
// Windows 8's, looked up so VanaCore.Core.dll loads on Windows 7 (hook() sets it from the import it replaces)
decltype(&::CreateFile2) g_CreateFile2 =
    reinterpret_cast<decltype(&::CreateFile2)>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFile2"));
ORIGINAL(GetFileAttributesA);
ORIGINAL(GetFileAttributesW);
ORIGINAL(GetFileAttributesExA);
ORIGINAL(GetFileAttributesExW);
ORIGINAL(FindFirstFileA);
ORIGINAL(FindFirstFileW);
ORIGINAL(FindFirstFileExA);
ORIGINAL(FindFirstFileExW);
ORIGINAL(CreateDirectoryA);
ORIGINAL(CreateDirectoryW);
ORIGINAL(RemoveDirectoryA);
ORIGINAL(RemoveDirectoryW);
ORIGINAL(DeleteFileA);
ORIGINAL(DeleteFileW);
ORIGINAL(MoveFileExA);
ORIGINAL(MoveFileExW);
ORIGINAL(CopyFileA);
ORIGINAL(CopyFileW);
ORIGINAL(SetFileAttributesA);
ORIGINAL(SetFileAttributesW);

HANDLE WINAPI create_file_a(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                            HANDLE templ)
{
    return g_CreateFileA(NarrowPath(path).get(), access, share, security, disposition, flags, templ);
}
HANDLE WINAPI create_file_w(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags,
                            HANDLE templ)
{
    return g_CreateFileW(WidePath(path).get(), access, share, security, disposition, flags, templ);
}
HANDLE WINAPI create_file_2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition, LPCREATEFILE2_EXTENDED_PARAMETERS extended)
{
    return g_CreateFile2(WidePath(path).get(), access, share, disposition, extended);
}
DWORD WINAPI get_file_attributes_a(LPCSTR path)
{
    return g_GetFileAttributesA(NarrowPath(path).get());
}
DWORD WINAPI get_file_attributes_w(LPCWSTR path)
{
    return g_GetFileAttributesW(WidePath(path).get());
}
BOOL WINAPI get_file_attributes_ex_a(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    return g_GetFileAttributesExA(NarrowPath(path).get(), level, info);
}
BOOL WINAPI get_file_attributes_ex_w(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info)
{
    return g_GetFileAttributesExW(WidePath(path).get(), level, info);
}
HANDLE WINAPI find_first_file_a(LPCSTR path, LPWIN32_FIND_DATAA data)
{
    return g_FindFirstFileA(NarrowPath(path).get(), data);
}
HANDLE WINAPI find_first_file_w(LPCWSTR path, LPWIN32_FIND_DATAW data)
{
    return g_FindFirstFileW(WidePath(path).get(), data);
}
HANDLE WINAPI find_first_file_ex_a(LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter,
                                   DWORD flags)
{
    return g_FindFirstFileExA(NarrowPath(path).get(), level, data, search, filter, flags);
}
HANDLE WINAPI find_first_file_ex_w(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data, FINDEX_SEARCH_OPS search, LPVOID filter,
                                   DWORD flags)
{
    return g_FindFirstFileExW(WidePath(path).get(), level, data, search, filter, flags);
}
BOOL WINAPI create_directory_a(LPCSTR path, LPSECURITY_ATTRIBUTES security)
{
    return g_CreateDirectoryA(NarrowPath(path).get(), security);
}
BOOL WINAPI create_directory_w(LPCWSTR path, LPSECURITY_ATTRIBUTES security)
{
    return g_CreateDirectoryW(WidePath(path).get(), security);
}
BOOL WINAPI remove_directory_a(LPCSTR path)
{
    return g_RemoveDirectoryA(NarrowPath(path).get());
}
BOOL WINAPI remove_directory_w(LPCWSTR path)
{
    return g_RemoveDirectoryW(WidePath(path).get());
}
BOOL WINAPI delete_file_a(LPCSTR path)
{
    return g_DeleteFileA(NarrowPath(path).get());
}
BOOL WINAPI delete_file_w(LPCWSTR path)
{
    return g_DeleteFileW(WidePath(path).get());
}
BOOL WINAPI move_file_ex_a(LPCSTR from, LPCSTR to, DWORD flags)
{
    return g_MoveFileExA(NarrowPath(from).get(), NarrowPath(to).get(), flags);
}
BOOL WINAPI move_file_ex_w(LPCWSTR from, LPCWSTR to, DWORD flags)
{
    return g_MoveFileExW(WidePath(from).get(), WidePath(to).get(), flags);
}
BOOL WINAPI copy_file_a(LPCSTR from, LPCSTR to, BOOL fail)
{
    return g_CopyFileA(NarrowPath(from).get(), NarrowPath(to).get(), fail);
}
BOOL WINAPI copy_file_w(LPCWSTR from, LPCWSTR to, BOOL fail)
{
    return g_CopyFileW(WidePath(from).get(), WidePath(to).get(), fail);
}
BOOL WINAPI set_file_attributes_a(LPCSTR path, DWORD attributes)
{
    return g_SetFileAttributesA(NarrowPath(path).get(), attributes);
}
BOOL WINAPI set_file_attributes_w(LPCWSTR path, DWORD attributes)
{
    return g_SetFileAttributesW(WidePath(path).get(), attributes);
}

// Visual C++ 2008's runtime, which Windower 3 plugins use: its file functions open files through
// its own imports, so the plugin's imports of them are redirected instead.
using Fopen = FILE*(__cdecl*)(const char* path, const char* mode);
using FopenS = errno_t(__cdecl*)(FILE** file, const char* path, const char* mode);
using StreamOpen = void(__thiscall*)(void* stream, const char* path, int mode, int protection);
using StreamMake = void*(__thiscall*)(void* stream, const char* path, int mode, int protection);
void* g_fopen = nullptr;
void* g_fopen_s = nullptr;
void* g_ifstream_open = nullptr;
void* g_ofstream_open = nullptr;
void* g_fstream_open = nullptr;
void* g_ifstream_make = nullptr;
void* g_ofstream_make = nullptr;
void* g_fstream_make = nullptr;

FILE* __cdecl fopen_redirected(const char* path, const char* mode)
{
    return reinterpret_cast<Fopen>(g_fopen)(NarrowPath(path).get(), mode);
}
errno_t __cdecl fopen_s_redirected(FILE** file, const char* path, const char* mode)
{
    return reinterpret_cast<FopenS>(g_fopen_s)(file, NarrowPath(path).get(), mode);
}
// __thiscall members, as __fastcall: `this` in ecx, edx unused, the rest on the stack.
void __fastcall ifstream_open(void* stream, void*, const char* path, int mode, int protection)
{
    reinterpret_cast<StreamOpen>(g_ifstream_open)(stream, NarrowPath(path).get(), mode, protection);
}
void __fastcall ofstream_open(void* stream, void*, const char* path, int mode, int protection)
{
    reinterpret_cast<StreamOpen>(g_ofstream_open)(stream, NarrowPath(path).get(), mode, protection);
}
void __fastcall fstream_open(void* stream, void*, const char* path, int mode, int protection)
{
    reinterpret_cast<StreamOpen>(g_fstream_open)(stream, NarrowPath(path).get(), mode, protection);
}
void* __fastcall ifstream_make(void* stream, void*, const char* path, int mode, int protection)
{
    return reinterpret_cast<StreamMake>(g_ifstream_make)(stream, NarrowPath(path).get(), mode, protection);
}
void* __fastcall ofstream_make(void* stream, void*, const char* path, int mode, int protection)
{
    return reinterpret_cast<StreamMake>(g_ofstream_make)(stream, NarrowPath(path).get(), mode, protection);
}
void* __fastcall fstream_make(void* stream, void*, const char* path, int mode, int protection)
{
    return reinterpret_cast<StreamMake>(g_fstream_make)(stream, NarrowPath(path).get(), mode, protection);
}

// A DLL loading from plugins\windower: its file access redirected before its code runs.
void on_module(HMODULE module)
{
    if (g_plugin_folder.empty() || imports::is_windows_module(module))
        return;
    wchar_t file[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(module, file, static_cast<DWORD>(std::size(file)));
    if (length == 0 || length >= std::size(file))
        return;
    const std::wstring path = lower(std::wstring_view(file, length));
    if (path.size() <= g_plugin_folder.size() || path.compare(0, g_plugin_folder.size(), g_plugin_folder) != 0 ||
        path.find(L'\\', g_plugin_folder.size()) != std::wstring::npos)
        return;
    if (!g_redirected.insert(module).second)
        return; // done already
    static constexpr const char* kKernel[] = {"KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll",
                                              "api-ms-win-core-file-l2-1-0.dll"};
    int hooked = 0;
    for (const char* dll : kKernel) {
#define HOOK(name, replacement) hooked += imports::hook(module, dll, #name, &replacement, g_##name) ? 1 : 0
        HOOK(CreateFileA, create_file_a);
        HOOK(CreateFileW, create_file_w);
        HOOK(CreateFile2, create_file_2);
        HOOK(GetFileAttributesA, get_file_attributes_a);
        HOOK(GetFileAttributesW, get_file_attributes_w);
        HOOK(GetFileAttributesExA, get_file_attributes_ex_a);
        HOOK(GetFileAttributesExW, get_file_attributes_ex_w);
        HOOK(FindFirstFileA, find_first_file_a);
        HOOK(FindFirstFileW, find_first_file_w);
        HOOK(FindFirstFileExA, find_first_file_ex_a);
        HOOK(FindFirstFileExW, find_first_file_ex_w);
        HOOK(CreateDirectoryA, create_directory_a);
        HOOK(CreateDirectoryW, create_directory_w);
        HOOK(RemoveDirectoryA, remove_directory_a);
        HOOK(RemoveDirectoryW, remove_directory_w);
        HOOK(DeleteFileA, delete_file_a);
        HOOK(DeleteFileW, delete_file_w);
        HOOK(MoveFileExA, move_file_ex_a);
        HOOK(MoveFileExW, move_file_ex_w);
        HOOK(CopyFileA, copy_file_a);
        HOOK(CopyFileW, copy_file_w);
        HOOK(SetFileAttributesA, set_file_attributes_a);
        HOOK(SetFileAttributesW, set_file_attributes_w);
#undef HOOK
    }
    const auto hook_runtime = [&](const char* dll, const char* function, void* replacement, void*& original) {
        void* previous = nullptr;
        if (imports::hook(module, dll, function, replacement, &previous)) {
            ++hooked;
            if (previous != replacement)
                original = previous;
        }
    };
    hook_runtime("MSVCR90.dll", "fopen", reinterpret_cast<void*>(&fopen_redirected), g_fopen);
    hook_runtime("MSVCR90.dll", "fopen_s", reinterpret_cast<void*>(&fopen_s_redirected), g_fopen_s);
    hook_runtime("MSVCP90.dll", "?open@?$basic_ifstream@DU?$char_traits@D@std@@@std@@QAEXPBDHH@Z", reinterpret_cast<void*>(&ifstream_open),
                 g_ifstream_open);
    hook_runtime("MSVCP90.dll", "?open@?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAEXPBDHH@Z", reinterpret_cast<void*>(&ofstream_open),
                 g_ofstream_open);
    hook_runtime("MSVCP90.dll", "?open@?$basic_fstream@DU?$char_traits@D@std@@@std@@QAEXPBDHH@Z", reinterpret_cast<void*>(&fstream_open),
                 g_fstream_open);
    hook_runtime("MSVCP90.dll", "??0?$basic_ifstream@DU?$char_traits@D@std@@@std@@QAE@PBDHH@Z", reinterpret_cast<void*>(&ifstream_make),
                 g_ifstream_make);
    hook_runtime("MSVCP90.dll", "??0?$basic_ofstream@DU?$char_traits@D@std@@@std@@QAE@PBDHH@Z", reinterpret_cast<void*>(&ofstream_make),
                 g_ofstream_make);
    hook_runtime("MSVCP90.dll", "??0?$basic_fstream@DU?$char_traits@D@std@@@std@@QAE@PBDHH@Z", reinterpret_cast<void*>(&fstream_make),
                 g_fstream_make);
    log::write(L"Windower plugins: %ls: %d file functions redirected (plugins\\ is plugins\\windower\\ for it)", file + g_root.size(),
               hooked);
}

} // namespace

////////////////////////////////////////////////////////////////////////////////////////////////////

std::string_view calling()
{
    if (g_thread != 0 && GetCurrentThreadId() != g_thread)
        return {};
    return g_calling;
}

const std::string& install_path()
{
    return g_install;
}

std::wstring redirect(std::wstring_view path)
{
    if (g_root.empty())
        return std::wstring(path);
    // <root>plugins\<rest>, either slash (runs of them as one), any case.
    std::wstring normal;
    normal.reserve(path.size());
    for (const wchar_t c : path) {
        const wchar_t d = c == L'/' ? L'\\' : c;
        if (d == L'\\' && !normal.empty() && normal.back() == L'\\' && normal.size() > 2)
            continue;
        normal += d;
    }
    const std::wstring prefix = g_root + L"plugins\\";
    if (normal.size() <= prefix.size() || lower(std::wstring_view(normal).substr(0, prefix.size())) != lower(prefix))
        return std::wstring(path);
    const std::wstring_view rest = std::wstring_view(normal).substr(prefix.size());
    const std::wstring first = lower(rest.substr(0, rest.find(L'\\')));
    if (first == L"ashita" || first == L"windower")
        return std::wstring(path);
    return prefix + L"windower\\" + std::wstring(rest);
}

void install(const std::string& install_dir)
{
    g_install = install_dir;
    g_root = wide(install_dir.c_str());
    if (!g_root.empty() && g_root.back() != L'\\')
        g_root += L'\\';
    g_plugin_folder = lower(g_root + L"plugins\\windower\\");
    if (!imports::for_each_module(&on_module))
        log::write(L"Windower plugins: can't watch DLLs load: their files won't be redirected");
}

bool load(const std::string& display, const std::string& path, std::string& error)
{
    if (g_thread == 0)
        g_thread = GetCurrentThreadId();
    if (find(display) != nullptr) {
        error = display + " is already loaded";
        return false;
    }
    plugin_memory::loading(display, path); // what it allocates is counted from before its DllMain
    const HMODULE module = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD code = GetLastError();
    plugin_memory::loaded(module, display);
    // its DLL let go of, uncounted
    const auto free_module = [&] {
        FreeLibrary(module);
        g_redirected.erase(module);
        plugin_memory::unloaded(module);
    };
    if (module == nullptr) {
        char text[256] = {};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, text, sizeof(text), nullptr);
        std::string reason(text);
        while (!reason.empty() && (reason.back() == '\n' || reason.back() == '\r' || reason.back() == '.'))
            reason.pop_back();
        error = "Windows couldn't load its DLL (" + reason + ", error " + std::to_string(code) + ")";
        return false;
    }
    on_module(module); // in case it was loaded before VanaCore watched (it's hooked once)
    const auto get_version = reinterpret_cast<abi::GetInterfaceVersion_f>(GetProcAddress(module, "GetInterfaceVersion"));
    const auto create = reinterpret_cast<abi::CreateInstance_f>(GetProcAddress(module, "CreateInstance"));
    if (get_version == nullptr || create == nullptr) {
        free_module();
        error = "it isn't a Windower plugin (no GetInterfaceVersion or CreateInstance)";
        return false;
    }

    auto plugin = std::make_unique<Plugin>();
    plugin->key = lower(display);
    plugin->display = display;
    plugin->path = path;
    plugin->module = module;
    Plugin& p = *plugin;
    if (!call_plugin(p, "GetInterfaceVersion", [&] { p.interface_version = get_version(); })) {
        error = "it crashed reporting its interface version";
        return false;
    }
    const bool legacy = abi::legacy::is_version(p.interface_version);
    if (p.interface_version != abi::kInterfaceVersion && !legacy) {
        free_module();
        char text[160];
        sprintf_s(text, "it's built for Windower plugin interface 0x%08X; VanaCore hosts 0x%08X (Windower 4) and 0x0004xxxx (Windower 3)",
                  p.interface_version, abi::kInterfaceVersion);
        error = text;
        return false;
    }
    const bool made = legacy ? call_plugin(p, "CreateInstance", [&] { p.legacy = reinterpret_cast<abi::legacy::CreateInstance_f>(create)(); }) &&
                                   p.legacy != nullptr
                             : call_plugin(p, "CreateInstance", [&] { p.object = create(); }) && p.object != nullptr;
    if (!made) {
        if (!p.crashed)
            free_module();
        error = p.crashed ? "it crashed starting" : "it didn't start (CreateInstance returned nothing)";
        return false;
    }
    if (legacy) {
        auto info = std::make_unique<abi::legacy::Info>();
        std::memset(info.get(), 0, sizeof(*info));
        call_plugin(p, "GetInfo", [&] { p.legacy->GetInfo(info.get()); });
        p.author = std::string(info->author, strnlen(info->author, sizeof(info->author)));
        p.name = p.display;
        // "st|statustimer": the commands it answers to.
        const std::string commands(info->commands, strnlen(info->commands, sizeof(info->commands)));
        for (size_t start = 0; start <= commands.size();) {
            const size_t end = std::min(commands.find('|', start), commands.size());
            if (end > start)
                p.commands.push_back(lower(std::string_view(commands).substr(start, end - start)));
            start = end + 1;
        }
    } else {
        call_plugin(p, "its details", [&] {
            p.name = text_of(p.object->GetName());
            p.author = text_of(p.object->GetAuthor());
        });
    }
    g_plugins.push_back(std::move(plugin));
    if (legacy)
        call_plugin(p, "Load", [&] { p.legacy->Load(legacy_host()); });
    else
        call_plugin(p, "Load", [&] { p.object->Load(manager()); });
    if (p.crashed) {
        error = "it crashed starting";
        return false;
    }
    log::write(L"Windower plugins: loaded %hs (%hs by %hs, interface 0x%08X) from %hs", p.display.c_str(), p.name.c_str(),
               p.author.c_str(), p.interface_version, p.path.c_str());
    return true;
}

bool unload(const std::string& name)
{
    Plugin* plugin = find(name);
    if (plugin == nullptr)
        return false;
    plugin->unloading = true;
    process_unloads();
    log::write(L"Windower plugins: unloaded %hs", name.c_str());
    return true;
}

void shutdown()
{
    if (g_depth != 0) {
        log::write(L"Windower plugins: the game is closing while a plugin is running: left as they are");
        return;
    }
    g_closing = true;
    for (size_t i = g_plugins.size(); i-- > 0;) {
        if (i >= g_plugins.size())
            continue;
        const std::string display = g_plugins[i]->display;
        remove(*g_plugins[i]);
        log::write(L"Windower plugins: unloaded %hs", display.c_str());
    }
}

bool is_loaded(const std::string& name)
{
    return find(name) != nullptr;
}

bool info(const std::string& name, Info& out)
{
    const Plugin* plugin = find(name);
    if (plugin == nullptr)
        return false;
    out = {plugin->display, plugin->name, plugin->author, plugin->interface_version, plugin->crashed};
    return true;
}

std::vector<Info> list()
{
    std::vector<Info> out;
    for (const auto& plugin : g_plugins)
        if (!plugin->unloading)
            out.push_back({plugin->display, plugin->name, plugin->author, plugin->interface_version, plugin->crashed});
    return out;
}

bool on_command(const std::string& line)
{
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;
    if (line.compare(start, 2, "//") != 0)
        return false;
    const size_t word = start + 2;
    size_t end = word;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t')
        ++end;
    Plugin* plugin = find_command(std::string_view(line).substr(word, end - word));
    if (plugin == nullptr || plugin->crashed)
        return false;
    // What follows its name, the space included (as Windower passes it).
    const std::string args = line.substr(end);
    if (plugin->legacy != nullptr)
        call_plugin(*plugin, "Command", [&] { plugin->legacy->Command(args.c_str(), 0); });
    else
        call_plugin(*plugin, "PluginCommand", [&] { plugin->object->PluginCommand(args.c_str()); });
    process_unloads();
    return true;
}

bool on_mouse(int type, int x, int y, int delta, bool blocked)
{
    // A left press (and double click) reaches them only with Shift held: FFXIDB starts dragging its
    // minimap on any press over it, so a plain click stays the game's and Shift + left drag moves it,
    // as Ashita's fonts move. Their releases always come, so a drag always ends.
    if ((type == WM_LBUTTONDOWN - WM_MOUSEFIRST || type == WM_LBUTTONDBLCLK - WM_MOUSEFIRST) &&
        (GetKeyState(VK_SHIFT) & 0x8000) == 0)
        return false;
    // The wheel in notches, as Hook.dll hands it them (its turn / WHEEL_DELTA, toward zero)
    const int notches = delta / WHEEL_DELTA;
    bool taken = false;
    for (Plugin* plugin : snapshot()) {
        if (plugin->object == nullptr)
            continue;
        int px = x, py = y;
        plugin_move::unmoved(plugin->display, px, py); // (its drawing moved: the mouse where it put it)
        call_plugin(*plugin, "Mouse", [&] {
            taken = plugin->object->Mouse(type, px, py, notches, blocked || taken) || taken;
        });
    }
    process_unloads();
    return taken;
}

bool on_key(WPARAM, LPARAM flags, bool blocked)
{
    const auto dik = static_cast<std::uintptr_t>(((flags >> 16) & 0x7F) | ((flags & (1 << 24)) != 0 ? 0x80 : 0));
    const auto down = static_cast<std::uintptr_t>((flags & (1u << 31)) == 0 ? 1 : 0);
    bool taken = false;
    for (Plugin* plugin : snapshot()) {
        if (plugin->object == nullptr)
            continue;
        call_plugin(*plugin, "Keyboard", [&] {
            taken = plugin->object->Keyboard(reinterpret_cast<void*>(dik), reinterpret_cast<void*>(down), blocked || taken) || taken;
        });
    }
    process_unloads();
    return taken;
}

bool on_unhandled(std::string_view line)
{
    const std::string text(line);
    bool handled = false;
    for (Plugin* plugin : snapshot()) {
        call_plugin(*plugin, "UnhandledCommand", [&] {
            handled = plugin->legacy != nullptr ? plugin->legacy->UnhandledCommand(text.c_str()) : plugin->object->UnhandledCommand(text.c_str());
        });
        if (handled)
            break;
    }
    process_unloads();
    return handled;
}

void on_device(void* device)
{
    if (g_thread == 0)
        g_thread = GetCurrentThreadId();
    set_device(device);
}

void draw(void* device)
{
    // Windower's order, inside the frame's scene: the plugins' PreRender (FFXIDB draws its
    // minimap there, on the device itself), Windower's own text and primitives, PostRender.
    const std::vector<Plugin*> plugins = snapshot();
    if (!plugins.empty()) {
        // What they change on the device is undone before VanaCore draws on.
        DWORD overlay = 0;
        const bool saved = SUCCEEDED(d3d8::call(device, d3d8::slot::CreateStateBlock, d3d8::kStateBlockAll, &overlay));
        for (Plugin* plugin : plugins) {
            plugin_move::drawing_as(plugin->display); // (what it draws moved as Addons > Layout has it)
            if (plugin->legacy != nullptr)
                call_plugin(*plugin, "its frame", [&] { plugin->legacy->Frame(); });
            else
                call_plugin(*plugin, "PreRender", [&] { plugin->object->PreRender(); });
        }
        plugin_move::drawing_as({});
        if (saved) {
            d3d8::call(device, d3d8::slot::ApplyStateBlock, overlay);
            d3d8::call(device, d3d8::slot::DeleteStateBlock, overlay);
        }
    }
    draw_objects(device);
    for (Plugin* plugin : snapshot())
        if (plugin->object != nullptr) {
            plugin_move::drawing_as(plugin->display);
            call_plugin(*plugin, "PostRender", [&] { plugin->object->PostRender(); });
        }
    plugin_move::drawing_as({});
    process_unloads();
}

void on_received(const std::uint8_t* buffer, std::size_t size)
{
    state_on_received(buffer, size);
}

bool wants_packets()
{
    for (const auto& plugin : g_plugins)
        if (!plugin->crashed && !plugin->unloading)
            return true;
    return false;
}

bool on_packet(bool incoming, std::uint16_t id, std::uint32_t size, const std::uint8_t* data, std::uint8_t* modified, bool injected,
               bool blocked)
{
    // Windower's plugins read 0x200 bytes of each: copies, padded.
    alignas(4) std::uint8_t original[0x200] = {};
    alignas(4) std::uint8_t current[0x200] = {};
    const size_t length = std::min<size_t>(size, 0x1FC);
    std::memcpy(original, data, length);
    for (Plugin* plugin : snapshot()) {
        std::memset(current, 0, sizeof(current));
        std::memcpy(current, modified, length);
        call_plugin(*plugin, incoming ? "IncomingChunk" : "OutgoingChunk", [&] {
            const bool block =
                plugin->legacy != nullptr
                    ? (incoming ? plugin->legacy->IncomingChunk(original, current, injected, blocked)
                                : plugin->legacy->OutgoingChunk(original, current, injected, blocked))
                    : (incoming ? plugin->object->IncomingChunk(original, current, injected, blocked)
                                : plugin->object->OutgoingChunk(original, current, injected, blocked));
            blocked = blocked || block;
        });
        // What it changed, up to the largest packet a header can describe.
        std::memcpy(modified, current, 0x1FC);
    }
    process_unloads();
    record_packet(incoming, id, modified, static_cast<std::uint32_t>(length));
    return blocked;
}

} // namespace vanacore::windower
