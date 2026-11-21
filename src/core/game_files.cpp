#include "game_files.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <objbase.h>
#include <psapi.h>

#include <atomic>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "imports.hpp"
#include "log.hpp"
#include "registry.hpp"

#pragma comment(lib, "bcrypt.lib") // SHA-256 of the game's DLLs (scan)

namespace vanacore::game_files {
namespace {

enum class Root { Pol, Ffxi };

// PlayOnline's and FFXI's COM classes and the DLL each lives in, relative to its install folder,
// as the PlayOnline installer registers them (JP/US, and EU where it differs).
struct Class {
    const wchar_t* clsid;
    const wchar_t* name;
    Root root;
    const wchar_t* dll;
};
constexpr Class kClasses[] = {
    {L"{3501F5DD-7894-42DF-866A-A2B6527D8049}", L"POLCoreCom", Root::Pol, L"viewer\\com\\polcore.dll"},
    {L"{07974581-0DF6-4EF0-BD05-604B3ADA9BE9}", L"POLCoreCom", Root::Pol, L"viewer\\com\\polcore.dll"},
    {L"{E5966FB3-C97B-42EB-84BF-37F95EE54A9F}", L"POLCoreCom", Root::Pol, L"viewer\\com\\polcoreeu.dll"},
    {L"{40555AAE-53AD-4ABC-AE65-8441755E7D69}", L"PolAppCom", Root::Pol, L"viewer\\com\\app.dll"},
    {L"{1BD94C71-26E5-42C3-8A8A-8D4F004A309E}", L"PolAppCom", Root::Pol, L"viewer\\com\\app.dll"},
    {L"{2A670579-4385-4592-925B-50434026C86C}", L"PolAppCom", Root::Pol, L"viewer\\com\\appeu.dll"},
    {L"{62021866-976B-49A3-A18B-7A44869008A2}", L"PolContentsCom", Root::Pol, L"viewer\\contents\\PolContents.dll"},
    {L"{3FC1EF9A-F346-413C-BB47-ED6F9A4BD52F}", L"PolContentsCom", Root::Pol, L"viewer\\contents\\polcontentsINT.dll"},
    {L"{B61D1955-FDFF-44B0-BB43-E096F752FDD6}", L"PolContentsCom", Root::Pol, L"viewer\\contents\\PolContents.dll"},
    {L"{989D790D-6236-11D4-80E9-00105A81E890}", L"FFXiEntry", Root::Ffxi, L"FFXi.dll"},
    {L"{0DF0E951-D03C-4A94-90EF-40AE60668F5F}", L"FxFileManager", Root::Ffxi, L"FFXi.dll"},
    {L"{1027DC46-750D-4B1F-8834-1D25B8BEBAB8}", L"GameMain", Root::Ffxi, L"FFXiMain.dll"},
    {L"{E636B693-A5A7-4161-9F02-D769178DE989}", L"FFXiVersion", Root::Ffxi, L"FFXiVersions.dll"},
};
constexpr size_t kClassCount = std::size(kClasses);

CLSID g_ids[kClassCount];                // kClasses' CLSIDs, parsed
std::atomic<bool> g_logged[kClassCount]; // per class: first creation logged
std::wstring g_pol;                      // absolute, no trailing backslash; empty: not redirected
std::wstring g_ffxi;

decltype(&CoCreateInstance) g_create_instance = nullptr;

std::wstring g_vanacore; // VanaCore's own folder, the game's program folder
HANDLE g_scan = nullptr; // the thread listing the game's DLLs (scan)

// The DLLs create()'s LoadLibraryExW brings in, recorded by on_module on that thread: {path, from Windows}. A load
// that fails unloads them again, so this is the only record of a mod's DLL that broke it.
using Loaded = std::vector<std::pair<std::wstring, bool>>;
std::atomic<DWORD> g_loading_thread{0};
Loaded* g_loading = nullptr; // only g_loading_thread touches it

std::wstring module_path(HMODULE module)
{
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    return std::wstring(path, length < std::size(path) ? length : 0);
}

// The file's SHA-256 in hex, or empty if it can't be read.
std::wstring sha256(BCRYPT_ALG_HANDLE algorithm, const std::wstring& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    std::wstring hex;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCRYPT_SUCCESS(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0))) {
        std::vector<UCHAR> buffer(1 << 16);
        bool read_all = false;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                break;
            if (read == 0) {
                read_all = true;
                break;
            }
            if (!BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0)))
                break;
        }
        UCHAR digest[32] = {};
        if (read_all && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
            static const wchar_t kHex[] = L"0123456789ABCDEF";
            for (const UCHAR byte : digest) {
                hex += kHex[byte >> 4];
                hex += kHex[byte & 0xF];
            }
        }
        BCryptDestroyHash(hash);
    }
    CloseHandle(file);
    return hex;
}

// The .dll files in `root`, and in its subfolders if `recurse`.
std::vector<std::filesystem::path> dlls(const std::wstring& root, bool recurse)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> found;
    const auto take = [&found](const fs::directory_entry& entry) {
        std::error_code error;
        if (entry.is_regular_file(error) && _wcsicmp(entry.path().extension().c_str(), L".dll") == 0)
            found.push_back(entry.path());
    };
    std::error_code error;
    if (recurse) {
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
             !error && it != end; it.increment(error))
            take(*it);
    } else {
        for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error))
            take(*it);
    }
    return found;
}

// Logs every DLL in the game's folders with its size and SHA-256, so a player's log shows a damaged or swapped game
// file, and a mod's DLL named like one of Windows' (d3d8.dll, dsound.dll, dinput8.dll...): DLLs that load from that
// folder get it in place of Windows' own. Off the start-up path: the files are ~17 MB.
DWORD WINAPI scan(void*)
{
    const ULONGLONG start = GetTickCount64();
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        algorithm = nullptr;
    wchar_t system[MAX_PATH] = {}; // SysWOW64 for the 32-bit game, through WOW64's redirection
    const UINT system_length = GetSystemDirectoryW(system, MAX_PATH);

    struct Folder {
        const std::wstring* root;
        bool recurse; // FFXI's has 100,000 DATs below it; its DLLs are all at the top
    };
    const Folder folders[] = {{&g_ffxi, false}, {&g_pol, true}, {&g_vanacore, false}};
    int count = 0;
    std::wstring shadows; // names Windows has too
    for (const Folder& folder : folders) {
        if (folder.root->empty())
            continue;
        const std::filesystem::path parent = std::filesystem::path(*folder.root).parent_path();
        for (const std::filesystem::path& path : dlls(*folder.root, folder.recurse)) {
            ++count;
            std::error_code error;
            const unsigned long long size = std::filesystem::file_size(path, error);
            const std::wstring hash = algorithm != nullptr ? sha256(algorithm, path.wstring()) : L"";
            const std::wstring name = path.filename().wstring();
            const bool windows =
                system_length > 0 && system_length < MAX_PATH &&
                GetFileAttributesW((std::wstring(system, system_length) + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES;
            if (windows)
                shadows += (shadows.empty() ? L"" : L", ") + path.lexically_relative(parent).wstring();
            log::write(L"Game files: %ls, %llu bytes, SHA-256 %ls%ls", path.lexically_relative(parent).c_str(), size,
                       hash.empty() ? L"(can't be read)" : hash.c_str(),
                       windows ? L"; named like one of Windows' DLLs" : L"");
        }
    }
    if (algorithm != nullptr)
        BCryptCloseAlgorithmProvider(algorithm, 0);
    if (shadows.empty())
        log::write(L"Game files: %d DLLs in the game's folders, listed in %llu ms", count, GetTickCount64() - start);
    else
        log::write(L"Game files: %d DLLs in the game's folders, listed in %llu ms. Named like Windows' own, so loaded in "
                   L"their place by what loads from their folder (a mod's, or an old install's?): %ls",
                   count, GetTickCount64() - start, shadows.c_str());
    return 0;
}

// Why the game's own DLL didn't load: what the error means, what the attempt brought in, and what else is in the
// process from outside Windows (mods, overlays, security software). Once per class.
void log_failure(HRESULT result, const Loaded& loaded)
{
    if (g_scan != nullptr)
        WaitForSingleObject(g_scan, 5000); // the DLL listing first: the game may close as soon as this returns
    if (result == HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED))
        log::write(L"Game files: 0x%08lX is a DLL's start-up failing: its own or one it needs (below); usually one from "
                   L"outside Windows: a mod's, an overlay's or security software's", static_cast<unsigned long>(result));
    else if (result == HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND))
        log::write(L"Game files: 0x%08lX is a missing file: the DLL or one it needs", static_cast<unsigned long>(result));
    else if (result == HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT))
        log::write(L"Game files: 0x%08lX is a damaged file: the DLL or one it needs", static_cast<unsigned long>(result));
    else if (result == HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND))
        log::write(L"Game files: 0x%08lX is a function missing from a DLL it needs: one made for a newer Windows than "
                   L"this one (on Windows 7, say), or an old copy of one in the game's folder",
                   static_cast<unsigned long>(result));
    for (const auto& [path, windows] : loaded)
        log::write(L"Game files: loading it brought in %ls%ls", path.c_str(), windows ? L"" : L" (not Windows')");
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes))
        return;
    const DWORD count = bytes / sizeof(HMODULE) < 1024 ? bytes / sizeof(HMODULE) : 1024;
    for (DWORD i = 0; i < count; ++i)
        if (!imports::is_windows_module(modules[i]))
            log::write(L"Game files: in the game, from outside Windows: %ls", module_path(modules[i]).c_str());
}

// [client] `key` as the absolute path of an existing folder, or empty (logged) if it isn't one.
std::wstring folder(const wchar_t* ini, const wchar_t* key, const wchar_t* base_dir)
{
    wchar_t value[MAX_PATH] = {};
    GetPrivateProfileStringW(L"client", key, L"", value, MAX_PATH, ini);
    if (value[0] == L'\0')
        return {};
    // An absolute path replaces base_dir; lexically_normal() also collapses doubled backslashes.
    std::wstring path = (std::filesystem::path(base_dir) / value).lexically_normal().wstring();
    while (path.size() > 3 && path.back() == L'\\')
        path.pop_back();
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        log::write(L"Game files: [client] %ls=%ls is not a folder (%ls); ignored", key, value, path.c_str());
        return {};
    }
    return path;
}

const Class* find(REFCLSID clsid)
{
    for (size_t i = 0; i < kClassCount; ++i)
        if (IsEqualCLSID(clsid, g_ids[i]))
            return &kClasses[i];
    return nullptr;
}

// What COM does for an in-process server, minus the registry: load the DLL, ask it for the
// class factory, create the object. `loaded`: the DLLs the load brought in.
HRESULT create(REFCLSID clsid, const std::wstring& dll, IUnknown* outer, REFIID iid, void** object, Loaded& loaded)
{
    Loaded* const outer_loading = g_loading; // a DLL's start-up creating a class of its own
    const DWORD outer_thread = g_loading_thread.load();
    g_loading = &loaded;
    g_loading_thread.store(GetCurrentThreadId());
    const HMODULE module = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD error = GetLastError();
    g_loading_thread.store(outer_thread);
    g_loading = outer_loading;
    if (module == nullptr)
        return HRESULT_FROM_WIN32(error);
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto get_class_object = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    if (get_class_object == nullptr)
        return CLASS_E_CLASSNOTAVAILABLE;
    IClassFactory* factory = nullptr;
    HRESULT result = get_class_object(clsid, __uuidof(IClassFactory), reinterpret_cast<void**>(&factory));
    if (FAILED(result))
        return result;
    result = factory->CreateInstance(outer, iid, object);
    factory->Release();
    return result;
}

// Replaces CoCreateInstance in every non-Windows module.
HRESULT STDAPICALLTYPE create_instance(REFCLSID clsid, LPUNKNOWN outer, DWORD context, REFIID iid, LPVOID* object)
{
    const Class* found = find(clsid);
    const std::wstring& root = found == nullptr ? g_pol : found->root == Root::Pol ? g_pol : g_ffxi;
    if (found == nullptr || root.empty() || (context & CLSCTX_INPROC_SERVER) == 0)
        return g_create_instance(clsid, outer, context, iid, object);

    const std::wstring dll = root + L"\\" + found->dll;
    Loaded loaded;
    const HRESULT result = create(clsid, dll, outer, iid, object, loaded);
    const bool first = !g_logged[found - kClasses].exchange(true);
    if (SUCCEEDED(result)) {
        if (first)
            log::write(L"Game files: %ls from %ls", found->name, dll.c_str());
        return result;
    }
    if (first) {
        log::write(L"Game files: %ls from %ls failed (0x%08lX); using the one Windows has registered", found->name,
                   dll.c_str(), static_cast<unsigned long>(result));
        log_failure(result, loaded);
    }
    const HRESULT registered = g_create_instance(clsid, outer, context, iid, object);
    if (registered != REGDB_E_CLASSNOTREG)
        return registered;
    // Windows has none (a player's install mostly hasn't): the DLL's own failure is the reason to report, not that
    if (first)
        log::write(L"Game files: Windows has no %ls registered either (0x%08lX): 0x%08lX is the reason", found->name,
                   static_cast<unsigned long>(registered), static_cast<unsigned long>(result));
    return result;
}

// As each DLL loads, before its start-up runs (the loader lock is held).
void on_module(HMODULE module)
{
    const bool windows = imports::is_windows_module(module);
    if (g_loading_thread.load(std::memory_order_relaxed) == GetCurrentThreadId())
        g_loading->emplace_back(module_path(module), windows);
    if (!windows)
        imports::hook(module, "ole32.dll", "CoCreateInstance", &create_instance, g_create_instance);
}

} // namespace

void install(const wchar_t* ini, const wchar_t* base_dir)
{
    g_pol = folder(ini, L"pol", base_dir);
    g_ffxi = folder(ini, L"ffxi", base_dir);
    if (g_pol.empty() && g_ffxi.empty()) {
        log::write(L"No [client] pol= or ffxi= in %ls: PlayOnline and FFXI load from where Windows has them installed",
                   ini);
        return;
    }

    // Install paths, formatted as the PlayOnline installer writes them.
    if (!g_pol.empty())
        registry::serve(registry::Key::InstallFolder, "1000", g_pol);
    if (!g_ffxi.empty())
        registry::serve(registry::Key::InstallFolder, "0001", g_ffxi + L"\\");
    log::write(L"Game files: PlayOnline from %ls, FFXI from %ls", g_pol.empty() ? L"(registry)" : g_pol.c_str(),
               g_ffxi.empty() ? L"(registry)" : g_ffxi.c_str());
    g_vanacore = std::filesystem::path(base_dir).lexically_normal().wstring();
    while (g_vanacore.size() > 3 && g_vanacore.back() == L'\\')
        g_vanacore.pop_back();
    g_scan = CreateThread(nullptr, 0, &scan, nullptr, 0, nullptr); // kept open: log_failure waits for it

    for (size_t i = 0; i < kClassCount; ++i)
        IIDFromString(kClasses[i].clsid, &g_ids[i]);
    if (!imports::for_each_module(&on_module))
        log::write(L"Game files: could not watch modules load: COM classes come from the registry");
}

} // namespace vanacore::game_files
