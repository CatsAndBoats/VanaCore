// Ashita's folder layout on VanaCore's. An Ashita addon is <install>\addons\ashita\<x> here, where Ashita has
// <install>\addons\<x>, and addons build those paths themselves: trove lists its plugins with
// io.popen('dir /b "' .. AshitaCore:GetInstallPath() .. 'addons\\trove\\core\\*.lua"'), XIUI loads
// '%saddons\\XIUI\\assets\\...'. VanaCore.Core.dll links LuaJIT and the C runtime in, so every addon's io, os,
// require and ffi.load, and VanaCore's own file access, reach Windows through its imports from KERNEL32: pointed here,
// they find the file wherever the addon spelled it, whatever function it used. Each path moved is logged (the first
// kLogged), so an addon looking in Ashita's layout shows in vanacore.log.

#include <windows.h>

#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

#include "../../imports.hpp"
#include "../../log.hpp"
#include "backend.hpp"

namespace vanacore::ashita {
namespace {

constexpr size_t kNone = std::wstring_view::npos;
constexpr size_t kLogged = 64;

std::wstring g_root; // <install>addons, as VanaCore's folder is spelled; set once, before the hooks go in

bool separator(wchar_t c)
{
    return c == L'\\' || c == L'/';
}

// Where the first folder under <install>\addons starts if `text` has that root at `at` (any case, / or \, a run of
// them counting as one: GetInstallPath ends in a backslash and addons add their own); kNone if it doesn't.
size_t after_root(std::wstring_view text, size_t at)
{
    size_t i = at;
    for (size_t r = 0; r < g_root.size(); ++r) {
        if (separator(g_root[r])) {
            if (i >= text.size() || !separator(text[i]))
                return kNone;
            while (i < text.size() && separator(text[i]))
                ++i;
            while (r + 1 < g_root.size() && separator(g_root[r + 1]))
                ++r;
        } else {
            if (i >= text.size() || std::towlower(text[i]) != std::towlower(g_root[r]))
                return kNone;
            ++i;
        }
    }
    if (i >= text.size() || !separator(text[i]))
        return kNone;
    while (i < text.size() && separator(text[i]))
        ++i;
    return i;
}

// Whether the folder named from `first` (to a separator, or in a command line a quote or a space too: `end`) is an
// Ashita addon's, so moves under addons\ashita. The kinds' own folders stay, and so do . and .. (addons\..\config) and
// addons\install, where addon packages wait (addonpkg.hpp).
bool moves(std::wstring_view text, size_t first, bool command, size_t& end)
{
    end = first;
    while (end < text.size() && !separator(text[end]) && !(command && (text[end] == L'"' || std::iswspace(text[end]))))
        ++end;
    std::wstring name(text.substr(first, end - first));
    for (wchar_t& c : name)
        c = static_cast<wchar_t>(std::towlower(c));
    return !name.empty() && name != L"." && name != L".." && name != L"ashita" && name != L"windower" && name != L"vanacore" &&
           name != L"install";
}

// `text` with ashita\ put in after <install>\addons\ where it names an Ashita addon's folder: at its start (a path), or
// anywhere in it (a command line: cmd /c dir /b "<path>"). False, `out` untouched, if nothing moves.
bool rewrite(std::wstring_view text, bool command, std::wstring& out)
{
    if (g_root.empty() || text.size() <= g_root.size())
        return false;
    std::wstring result;
    size_t copied = 0;
    const size_t starts = command ? text.size() - g_root.size() : 1;
    for (size_t at = 0; at < starts; ++at) {
        const size_t first = after_root(text, at);
        size_t end = 0;
        if (first == kNone || !moves(text, first, command, end))
            continue;
        result.append(text.substr(copied, first - copied));
        result += L"ashita";
        result += text[first - 1]; // the separator they used
        copied = first;
        at = end;
    }
    if (copied == 0)
        return false;
    result.append(text.substr(copied));
    out = std::move(result);
    return true;
}

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

void note(std::wstring_view from, const std::wstring& to)
{
    static std::mutex lock;
    static std::set<std::wstring, std::less<>> seen;
    bool last = false;
    {
        const std::lock_guard guard(lock);
        if (seen.size() >= kLogged || !seen.emplace(from).second)
            return;
        last = seen.size() == kLogged;
    }
    log::write(L"Ashita layout: %.*ls is %ls%ls", static_cast<int>(from.size()), from.data(), to.c_str(),
               last ? L" (the rest aren't logged)" : L"");
}

// A path (or command line) as the caller gave it, and where VanaCore has it.
struct WidePath {
    WidePath(const wchar_t* text, bool command = false) : given(text)
    {
        changed = text != nullptr && rewrite(text, command, moved);
        if (changed)
            note(text, moved);
    }
    const wchar_t* get() const { return changed ? moved.c_str() : given; }
    wchar_t* writable() { return changed ? moved.data() : const_cast<wchar_t*>(given); }
    const wchar_t* given;
    std::wstring moved;
    bool changed = false;
};

struct NarrowPath {
    explicit NarrowPath(const char* text) : given(text)
    {
        if (text == nullptr)
            return;
        const std::wstring from = wide(text);
        std::wstring to;
        if (rewrite(from, false, to)) {
            moved = narrow(to);
            changed = true;
            note(from, to);
        }
    }
    const char* get() const { return changed ? moved.c_str() : given; }
    const char* given;
    std::string moved;
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
ORIGINAL(LoadLibraryW);
ORIGINAL(LoadLibraryExA);
ORIGINAL(LoadLibraryExW);
ORIGINAL(CreateProcessW);

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
// ffi.load and package.loadlib
HMODULE WINAPI load_library_w(LPCWSTR path)
{
    return g_LoadLibraryW(WidePath(path).get());
}
HMODULE WINAPI load_library_ex_a(LPCSTR path, HANDLE file, DWORD flags)
{
    return g_LoadLibraryExA(NarrowPath(path).get(), file, flags);
}
HMODULE WINAPI load_library_ex_w(LPCWSTR path, HANDLE file, DWORD flags)
{
    return g_LoadLibraryExW(WidePath(path).get(), file, flags);
}
// io.popen and os.execute run cmd /c <their line>: the paths in it are moved, as is cmd's folder if it's given one.
// The game has no console, so each cmd got a console window of its own (trove's dir /b scans flashed four at load);
// CREATE_NO_WINDOW keeps it hidden, with its pipes and the windows of what it starts (start, explorer) as they were.
BOOL WINAPI create_process_w(LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process, LPSECURITY_ATTRIBUTES thread,
                             BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR folder, LPSTARTUPINFOW startup,
                             LPPROCESS_INFORMATION information)
{
    if ((flags & (CREATE_NEW_CONSOLE | DETACHED_PROCESS)) == 0 && GetConsoleWindow() == nullptr)
        flags |= CREATE_NO_WINDOW;
    WidePath line(command, true);
    return g_CreateProcessW(WidePath(application).get(), command != nullptr ? line.writable() : nullptr, process, thread, inherit,
                            flags, environment, WidePath(folder).get(), startup, information);
}

} // namespace

std::wstring layout_path(const std::wstring& path)
{
    std::wstring out;
    return rewrite(path, false, out) ? out : path;
}

void install_layout(const std::string& install_dir)
{
    static bool done = false; // VanaCore.Core.dll's imports are pointed here once: again would call itself
    if (done || install_dir.empty())
        return;
    done = true;
    g_root = wide(install_dir.c_str());
    if (!separator(g_root.back()))
        g_root += L'\\';
    g_root += L"addons";

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&install_layout), &self)) {
        log::write(L"Ashita layout: can't find VanaCore.Core.dll: addons' paths to %ls\\<addon> aren't moved to addons\\ashita",
                   g_root.c_str());
        return;
    }
    static constexpr const char* kKernel[] = {"KERNEL32.dll", "api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll",
                                              "api-ms-win-core-file-l2-1-0.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
                                              "api-ms-win-core-processthreads-l1-1-0.dll"};
    int hooked = 0;
    for (const char* dll : kKernel) {
#define HOOK(name, replacement) hooked += imports::hook(self, dll, #name, &replacement, g_##name) ? 1 : 0
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
        HOOK(LoadLibraryW, load_library_w);
        HOOK(LoadLibraryExA, load_library_ex_a);
        HOOK(LoadLibraryExW, load_library_ex_w);
        HOOK(CreateProcessW, create_process_w);
#undef HOOK
    }
    log::write(L"Ashita layout: %ls\\<addon> is %ls\\ashita\\<addon> for the files addons open, list, make and run (%d functions)",
               g_root.c_str(), g_root.c_str(), hooked);
}

} // namespace vanacore::ashita
