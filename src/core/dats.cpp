#include "dats.hpp"

#include <windows.h>

#include <mutex>

#include "log.hpp"
#include "pivot.hpp"

namespace vanacore::dats {
namespace {

// ROM folders a client can have: ROM (1) to ROM9 for retail's expansions, more for private
// servers' own.
constexpr int kRoms = 20;

struct Table {
    int rom = 0;
    std::vector<std::uint8_t> versions; // VTABLE: a byte per file id, the ROM it's in (0: none)
    std::vector<std::uint8_t> places;   // FTABLE: a u16 per file id, folder << 7 | file
};

std::once_flag g_loaded;
std::wstring g_folder;
std::vector<Table> g_tables; // ROM2 and up first (they're checked first), then ROM

} // namespace

bool read_file(const std::wstring& file, std::vector<std::uint8_t>& out)
{
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(handle, &size) && size.QuadPart < 256 * 1024 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(handle, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    CloseHandle(handle);
    return ok;
}

namespace {

void load()
{
    g_folder = ffxi_folder();
    if (g_folder.empty())
        return;
    for (int rom = kRoms; rom >= 1; --rom) {
        const std::wstring suffix = rom == 1 ? L"" : std::to_wstring(rom);
        const std::wstring folder = rom == 1 ? g_folder : g_folder + L"ROM" + suffix + L"\\";
        Table table;
        table.rom = rom;
        if (read_file(pivot::resolve(folder + L"VTABLE" + suffix + L".DAT"), table.versions) &&
            read_file(pivot::resolve(folder + L"FTABLE" + suffix + L".DAT"), table.places))
            g_tables.push_back(std::move(table));
    }
    log::write(L"DATs: %zu file tables in %ls", g_tables.size(), g_folder.c_str());
}

} // namespace

std::wstring ffxi_folder()
{
    const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
    if (game == nullptr)
        return {};
    wchar_t file[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(game, file, MAX_PATH);
    std::wstring path(file, length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash != std::wstring::npos ? path.substr(0, slash + 1) : std::wstring();
}

std::wstring rom_path(std::uint32_t file_id)
{
    std::call_once(g_loaded, &load);
    for (const Table& table : g_tables) {
        if (file_id >= table.versions.size() || file_id * 2 + 2 > table.places.size())
            continue;
        const std::uint8_t rom = table.versions[file_id];
        if (rom == 0)
            continue;
        const std::uint16_t place = static_cast<std::uint16_t>(table.places[file_id * 2] | (table.places[file_id * 2 + 1] << 8));
        const std::wstring folder = rom == 1 ? L"ROM" : L"ROM" + std::to_wstring(rom);
        return folder + L"\\" + std::to_wstring(place >> 7) + L"\\" + std::to_wstring(place & 0x7F) + L".DAT";
    }
    return {};
}

std::wstring path(std::uint32_t file_id)
{
    const std::wstring rom = rom_path(file_id);
    return rom.empty() ? rom : pivot::resolve(g_folder + rom);
}

bool read(std::uint32_t file_id, std::vector<std::uint8_t>& out)
{
    const std::wstring file = path(file_id);
    return !file.empty() && read_file(file, out);
}

} // namespace vanacore::dats
