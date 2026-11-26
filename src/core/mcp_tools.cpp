// The MCP server's tools (mcp.hpp): what an AI connected to the game can see and do. Each tool takes its
// arguments as JSON and answers with text written for a reader (hex dumps, disassembly, tables), or a
// picture. Memory is read with ReadProcessMemory, so a bad address is an error and never a crash; what
// belongs to the game's thread (its structures, Lua, the command line, the screen) is run there at the next
// frame (on_game_thread).
#include "mcp_tools.hpp"

#include <windows.h>

#include <ole2.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <vanacore/version.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "addons.hpp"
#include "chat.hpp"
#include "disasm.hpp"
#include "game.hpp"
#include "gametext.hpp"
#include "log.hpp"
#include "packets.hpp"
#include "screenshot.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace vanacore::mcp::tools {
namespace {

// A tool's own failure (a bad argument, an unreadable address): its text goes back as the tool's error.
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message)
{
    throw Failure(message);
}

// ---- text ------------------------------------------------------------------------------------

std::string hex(std::uint64_t value, int digits = 0)
{
    char text[32];
    if (digits > 0)
        std::snprintf(text, sizeof text, "0x%0*llX", digits, static_cast<unsigned long long>(value));
    else
        std::snprintf(text, sizeof text, "0x%llX", static_cast<unsigned long long>(value));
    return text;
}

std::string hex8(std::uint32_t value)
{
    return hex(value, 8);
}

std::string strf(const char* pattern, ...)
{
    char text[1024];
    va_list args;
    va_start(args, pattern);
    std::vsnprintf(text, sizeof text, pattern, args);
    va_end(args);
    return text;
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The game's text is Shift-JIS with its own row for ñ, é, ¿ and the like (gametext.hpp); JSON's is UTF-8.
std::string utf8(std::string_view sjis)
{
    return gametext::to_utf8(sjis);
}

std::string sjis(std::string_view text)
{
    return gametext::from_utf8(text);
}

std::string narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

const char* job_name(unsigned job)
{
    static const char* const kJobs[] = {"NON", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK", "BST", "BRD", "RNG",
                                        "SAM", "NIN", "DRG", "SMN", "BLU", "COR", "PUP", "DNC", "SCH", "GEO", "RUN"};
    return job < sizeof(kJobs) / sizeof(kJobs[0]) ? kJobs[job] : "???";
}

// ---- memory ----------------------------------------------------------------------------------

// Copies `size` bytes from the game's memory; false (an unreadable page) if any can't be read.
bool read(std::uint32_t address, void* out, size_t size)
{
    SIZE_T got = 0;
    return size == 0 || (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), out,
                                           size, &got) &&
                         got == size);
}

bool read32(std::uint32_t address, std::uint32_t& out)
{
    return read(address, &out, 4);
}

// What can be read of `size` bytes, a page at a time: `valid` says which bytes were.
size_t read_partial(std::uint32_t address, std::uint8_t* out, std::vector<bool>& valid, size_t size)
{
    valid.assign(size, false);
    size_t readable = 0;
    for (size_t done = 0; done < size;) {
        const std::uint64_t at = static_cast<std::uint64_t>(address) + done;
        const size_t page_left = 0x1000 - static_cast<size_t>(at & 0xFFF);
        const size_t part = std::min(page_left, size - done);
        if (at <= 0xFFFFFFFFull && read(static_cast<std::uint32_t>(at), out + done, part)) {
            std::fill(valid.begin() + static_cast<std::ptrdiff_t>(done), valid.begin() + static_cast<std::ptrdiff_t>(done + part), true);
            readable += part;
        } else {
            std::memset(out + done, 0, part);
        }
        done += part;
    }
    return readable;
}

// Writes into the game's memory, code included (its protection lifted while it's written).
bool write(std::uint32_t address, const void* data, size_t size)
{
    void* at = reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
    DWORD old = 0;
    const bool unprotected = VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old) != 0;
    SIZE_T written = 0;
    const bool ok = WriteProcessMemory(GetCurrentProcess(), at, data, size, &written) && written == size;
    if (unprotected) {
        DWORD ignored = 0;
        VirtualProtect(at, size, old, &ignored);
    }
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return ok;
}

bool readable_protection(DWORD protect)
{
    if ((protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        return false;
    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

bool writable_protection(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

struct Region {
    std::uint32_t base = 0;
    std::uint32_t size = 0;
};

// The committed, readable memory between `low` and `high` (writable only, if asked), in order.
std::vector<Region> regions(std::uint32_t low, std::uint64_t high, bool writable_only)
{
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    std::uint64_t at = std::max<std::uint64_t>(low, reinterpret_cast<std::uintptr_t>(system.lpMinimumApplicationAddress));
    const std::uint64_t top = std::min<std::uint64_t>(high, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
                                                                system.lpMaximumApplicationAddress)) + 1);
    std::vector<Region> out;
    while (at < top) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(at)), &info, sizeof(info)) == 0)
            break;
        const std::uint64_t base = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        const std::uint64_t end = base + info.RegionSize;
        if (info.State == MEM_COMMIT && readable_protection(info.Protect) && (!writable_only || writable_protection(info.Protect))) {
            const std::uint64_t from = std::max(base, at), to = std::min(end, top);
            if (to > from)
                out.push_back({static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to - from)});
        }
        if (end <= at)
            break;
        at = end;
    }
    return out;
}

// ---- modules ---------------------------------------------------------------------------------

struct Module {
    std::string name;          // "FFXiMain.dll"
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    std::uint32_t preferred = 0; // the base its file asks for, as a disassembly of the file shows addresses
    std::wstring path;
};

std::mutex g_modules_lock;
std::vector<Module> g_modules;
ULONGLONG g_modules_at = 0;
std::map<std::wstring, std::uint32_t> g_preferred; // by path: the files' image bases, read once

// The image base in a DLL's file (its PE header), which a relocated DLL's own header in memory no longer has.
std::uint32_t file_image_base(const std::wstring& path)
{
    if (const auto it = g_preferred.find(path); it != g_preferred.end())
        return it->second;
    std::uint32_t base = 0;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        std::uint8_t header[4096] = {};
        DWORD got = 0;
        if (ReadFile(file, header, sizeof(header), &got, nullptr) && got >= sizeof(IMAGE_DOS_HEADER)) {
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(header);
            if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 &&
                static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS32) <= got) {
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(header + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE)
                    base = nt->OptionalHeader.ImageBase;
            }
        }
        CloseHandle(file);
    }
    g_preferred[path] = base;
    return base;
}

std::vector<Module> modules(bool fresh = false)
{
    const std::lock_guard lock(g_modules_lock);
    const ULONGLONG now = GetTickCount64();
    if (fresh || g_modules.empty() || now - g_modules_at > 2000) {
        std::vector<Module> list;
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            for (BOOL more = Module32FirstW(snapshot, &entry); more; more = Module32NextW(snapshot, &entry)) {
                Module module;
                module.name = narrow(entry.szModule);
                module.base = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(entry.modBaseAddr));
                module.size = entry.modBaseSize;
                module.path = entry.szExePath;
                module.preferred = file_image_base(module.path);
                if (module.preferred == 0)
                    module.preferred = module.base;
                list.push_back(std::move(module));
            }
            CloseHandle(snapshot);
        }
        std::sort(list.begin(), list.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
        g_modules = std::move(list);
        g_modules_at = now;
    }
    return g_modules;
}

bool module_at(std::uint32_t address, Module& out)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (const Module& module : modules(attempt == 1))
            if (address >= module.base && address - module.base < module.size) {
                out = module;
                return true;
            }
    }
    return false;
}

bool module_named(std::string_view name, Module& out)
{
    const std::string wanted = lower(std::string(name));
    for (int attempt = 0; attempt < 2; ++attempt) {
        for (const Module& module : modules(attempt == 1)) {
            const std::string have = lower(module.name);
            const std::string stem = have.substr(0, have.rfind('.'));
            if (have == wanted || stem == wanted) {
                out = module;
                return true;
            }
        }
    }
    return false;
}

// Exports, by address, of each module looked at: names for system functions.
std::mutex g_exports_lock;
std::map<std::uint32_t, std::map<std::uint32_t, std::string>> g_exports; // by module base

const std::map<std::uint32_t, std::string>& exports(const Module& module)
{
    const std::lock_guard lock(g_exports_lock);
    auto [it, added] = g_exports.try_emplace(module.base);
    if (!added)
        return it->second;
    std::map<std::uint32_t, std::string>& names = it->second;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!read(module.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !read(module.base + static_cast<std::uint32_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
        return names;
    const IMAGE_DATA_DIRECTORY& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    IMAGE_EXPORT_DIRECTORY table{};
    if (directory.VirtualAddress == 0 || !read(module.base + directory.VirtualAddress, &table, sizeof(table)))
        return names;
    const std::uint32_t count = std::min<std::uint32_t>(table.NumberOfNames, 20000);
    std::vector<std::uint32_t> name_rvas(count), function_rvas(std::min<std::uint32_t>(table.NumberOfFunctions, 65536));
    std::vector<std::uint16_t> ordinals(count);
    if (!read(module.base + table.AddressOfNames, name_rvas.data(), count * 4) ||
        !read(module.base + table.AddressOfNameOrdinals, ordinals.data(), count * 2) ||
        !read(module.base + table.AddressOfFunctions, function_rvas.data(), function_rvas.size() * 4))
        return names;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (ordinals[i] >= function_rvas.size())
            continue;
        const std::uint32_t rva = function_rvas[ordinals[i]];
        if (rva >= directory.VirtualAddress && rva < directory.VirtualAddress + directory.Size)
            continue; // forwarded elsewhere
        char name[128] = {};
        std::vector<bool> valid;
        read_partial(module.base + name_rvas[i], reinterpret_cast<std::uint8_t*>(name), valid, sizeof(name) - 1);
        names.emplace(module.base + rva, name);
    }
    return names;
}

// "FFXiMain.dll+0x1A2B3C" (with " (VA 0x101A2B3C)" when it isn't where its file asks to be), a system DLL's
// function ("KERNEL32.dll!CreateFileW+0x12"), or "0x0BADF00D" outside any module.
std::string where(std::uint32_t address)
{
    Module module;
    if (!module_at(address, module))
        return hex8(address);
    const std::uint32_t offset = address - module.base;
    std::string text = module.name + "+" + hex(offset);
    if (module.preferred != module.base)
        text += " (VA " + hex8(module.preferred + offset) + ")";
    const std::string folder = lower(narrow(module.path));
    if (folder.find("\\windows\\") != std::string::npos) {
        const auto& names = exports(module);
        auto it = names.upper_bound(address);
        if (it != names.begin()) {
            --it;
            if (address - it->first < 0x4000)
                return module.name + "!" + it->second + (address == it->first ? std::string() : "+" + hex(address - it->first));
        }
    }
    return text;
}

// "0x0A1A2B3C = FFXiMain.dll+0x1A2B3C", or the address alone outside modules.
std::string describe(std::uint32_t address)
{
    const std::string place = where(address);
    if (place.rfind("0x", 0) == 0)
        return place;
    return hex8(address) + " = " + place;
}

bool in_code(std::uint32_t address)
{
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(address)), &info, sizeof(info)) == 0)
        return false;
    const DWORD protect = info.Protect & 0xFF;
    return info.State == MEM_COMMIT &&
           (protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_WRITECOPY);
}

// A short string at `address`, quoted, if there is one there (ASCII or Shift-JIS, at least 3 characters).
std::string string_at(std::uint32_t address)
{
    std::uint8_t bytes[96];
    std::vector<bool> valid;
    if (read_partial(address, bytes, valid, sizeof(bytes)) == 0 || !valid[0])
        return {};
    size_t length = 0;
    while (length < sizeof(bytes) && valid[length] && bytes[length] != 0) {
        const std::uint8_t c = bytes[length];
        if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) { // a Shift-JIS lead byte and its trail
            if (length + 1 >= sizeof(bytes) || bytes[length + 1] < 0x40)
                return {};
            length += 2;
            continue;
        }
        if ((c < 0x20 && c != '\n' && c != '\r' && c != '\t') || c == 0x7F || (c >= 0x80 && c < 0xA1) || c >= 0xFD)
            return {};
        ++length;
    }
    if (length < 3 || (length < sizeof(bytes) && !valid[length]))
        return {};
    std::string text = utf8(std::string_view(reinterpret_cast<const char*>(bytes), std::min<size_t>(length, 72)));
    std::string quoted = "\"";
    for (const char c : text)
        quoted += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : c == '\t' ? std::string("\\t") : std::string(1, c);
    return quoted + (length > 72 ? "...\"" : "\"");
}

// What a 32-bit value points at, if anything worth saying: a function, code, a string.
std::string pointee(std::uint32_t value)
{
    if (value < 0x10000)
        return {};
    Module module;
    if (module_at(value, module)) {
        const std::string place = where(value);
        if (in_code(value))
            return place;
        const std::string text = string_at(value);
        return text.empty() ? place : place + " " + text;
    }
    return string_at(value);
}

// ---- the game's structures ---------------------------------------------------------------------

std::atomic<std::uint32_t> g_entities{0}, g_party{0}, g_player{0}, g_target{0};
std::atomic<unsigned> g_entity_count{0};
ULONGLONG g_addresses_at = 0;

// ---- address expressions ----------------------------------------------------------------------

// "[[FFXiMain.dll+0x4A1234]+0x10]+4": numbers in hex (#12 decimal), modules' bases, + - * ( ), [x] a pointer read,
// ghidra(VA) the file's address made the live one, and the game's structures by name.
class Expression {
public:
    explicit Expression(std::string_view text) : text_(text) {}

    std::uint32_t evaluate()
    {
        const std::uint64_t value = sum();
        space();
        if (at_ != text_.size())
            fail("can't read the address \"" + std::string(text_) + "\" past \"" + std::string(text_.substr(0, at_)) + "\"");
        return static_cast<std::uint32_t>(value);
    }

private:
    void space()
    {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t'))
            ++at_;
    }

    bool take(char c)
    {
        space();
        if (at_ < text_.size() && text_[at_] == c) {
            ++at_;
            return true;
        }
        return false;
    }

    std::uint64_t sum()
    {
        std::uint64_t value = product();
        for (;;) {
            if (take('+'))
                value += product();
            else if (take('-'))
                value -= product();
            else
                return value & 0xFFFFFFFFull;
        }
    }

    std::uint64_t product()
    {
        std::uint64_t value = factor();
        while (take('*'))
            value *= factor();
        return value & 0xFFFFFFFFull;
    }

    std::uint64_t factor()
    {
        space();
        if (at_ >= text_.size())
            fail("the address \"" + std::string(text_) + "\" ends too soon");
        if (take('-'))
            return (0 - factor()) & 0xFFFFFFFFull;
        if (take('(')) {
            const std::uint64_t value = sum();
            if (!take(')'))
                fail("a missing ) in \"" + std::string(text_) + "\"");
            return value;
        }
        if (take('[')) {
            const std::uint64_t pointer = sum();
            if (!take(']'))
                fail("a missing ] in \"" + std::string(text_) + "\"");
            std::uint32_t value = 0;
            if (!read32(static_cast<std::uint32_t>(pointer), value))
                fail("can't read the pointer at " + hex8(static_cast<std::uint32_t>(pointer)) + " (in \"" + std::string(text_) + "\")");
            return value;
        }
        if (text_[at_] == '#') {
            ++at_;
            const size_t start = at_;
            while (at_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[at_])))
                ++at_;
            if (start == at_)
                fail("a # without a decimal number in \"" + std::string(text_) + "\"");
            return std::strtoull(std::string(text_.substr(start, at_ - start)).c_str(), nullptr, 10) & 0xFFFFFFFFull;
        }
        const size_t start = at_;
        while (at_ < text_.size() &&
               (std::isalnum(static_cast<unsigned char>(text_[at_])) || text_[at_] == '_' || text_[at_] == '.' || text_[at_] == '$'))
            ++at_;
        const std::string word(text_.substr(start, at_ - start));
        if (word.empty())
            fail("can't read the address \"" + std::string(text_) + "\" at \"" + std::string(text_.substr(start)) + "\"");
        const std::string name = lower(word);
        if (name == "ghidra" || name == "va") {
            if (!take('('))
                fail(word + " needs ( ): ghidra(0x10123456)");
            const std::uint64_t va = sum();
            if (!take(')'))
                fail("a missing ) after " + word + "(");
            return from_file(static_cast<std::uint32_t>(va));
        }
        if (name == "entities" || name == "party" || name == "player" || name == "target") {
            const std::uint32_t value = name == "entities" ? g_entities.load()
                                        : name == "party"  ? g_party.load()
                                        : name == "player" ? g_player.load()
                                                           : g_target.load();
            if (value == 0)
                fail(name + " isn't found yet (is a character logged in?)");
            return value;
        }
        if (name.rfind("0x", 0) == 0) {
            if (name.size() == 2 || name.find_first_not_of("0123456789abcdef", 2) != std::string::npos)
                fail("\"" + word + "\" isn't a hex number");
            return std::strtoull(name.c_str() + 2, nullptr, 16) & 0xFFFFFFFFull;
        }
        Module module;
        if (module_named(name, module))
            return module.base;
        if (name.find_first_not_of("0123456789abcdef") == std::string::npos && name.size() <= 8)
            return std::strtoull(name.c_str(), nullptr, 16);
        fail("\"" + word + "\" isn't a number, a loaded module or a name this knows (entities, party, player, target, ghidra())");
    }

    // A file's address (its preferred base) as it's loaded now: the module whose file range has it.
    static std::uint32_t from_file(std::uint32_t va)
    {
        for (const Module& module : modules())
            if (va >= module.preferred && va - module.preferred < module.size)
                return module.base + (va - module.preferred);
        fail(hex8(va) + " isn't inside any module's file addresses");
    }

    std::string_view text_;
    size_t at_ = 0;
};

std::uint32_t address_of(const json::Value& value, const char* what)
{
    if (value.is_number()) {
        if (value.number < 0 || value.number > 4294967295.0)
            fail(std::string(what) + " is out of range");
        return static_cast<std::uint32_t>(value.number);
    }
    if (value.is_string())
        return Expression(value.string).evaluate();
    fail(std::string(what) + " should be an address: a number or an expression like \"FFXiMain.dll+0x1234\"");
}

std::uint32_t arg_address(const json::Value& args, const char* key)
{
    const json::Value* value = args.find(key);
    if (value == nullptr || value->is_null())
        fail(std::string("'") + key + "' is needed");
    return address_of(*value, key);
}

bool has(const json::Value& args, const char* key)
{
    const json::Value* value = args.find(key);
    return value != nullptr && !value->is_null();
}

std::int64_t arg_int(const json::Value& args, const char* key, std::int64_t fallback, std::int64_t low, std::int64_t high)
{
    const json::Value* value = args.find(key);
    std::int64_t result = fallback;
    if (value != nullptr && value->is_number())
        result = static_cast<std::int64_t>(value->number);
    else if (value != nullptr && value->is_string() && !value->string.empty())
        result = value->string.find_first_not_of("0123456789-") == std::string::npos
                     ? std::strtoll(value->string.c_str(), nullptr, 10)
                     : static_cast<std::int64_t>(Expression(value->string).evaluate());
    return std::clamp(result, low, high);
}

double arg_number(const json::Value& args, const char* key, double fallback)
{
    const json::Value* value = args.find(key);
    if (value != nullptr && value->is_number())
        return value->number;
    if (value != nullptr && value->is_string() && !value->string.empty()) {
        char* end = nullptr;
        const double parsed = std::strtod(value->string.c_str(), &end);
        if (end != nullptr && *end == '\0')
            return parsed;
        if (value->string.rfind("0x", 0) == 0)
            return static_cast<double>(std::strtoull(value->string.c_str() + 2, nullptr, 16));
        fail(std::string("'") + key + "' should be a number");
    }
    return fallback;
}

std::string arg_string(const json::Value& args, const char* key, std::string fallback = {})
{
    const json::Value* value = args.find(key);
    if (value != nullptr && value->is_string())
        return value->string;
    if (value != nullptr && value->is_number())
        return json::dump(*value);
    return fallback;
}

bool arg_bool(const json::Value& args, const char* key, bool fallback)
{
    const json::Value* value = args.find(key);
    if (value != nullptr && value->is_bool())
        return value->boolean;
    if (value != nullptr && value->is_string())
        return value->string == "true" || value->string == "1" || value->string == "yes";
    if (value != nullptr && value->is_number())
        return value->number != 0;
    return fallback;
}

// "8B 44 24 ?? 90" (spaces optional, ? or ?? for any byte): bytes and which of them must match.
void parse_pattern(const std::string& text, std::vector<std::uint8_t>& bytes, std::vector<bool>& mask)
{
    bytes.clear();
    mask.clear();
    for (size_t at = 0; at < text.size();) {
        const char c = text[at];
        if (c == ' ' || c == '\t' || c == ',' || c == '\n') {
            ++at;
            continue;
        }
        if (c == '?') {
            bytes.push_back(0);
            mask.push_back(false);
            at += at + 1 < text.size() && text[at + 1] == '?' ? 2 : 1;
            continue;
        }
        if (at + 1 >= text.size() || !std::isxdigit(static_cast<unsigned char>(c)) || !std::isxdigit(static_cast<unsigned char>(text[at + 1])))
            fail("\"" + text + "\" isn't hex bytes (two digits each, ?? for any byte)");
        bytes.push_back(static_cast<std::uint8_t>(std::strtoul(text.substr(at, 2).c_str(), nullptr, 16)));
        mask.push_back(true);
        at += 2;
    }
    if (bytes.empty())
        fail("no bytes in \"" + text + "\"");
}

// ---- results -----------------------------------------------------------------------------------

json::Value text_result(const std::string& text)
{
    json::Value result;
    json::Value& item = result["content"].push(json::Value::make_object());
    item["type"] = "text";
    item["text"] = text;
    result["isError"] = false;
    return result;
}

void need_frame(bool ran)
{
    if (!ran)
        fail("the game didn't draw a frame within 5 seconds (loading, or its window is minimised with drawing stopped); try again");
}

// ---- captured: packets and chat ----------------------------------------------------------------

struct PacketRecord {
    std::uint64_t seq = 0;
    ULONGLONG ms = 0;
    bool incoming = false;
    std::uint16_t id = 0;
    std::string data; // header first
};
std::mutex g_packets_lock;
std::deque<PacketRecord> g_packets;
std::uint64_t g_packet_seq = 0;
constexpr size_t kPacketLog = 4000;

struct ChatRecord {
    std::uint64_t seq = 0;
    SYSTEMTIME time{};
    int mode = 0;
    std::string text; // UTF-8, colour codes left out, auto-translate phrases spelled out
};
std::mutex g_chat_lock;
std::deque<ChatRecord> g_chat;
std::uint64_t g_chat_seq = 0;
constexpr size_t kChatLog = 2000;

// ---- hardware breakpoints: watch ---------------------------------------------------------------

struct Hit {
    std::uint32_t key = 0;    // what hits are grouped by: the instruction (data watches) or the caller (execute)
    std::uint32_t eip = 0;
    std::uint64_t count = 0;
    std::uint32_t regs[8] = {}; // eax ecx edx ebx esp ebp esi edi, the last time
    std::uint32_t eflags = 0;
    std::uint32_t stack[16] = {};
    bool stack_ok = false;
    DWORD thread = 0;
    ULONGLONG last_ms = 0;
};

struct Watch {
    bool active = false;
    std::uint32_t address = 0;
    unsigned kind = 0;   // DR7's R/W: 0 execute, 1 write, 3 read or write
    unsigned length = 1; // 1, 2 or 4 bytes
    std::uint64_t total = 0;
    std::uint64_t dropped = 0; // hits at more places than there's room for
    Hit hits[48];
    unsigned hit_count = 0;
    ULONGLONG set_ms = 0;
};

Watch g_watches[4];
std::atomic_flag g_watch_lock = ATOMIC_FLAG_INIT; // a spin lock: the exception handler can't wait on anything else
PVOID g_handler = nullptr;
std::mutex g_watch_setup; // one change at a time

struct SpinLock {
    SpinLock()
    {
        while (g_watch_lock.test_and_set(std::memory_order_acquire))
            YieldProcessor();
    }
    ~SpinLock() { g_watch_lock.clear(std::memory_order_release); }
};

bool copy_stack(std::uint32_t esp, std::uint32_t* out)
{
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<std::uintptr_t>(esp)), 16 * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void record(Watch& watch, const CONTEXT& context)
{
    std::uint32_t stack[16] = {};
    const bool stack_ok = copy_stack(context.Esp, stack);
    const std::uint32_t key = watch.kind == 0 ? stack[0] : context.Eip;
    const SpinLock lock;
    ++watch.total;
    Hit* hit = nullptr;
    for (unsigned i = 0; i < watch.hit_count; ++i)
        if (watch.hits[i].key == key)
            hit = &watch.hits[i];
    if (hit == nullptr) {
        if (watch.hit_count >= sizeof(watch.hits) / sizeof(watch.hits[0])) {
            ++watch.dropped;
            return;
        }
        hit = &watch.hits[watch.hit_count++];
        hit->key = key;
    }
    ++hit->count;
    hit->eip = context.Eip;
    const std::uint32_t regs[8] = {context.Eax, context.Ecx, context.Edx, context.Ebx, context.Esp, context.Ebp, context.Esi, context.Edi};
    std::memcpy(hit->regs, regs, sizeof(regs));
    hit->eflags = context.EFlags;
    std::memcpy(hit->stack, stack, sizeof(stack));
    hit->stack_ok = stack_ok;
    hit->thread = GetCurrentThreadId();
    hit->last_ms = GetTickCount64();
}

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info)
{
    if (info == nullptr || info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT& context = *info->ContextRecord;
    const bool have_dr6 = (context.ContextFlags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS;
    bool ours = false;
    for (unsigned i = 0; i < 4; ++i) {
        Watch& watch = g_watches[i];
        if (!watch.active)
            continue;
        const bool hit = have_dr6 ? (context.Dr6 & (1u << i)) != 0 : watch.kind == 0 && context.Eip == watch.address;
        if (!hit)
            continue;
        record(watch, context);
        ours = true;
    }
    if (!ours) {
        // A data breakpoint (DR6's B0-B3) that tripped just as its watch was cleared or replaced: no watch is active for it any
        // more, but the trap is already on its way. Let the thread go on, or the game ends on an unhandled single step.
        if (have_dr6 && (context.Dr6 & 0xF) != 0) {
            context.Dr6 = 0;
            context.EFlags |= 0x10000;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH; // someone else's single step
    }
    if (have_dr6)
        context.Dr6 = 0;
    context.EFlags |= 0x10000; // RF: an execute breakpoint lets its instruction run instead of stopping again
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Puts the watches into every thread's debug registers (but this one's: a thread can't set its own).
unsigned apply_watches()
{
    DWORD dr[4] = {};
    DWORD dr7 = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const Watch& watch = g_watches[i];
        if (!watch.active)
            continue;
        dr[i] = watch.address;
        const DWORD length = watch.kind == 0 ? 0 : watch.length == 1 ? 0 : watch.length == 2 ? 1 : 3;
        dr7 |= 1u << (i * 2);
        dr7 |= (static_cast<DWORD>(watch.kind) << (16 + i * 4)) | (length << (18 + i * 4));
    }
    unsigned applied = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
            continue;
        const HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
            continue;
        // nothing between the suspend and the resume allocates: the thread may hold the heap's lock
        if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
            CONTEXT context{};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(thread, &context)) {
                context.Dr0 = dr[0];
                context.Dr1 = dr[1];
                context.Dr2 = dr[2];
                context.Dr3 = dr[3];
                context.Dr6 = 0;
                context.Dr7 = dr7;
                if (SetThreadContext(thread, &context))
                    ++applied;
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    return applied;
}

// ---- value scans (value_scan) ------------------------------------------------------------------

enum class ValueType { U8, I8, U16, I16, U32, I32, F32, F64 };

struct Scan {
    bool active = false;
    ValueType type = ValueType::U32;
    std::string scope;
    std::vector<std::uint32_t> addresses;
    std::vector<std::uint64_t> values; // as they were at the last scan
    unsigned scans = 0;
};
std::mutex g_scan_lock;
Scan g_scan;

// ---- the screen --------------------------------------------------------------------------------

std::string base64(const std::vector<std::uint8_t>& data)
{
    static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += kAlphabet[n & 63];
    }
    if (i + 1 == data.size()) {
        const std::uint32_t n = data[i] << 16;
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const std::uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += kAlphabet[(n >> 6) & 63];
        out += '=';
    }
    return out;
}

// 24-bit BGR rows, scaled to `width` x `height`, as a PNG or JPEG.
bool encode_image(const std::vector<std::uint8_t>& pixels, UINT width, UINT height, UINT stride, UINT out_width, UINT out_height,
                  bool jpeg, std::vector<std::uint8_t>& out, std::string& error)
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* bitmap = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result))
        result = factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat24bppBGR, stride, static_cast<UINT>(pixels.size()),
                                                 const_cast<BYTE*>(pixels.data()), &bitmap);
    if (SUCCEEDED(result))
        result = factory->CreateBitmapScaler(&scaler);
    if (SUCCEEDED(result))
        result = scaler->Initialize(bitmap, out_width, out_height, WICBitmapInterpolationModeFant);
    if (SUCCEEDED(result))
        result = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (SUCCEEDED(result))
        result = factory->CreateEncoder(jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(result))
        result = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(result))
        result = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(result) && jpeg && properties != nullptr) {
        PROPBAG2 option{};
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = 0.85f;
        properties->Write(1, &option, &value);
    }
    if (SUCCEEDED(result))
        result = frame->Initialize(properties);
    if (SUCCEEDED(result))
        result = frame->SetSize(out_width, out_height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(result))
        result = frame->SetPixelFormat(&format);
    if (SUCCEEDED(result))
        result = frame->WriteSource(scaler, nullptr);
    if (SUCCEEDED(result))
        result = frame->Commit();
    if (SUCCEEDED(result))
        result = encoder->Commit();
    if (SUCCEEDED(result)) {
        HGLOBAL memory = nullptr;
        result = GetHGlobalFromStream(stream, &memory);
        STATSTG stat{};
        if (SUCCEEDED(result))
            result = stream->Stat(&stat, STATFLAG_NONAME);
        if (SUCCEEDED(result)) {
            const auto* bytes = static_cast<const std::uint8_t*>(GlobalLock(memory));
            if (bytes != nullptr) {
                out.assign(bytes, bytes + static_cast<size_t>(stat.cbSize.QuadPart));
                GlobalUnlock(memory);
            } else {
                result = E_FAIL;
            }
        }
    }
    if (properties != nullptr) properties->Release();
    if (frame != nullptr) frame->Release();
    if (encoder != nullptr) encoder->Release();
    if (stream != nullptr) stream->Release();
    if (scaler != nullptr) scaler->Release();
    if (bitmap != nullptr) bitmap->Release();
    if (factory != nullptr) factory->Release();
    if (com)
        CoUninitialize();
    if (FAILED(result)) {
        error = strf("the picture couldn't be encoded (0x%08lX)", static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

// ==== the tools ===================================================================================

json::Value game_state(const json::Value&)
{
    std::string text;
    need_frame(on_game_thread([&] {
        game::PartyMember members[18];
        game::Alliance alliance;
        game::Player player;
        game::Target target;
        const bool have_party = game::party(members, alliance);
        const bool have_player = game::player(player);
        const bool have_target = game::target(target);
        const game::Addresses where_ = game::addresses();
        Module ffxi;
        if (module_named("FFXiMain.dll", ffxi))
            text += strf("FFXiMain.dll at %s (its file's base %s)\n", hex8(ffxi.base).c_str(), hex8(ffxi.preferred).c_str());
        if (!have_party || !members[0].active || members[0].id == 0) {
            text += "No character is logged in (the title screen, the lobby, or zoning).\n";
        } else {
            const game::PartyMember& me = members[0];
            text += strf("Character: %s (id %s, index %s) %s%u/%s%u, zone %u\n", utf8(me.name).c_str(), hex(me.id).c_str(),
                           hex(me.target_index).c_str(), job_name(me.main_job), me.main_job_level, job_name(me.sub_job),
                           me.sub_job_level, me.zone);
            text += strf("HP %u/%u (%u%%)  MP %u/%u (%u%%)  TP %u\n", me.hp, have_player ? player.hp_max : 0, me.hpp, me.mp,
                           have_player ? player.mp_max : 0, me.mpp, me.tp);
            game::Entity self;
            if (game::entity(me.target_index, self))
                text += strf("Position: x %.2f, y %.2f, z %.2f, heading %.3f; speed %.2f; status %u\n", self.x, self.y, self.z,
                               self.heading, self.speed, self.status);
            if (have_player) {
                static const char* const kStats[] = {"STR", "DEX", "VIT", "AGI", "INT", "MND", "CHR"};
                text += "Stats:";
                for (int i = 0; i < 7; ++i)
                    text += strf(" %s %d%+d", kStats[i], player.stats[i], player.stat_modifiers[i]);
                text += strf("; attack %d, defense %d, title %u, nation %u\n", player.attack, player.defense, player.title, player.nation);
                std::string buffs;
                for (const std::int16_t buff : player.buffs)
                    if (buff >= 0 && buff != 255)
                        buffs += (buffs.empty() ? "" : ", ") + std::to_string(buff);
                text += "Buffs (status effect ids): " + (buffs.empty() ? std::string("none") : buffs) + "\n";
            }
        }
        if (have_target && target.active && target.index != 0) {
            game::Entity e;
            if (game::entity(target.index, e))
                text += strf("Target: \"%s\" index %s id %s, HP %u%%, %.1f yalms%s%s\n", utf8(e.name).c_str(), hex(e.index).c_str(),
                               hex(e.id).c_str(), e.hpp, std::sqrt(std::max(0.0f, e.distance)), target.locked ? ", locked on" : "",
                               target.is_sub_target ? ", choosing a sub target" : "");
        } else if (have_target) {
            text += std::string("No target") + (target.menu_open ? " (a menu is open)" : "") + "\n";
        }
        if (have_party && members[0].active) {
            text += "Party:\n";
            for (const game::PartyMember& m : members)
                if (m.active && m.id != 0)
                    text += strf("  %2u. %s %s%u/%s%u HP %u (%u%%) MP %u TP %u zone %u (id %s, index %s)\n", m.slot, utf8(m.name).c_str(),
                                   job_name(m.main_job), m.main_job_level, job_name(m.sub_job), m.sub_job_level, m.hp, m.hpp, m.mp,
                                   m.tp, m.zone, hex(m.id).c_str(), hex(m.target_index).c_str());
        }
        text += strf("Structures: entities %s (%u slots, each a pointer to an entity), party %s, player %s, target pointer %s\n",
                       hex8(static_cast<std::uint32_t>(where_.entities)).c_str(), where_.entity_count,
                       hex8(static_cast<std::uint32_t>(where_.party)).c_str(), hex8(static_cast<std::uint32_t>(where_.player)).c_str(),
                       hex8(static_cast<std::uint32_t>(where_.target_pointer)).c_str());
        if (have_party && members[0].active && where_.entities != 0) {
            std::uint32_t mine = 0;
            if (read32(static_cast<std::uint32_t>(where_.entities) + members[0].target_index * 4, mine))
                text += "My entity: " + hex8(mine) + "\n";
        }
    }));
    return text_result(text);
}

const char* entity_kind(const game::Entity& e)
{
    if ((e.spawn_flags & 0x0D) == 0x0D)
        return "self";
    if (e.spawn_flags & 0x01)
        return "pc";
    if (e.spawn_flags & 0x10)
        return "monster";
    if (e.spawn_flags & 0x02)
        return "npc";
    return "other";
}

json::Value entities(const json::Value& args)
{
    const std::string name = lower(arg_string(args, "name"));
    const std::string kind = lower(arg_string(args, "kind", "any"));
    const double max_distance = arg_number(args, "max_distance", 0);
    const int limit = static_cast<int>(arg_int(args, "limit", 40, 1, 500));
    const bool one = has(args, "index");
    const unsigned only = one ? static_cast<unsigned>(arg_int(args, "index", 0, 0, 0xFFFF)) : 0;
    struct Row {
        game::Entity e;
        std::uint32_t address = 0;
    };
    std::vector<Row> rows;
    std::uint32_t array = 0;
    need_frame(on_game_thread([&] {
        array = static_cast<std::uint32_t>(game::addresses().entities);
        const unsigned count = game::entity_count();
        for (unsigned i = one ? only : 0; i < (one ? std::min(only + 1, count) : count); ++i) {
            Row row;
            if (!game::entity(i, row.e))
                continue;
            if (array != 0)
                read32(array + i * 4, row.address);
            rows.push_back(std::move(row));
        }
    }));
    if (array == 0)
        fail("the game's entity array isn't found yet (is a character logged in?)");
    if (one) {
        if (rows.empty())
            fail("no entity at index " + hex(only));
        const game::Entity& e = rows[0].e;
        std::string text = strf("Entity index %s, id %s, \"%s\" (%s)\n", hex(e.index).c_str(), hex(e.id).c_str(), utf8(e.name).c_str(),
                                  entity_kind(e));
        text += "Address: " + hex8(rows[0].address) + " (the entity array " + hex8(array) + " + index*4 points here)\n";
        text += strf("Position x %.3f y %.3f z %.3f heading %.4f; distance %.2f yalms; speed %.2f\n", e.x, e.y, e.z, e.heading,
                       std::sqrt(std::max(0.0f, e.distance)), e.speed);
        text += strf("HP %u%%, type %u, race %u, status %u, spawn flags %s, render flags %s, name flags %s\n", e.hpp, e.type, e.race,
                       e.status, hex(e.spawn_flags).c_str(), hex(e.render_flags).c_str(), hex(e.name_flags).c_str());
        text += strf("Model size %.2f, hitbox %.2f, claim %s, targeting index %s, pet index %s\n", e.model_size, e.hitbox_size,
                       hex(e.claim).c_str(), hex(e.target_index).c_str(), hex(e.pet_index).c_str());
        text += "Models (hair head body hands legs feet main sub ranged):";
        for (const std::uint16_t model : e.models)
            text += " " + hex(model);
        return text_result(text + "\n");
    }
    std::vector<const Row*> shown;
    for (const Row& row : rows) {
        const game::Entity& e = row.e;
        if (!name.empty() && lower(utf8(e.name)).find(name) == std::string::npos)
            continue;
        if (kind != "any" && kind != entity_kind(e) && !(kind == "pc" && std::strcmp(entity_kind(e), "self") == 0))
            continue;
        if (max_distance > 0 && std::sqrt(std::max(0.0f, e.distance)) > max_distance)
            continue;
        if (e.name.empty() && e.id == 0)
            continue;
        shown.push_back(&row);
    }
    std::sort(shown.begin(), shown.end(), [](const Row* a, const Row* b) { return a->e.distance < b->e.distance; });
    std::string text = strf("%zu entities match (of %zu); nearest first:\n", shown.size(), rows.size());
    int n = 0;
    for (const Row* row : shown) {
        if (n++ >= limit) {
            text += strf("... %zu more (raise limit, or filter)\n", shown.size() - static_cast<size_t>(limit));
            break;
        }
        const game::Entity& e = row->e;
        text += strf("%s id %s \"%s\" %s HP %u%% %.1fy (%.2f, %.2f, %.2f) status %u at %s\n", hex(e.index, 3).c_str(), hex(e.id, 8).c_str(),
                       utf8(e.name).c_str(), entity_kind(e), e.hpp, std::sqrt(std::max(0.0f, e.distance)), e.x, e.y, e.z, e.status,
                       hex8(row->address).c_str());
    }
    return text_result(text);
}

json::Value read_memory(const json::Value& args)
{
    const std::uint32_t address = arg_address(args, "address");
    const std::string type = lower(arg_string(args, "type", "hex"));
    const size_t size = static_cast<size_t>(arg_int(args, "size", type == "string" ? 256 : 64, 1, 16384));
    std::vector<std::uint8_t> bytes(size);
    std::vector<bool> valid;
    if (read_partial(address, bytes.data(), valid, size) == 0)
        fail("nothing at " + describe(address) + " can be read (unmapped or protected)");
    std::string text = describe(address) + ":\n";
    const auto ok = [&](size_t at, size_t n) {
        for (size_t i = at; i < at + n; ++i)
            if (i >= size || !valid[i])
                return false;
        return true;
    };
    if (type == "hex") {
        for (size_t row = 0; row < size; row += 16) {
            text += hex8(address + static_cast<std::uint32_t>(row)) + "  ";
            std::string ascii;
            for (size_t i = row; i < row + 16; ++i) {
                if (i >= size) {
                    text += "   ";
                    continue;
                }
                text += valid[i] ? strf("%02X ", bytes[i]) : "?? ";
                const std::uint8_t c = bytes[i];
                ascii += !valid[i] ? ' ' : (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
                if (i == row + 7)
                    text += " ";
            }
            text += " |" + ascii + "|\n";
        }
        return text_result(text);
    }
    if (type == "string" || type == "sjis") {
        size_t length = 0;
        while (length < size && valid[length] && bytes[length] != 0)
            ++length;
        text += "\"" + utf8(std::string_view(reinterpret_cast<const char*>(bytes.data()), length)) + "\"";
        text += strf(" (%zu bytes%s)\n", length, length == size ? ", no terminator within size" : "");
        return text_result(text);
    }
    if (type == "utf16") {
        std::wstring wide;
        for (size_t i = 0; i + 1 < size && ok(i, 2); i += 2) {
            wchar_t c = 0;
            std::memcpy(&c, bytes.data() + i, 2);
            if (c == 0)
                break;
            wide += c;
        }
        return text_result(text + "\"" + narrow(wide) + "\"\n");
    }
    size_t width = 4;
    if (type == "u8" || type == "i8")
        width = 1;
    else if (type == "u16" || type == "i16")
        width = 2;
    else if (type == "f64" || type == "double")
        width = 8;
    else if (type != "u32" && type != "i32" && type != "f32" && type != "float" && type != "ptr" && type != "pointer")
        fail("type should be hex, u8, i8, u16, i16, u32, i32, f32, f64, ptr, string (Shift-JIS) or utf16");
    const bool pointers = type == "ptr" || type == "pointer";
    const size_t per_row = pointers ? 1 : width == 1 ? 16 : width == 2 ? 8 : width == 8 ? 2 : 4;
    for (size_t row = 0; row < size; row += per_row * width) {
        text += strf("+%-5s ", hex(row).c_str());
        for (size_t i = row; i < row + per_row * width && i + width <= size; i += width) {
            if (!ok(i, width)) {
                text += "?? ";
                continue;
            }
            const std::uint8_t* p = bytes.data() + i;
            std::uint64_t raw = 0;
            std::memcpy(&raw, p, width);
            if (type == "u8" || type == "u16" || type == "u32")
                text += std::to_string(raw) + " ";
            else if (type == "i8")
                text += std::to_string(static_cast<std::int8_t>(raw)) + " ";
            else if (type == "i16")
                text += std::to_string(static_cast<std::int16_t>(raw)) + " ";
            else if (type == "i32")
                text += std::to_string(static_cast<std::int32_t>(raw)) + " ";
            else if (type == "f32" || type == "float") {
                float f = 0;
                std::memcpy(&f, p, 4);
                text += strf("%g ", f);
            } else if (type == "f64" || type == "double") {
                double d = 0;
                std::memcpy(&d, p, 8);
                text += strf("%g ", d);
            } else {
                const std::uint32_t value = static_cast<std::uint32_t>(raw);
                const std::string what = pointee(value);
                text += hex8(value) + (what.empty() ? "" : "  -> " + what);
            }
        }
        text += "\n";
    }
    return text_result(text);
}

// A value or bytes from the arguments (hex, or value and type), for writes and searches.
std::vector<std::uint8_t> bytes_of(const json::Value& args, std::string& described)
{
    if (has(args, "hex")) {
        std::vector<std::uint8_t> bytes;
        std::vector<bool> mask;
        parse_pattern(arg_string(args, "hex"), bytes, mask);
        if (std::find(mask.begin(), mask.end(), false) != mask.end())
            fail("?? isn't allowed here: give every byte");
        described = strf("%zu bytes", bytes.size());
        return bytes;
    }
    if (has(args, "text")) {
        const std::string encoding = lower(arg_string(args, "encoding", "sjis"));
        const std::string text = arg_string(args, "text");
        std::vector<std::uint8_t> bytes;
        if (encoding == "utf16") {
            std::wstring wide(text.size(), L'\0');
            wide.resize(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                                                                static_cast<int>(wide.size()))));
            bytes.resize(wide.size() * 2);
            std::memcpy(bytes.data(), wide.data(), bytes.size());
        } else {
            const std::string converted = encoding == "utf8" ? text : sjis(text);
            bytes.assign(converted.begin(), converted.end());
        }
        if (arg_bool(args, "terminate", false))
            bytes.insert(bytes.end(), encoding == "utf16" ? 2 : 1, 0);
        described = "\"" + text + "\" (" + encoding + ")";
        return bytes;
    }
    if (!has(args, "value"))
        fail("give hex, text, or value (with type)");
    const std::string type = lower(arg_string(args, "type", "u32"));
    const double value = arg_number(args, "value", 0);
    std::vector<std::uint8_t> bytes;
    const auto put = [&](auto v) {
        bytes.resize(sizeof(v));
        std::memcpy(bytes.data(), &v, sizeof(v));
    };
    if (type == "u8" || type == "i8")
        put(static_cast<std::uint8_t>(static_cast<std::int64_t>(value)));
    else if (type == "u16" || type == "i16")
        put(static_cast<std::uint16_t>(static_cast<std::int64_t>(value)));
    else if (type == "u32" || type == "i32" || type == "ptr")
        put(static_cast<std::uint32_t>(static_cast<std::int64_t>(value)));
    else if (type == "f32" || type == "float")
        put(static_cast<float>(value));
    else if (type == "f64" || type == "double")
        put(value);
    else
        fail("type should be u8, i8, u16, i16, u32, i32, f32 or f64");
    described = strf("%s %g", type.c_str(), value);
    return bytes;
}

std::string hex_bytes(const std::uint8_t* data, size_t size)
{
    std::string out;
    for (size_t i = 0; i < size; ++i)
        out += strf(i == 0 ? "%02X" : " %02X", data[i]);
    return out;
}

json::Value write_memory(const json::Value& args)
{
    const std::uint32_t address = arg_address(args, "address");
    std::string described;
    const std::vector<std::uint8_t> bytes = bytes_of(args, described);
    if (bytes.size() > 65536)
        fail("at most 64 KB at a time");
    std::vector<std::uint8_t> before(bytes.size());
    if (!read(address, before.data(), before.size()))
        fail("can't read " + describe(address) + " to write it");
    if (!write(address, bytes.data(), bytes.size()))
        fail(strf("couldn't write %zu bytes at %s (error %lu)", bytes.size(), describe(address).c_str(), GetLastError()));
    std::string text = "Wrote " + described + " at " + describe(address) + "\n";
    if (bytes.size() <= 64)
        text += "before: " + hex_bytes(before.data(), before.size()) + "\nafter:  " + hex_bytes(bytes.data(), bytes.size()) + "\n";
    return text_result(text);
}

// One line of disassembly, with what it refers to.
std::string listing(const disasm::Instruction& instruction, const std::uint8_t* bytes)
{
    std::string line = hex8(instruction.address) + "  " + where(instruction.address);
    line.resize(std::max<size_t>(line.size() + 1, 44), ' ');
    std::string code = hex_bytes(bytes, std::min(instruction.length, 8u));
    if (instruction.length > 8)
        code += "..";
    code.resize(std::max<size_t>(code.size() + 1, 26), ' ');
    line += code + instruction.text;
    std::string note;
    if (instruction.has_target) {
        note = where(instruction.target);
    } else if (instruction.has_absolute) {
        std::uint32_t value = 0;
        note = where(instruction.absolute);
        if (instruction.text.find("*") == std::string::npos && read32(instruction.absolute, value)) {
            const std::string what = pointee(value);
            note += " = " + hex8(value) + (what.empty() ? "" : " " + what);
        }
    }
    if (instruction.has_immediate) {
        const std::string what = pointee(instruction.immediate);
        if (!what.empty())
            note += (note.empty() ? "" : "; ") + hex8(instruction.immediate) + " " + what;
    }
    if (!note.empty())
        line += "    ; " + note;
    return line;
}

json::Value disassemble(const json::Value& args)
{
    const std::uint32_t address = arg_address(args, "address");
    const int count = static_cast<int>(arg_int(args, "count", 40, 1, 1000));
    const bool stop_at_ret = arg_bool(args, "stop_at_ret", false);
    std::vector<std::uint8_t> bytes(static_cast<size_t>(count) * 15 + 16);
    std::vector<bool> valid;
    read_partial(address, bytes.data(), valid, bytes.size());
    if (!valid[0])
        fail(describe(address) + " can't be read");
    std::string text;
    size_t at = 0;
    for (int n = 0; n < count && at < bytes.size(); ++n) {
        size_t readable = 0;
        while (at + readable < bytes.size() && valid[at + readable] && readable < 15)
            ++readable;
        if (readable == 0) {
            text += hex8(address + static_cast<std::uint32_t>(at)) + "  (unreadable)\n";
            break;
        }
        disasm::Instruction instruction;
        if (!disasm::decode(bytes.data() + at, readable, address + static_cast<std::uint32_t>(at), instruction)) {
            instruction.address = address + static_cast<std::uint32_t>(at);
            instruction.length = 1;
            instruction.text = strf("db 0x%02X", bytes[at]);
        }
        text += listing(instruction, bytes.data() + at) + "\n";
        at += instruction.length;
        if (stop_at_ret && instruction.ret)
            break;
    }
    return text_result(text);
}

struct Section {
    std::string name;
    std::uint32_t base = 0;
    std::uint32_t size = 0;
    DWORD characteristics = 0;
};

std::vector<Section> sections(const Module& module)
{
    std::vector<Section> out;
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS32 nt{};
    if (!read(module.base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !read(module.base + static_cast<std::uint32_t>(dos.e_lfanew), &nt, sizeof(nt)) || nt.Signature != IMAGE_NT_SIGNATURE)
        return out;
    const std::uint32_t first = module.base + static_cast<std::uint32_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                                nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections && i < 96; ++i) {
        IMAGE_SECTION_HEADER header{};
        if (!read(first + i * sizeof(header), &header, sizeof(header)))
            break;
        Section section;
        section.name.assign(reinterpret_cast<const char*>(header.Name), strnlen(reinterpret_cast<const char*>(header.Name), 8));
        section.base = module.base + header.VirtualAddress;
        section.size = std::max(header.Misc.VirtualSize, header.SizeOfRawData);
        section.characteristics = header.Characteristics;
        out.push_back(section);
    }
    return out;
}

// The module's code: its executable sections, or the whole image when its header says none are (a packer's).
std::vector<Region> code_of(const Module& module)
{
    std::vector<Region> out;
    for (const Section& section : sections(module))
        if (section.characteristics & IMAGE_SCN_MEM_EXECUTE)
            out.push_back({section.base, section.size});
    if (out.empty())
        out.push_back({module.base, module.size});
    return out;
}

json::Value list_modules(const json::Value& args)
{
    const std::string name = arg_string(args, "name");
    if (!name.empty()) {
        Module module;
        if (!module_named(name, module))
            fail("no module " + name + " is loaded");
        std::string text = strf("%s at %s, %s bytes; its file's base %s (live = file VA - %s + %s)\n%s\n", module.name.c_str(),
                                  hex8(module.base).c_str(), hex(module.size).c_str(), hex8(module.preferred).c_str(),
                                  hex8(module.preferred).c_str(), hex8(module.base).c_str(), narrow(module.path).c_str());
        for (const Section& s : sections(module)) {
            std::string flags;
            if (s.characteristics & IMAGE_SCN_MEM_EXECUTE) flags += "x";
            if (s.characteristics & IMAGE_SCN_MEM_READ) flags += "r";
            if (s.characteristics & IMAGE_SCN_MEM_WRITE) flags += "w";
            text += strf("  %-8s %s +%s, %s bytes, %s\n", s.name.c_str(), hex8(s.base).c_str(), hex(s.base - module.base).c_str(),
                           hex(s.size).c_str(), flags.c_str());
        }
        text += strf("  %zu named exports\n", exports(module).size());
        return text_result(text);
    }
    std::string text;
    for (const Module& module : modules(true))
        text += strf("%s  %s bytes  %-28s %s\n", hex8(module.base).c_str(), hex(module.size).c_str(), module.name.c_str(),
                       module.preferred != module.base ? ("(file base " + hex8(module.preferred) + ")").c_str() : "");
    return text_result(text);
}

// Each match of a pattern in `regions`, in order, up to `limit`.
std::vector<std::uint32_t> find_all(const std::vector<Region>& where_, const std::vector<std::uint8_t>& bytes, const std::vector<bool>& mask,
                                    size_t limit, size_t alignment = 1)
{
    std::vector<std::uint32_t> found;
    std::vector<std::uint8_t> chunk;
    const size_t n = bytes.size();
    // what's searched for lives in this process too: never report the search's own copies
    const auto own = [&](std::uint32_t at) {
        const auto inside = [&](const void* p, size_t size) {
            const auto start = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p));
            return at >= start && at < start + size;
        };
        return inside(bytes.data(), bytes.size()) || inside(chunk.data(), chunk.capacity());
    };
    for (const Region& region : where_) {
        constexpr size_t kStep = 1 << 20;
        for (std::uint64_t offset = 0; offset < region.size && found.size() < limit; offset += kStep) {
            const size_t want = static_cast<size_t>(std::min<std::uint64_t>(kStep + n - 1, region.size - offset));
            if (want < n)
                break;
            chunk.resize(want);
            const std::uint32_t start = region.base + static_cast<std::uint32_t>(offset);
            if (!read(start, chunk.data(), want))
                continue;
            const std::uint8_t first = bytes[0];
            const bool first_fixed = mask[0];
            for (size_t i = 0; i + n <= want && found.size() < limit; i += alignment) {
                if (first_fixed && chunk[i] != first)
                    continue;
                bool match = true;
                for (size_t j = 1; j < n; ++j)
                    if (mask[j] && chunk[i + j] != bytes[j]) {
                        match = false;
                        break;
                    }
                if (match && !own(start + static_cast<std::uint32_t>(i)))
                    found.push_back(start + static_cast<std::uint32_t>(i));
            }
        }
        if (found.size() >= limit)
            break;
    }
    return found;
}

std::vector<Region> scope_regions(const std::string& scope, std::string& described)
{
    const std::string name = lower(scope);
    if (name.empty() || name == "all") {
        described = "all readable memory";
        return regions(0, 0x100000000ull, false);
    }
    if (name == "writable" || name == "heap") {
        described = "all writable memory";
        return regions(0, 0x100000000ull, true);
    }
    Module module;
    if (!module_named(scope, module))
        fail("scope should be all, writable, or a loaded module's name (\"" + scope + "\" isn't loaded)");
    described = module.name;
    return regions(module.base, static_cast<std::uint64_t>(module.base) + module.size, false);
}

json::Value find_pattern(const json::Value& args)
{
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;
    parse_pattern(arg_string(args, "pattern"), bytes, mask);
    const std::string module_name = arg_string(args, "module", "FFXiMain.dll");
    const size_t limit = static_cast<size_t>(arg_int(args, "limit", 20, 1, 1000));
    std::vector<Region> where_;
    std::string described;
    if (lower(module_name) == "all") {
        where_ = regions(0, 0x100000000ull, false);
        described = "all readable memory";
    } else {
        Module module;
        if (!module_named(module_name, module))
            fail("no module " + module_name + " is loaded");
        where_ = code_of(module);
        described = module.name + "'s code";
    }
    const std::vector<std::uint32_t> found = find_all(where_, bytes, mask, limit + 1);
    std::string text = strf("%zu%s match%s in %s:\n", std::min(found.size(), limit), found.size() > limit ? "+" : "",
                              found.size() == 1 ? "" : "es", described.c_str());
    for (size_t i = 0; i < found.size() && i < limit; ++i)
        text += describe(found[i]) + "\n";
    return text_result(text);
}

json::Value search_memory(const json::Value& args)
{
    std::string described;
    std::vector<std::uint8_t> bytes;
    std::vector<bool> mask;
    if (has(args, "hex")) {
        parse_pattern(arg_string(args, "hex"), bytes, mask);
        described = arg_string(args, "hex");
    } else {
        bytes = bytes_of(args, described);
        mask.assign(bytes.size(), true);
    }
    std::string scope;
    const std::vector<Region> where_ = scope_regions(arg_string(args, "scope", "all"), scope);
    const size_t limit = static_cast<size_t>(arg_int(args, "limit", 50, 1, 5000));
    const size_t alignment = arg_bool(args, "aligned", false) ? std::min<size_t>(bytes.size(), 4) : 1;
    const std::vector<std::uint32_t> found = find_all(where_, bytes, mask, limit + 1, alignment);
    std::string text = strf("%zu%s match%s for %s in %s:\n", std::min(found.size(), limit), found.size() > limit ? "+" : "",
                              found.size() == 1 ? "" : "es", described.c_str(), scope.c_str());
    for (size_t i = 0; i < found.size() && i < limit; ++i) {
        text += describe(found[i]);
        if (has(args, "text")) {
            const std::string s = string_at(found[i]);
            if (!s.empty())
                text += "  " + s;
        }
        text += "\n";
    }
    return text_result(text);
}

// The instruction that contains `bytes_at` (a reference's 4 bytes), decoded from a few bytes back.
bool containing(std::uint32_t bytes_at, std::uint32_t target, disasm::Instruction& out, std::vector<std::uint8_t>& code)
{
    for (std::uint32_t back = 1; back <= 11; ++back) {
        const std::uint32_t start = bytes_at - back;
        std::uint8_t buffer[16];
        if (!read(start, buffer, sizeof(buffer)))
            continue;
        disasm::Instruction instruction;
        if (!disasm::decode(buffer, sizeof(buffer), start, instruction))
            continue;
        if (start + instruction.length < bytes_at + 4)
            continue;
        if ((instruction.has_absolute && instruction.absolute == target) || (instruction.has_immediate && instruction.immediate == target)) {
            out = instruction;
            code.assign(buffer, buffer + instruction.length);
            return true;
        }
    }
    return false;
}

json::Value find_references(const json::Value& args)
{
    const std::uint32_t target = arg_address(args, "target");
    const std::string module_name = arg_string(args, "module", "FFXiMain.dll");
    const size_t limit = static_cast<size_t>(arg_int(args, "limit", 100, 1, 2000));
    Module module;
    if (!module_named(module_name, module))
        fail("no module " + module_name + " is loaded");
    std::string text;
    size_t count = 0;
    // calls and jumps (E8, E9, 0F 8x: a 32-bit displacement from the next instruction)
    for (const Region& region : code_of(module)) {
        std::vector<std::uint8_t> code(region.size);
        std::vector<bool> valid;
        read_partial(region.base, code.data(), valid, code.size());
        for (size_t i = 0; i + 5 <= code.size() && count < limit; ++i) {
            const std::uint8_t op = code[i];
            size_t length = 0;
            const char* kind = nullptr;
            if (op == 0xE8)
                length = 5, kind = "call";
            else if (op == 0xE9)
                length = 5, kind = "jmp";
            else if (op == 0x0F && i + 6 <= code.size() && (code[i + 1] & 0xF0) == 0x80)
                length = 6, kind = "jcc";
            if (kind == nullptr || !valid[i])
                continue;
            std::int32_t rel = 0;
            std::memcpy(&rel, code.data() + i + length - 4, 4);
            const std::uint32_t from = region.base + static_cast<std::uint32_t>(i);
            if (from + static_cast<std::uint32_t>(length) + static_cast<std::uint32_t>(rel) != target)
                continue;
            disasm::Instruction instruction;
            if (!disasm::decode(code.data() + i, code.size() - i, from, instruction))
                continue;
            text += listing(instruction, code.data() + i) + "\n";
            ++count;
        }
    }
    // the address itself, 4 bytes anywhere in the module: operands in code (push offset, mov eax, [x]) and
    // pointers in data (vtables, tables of functions)
    std::uint8_t pattern[4];
    std::memcpy(pattern, &target, 4);
    const std::vector<std::uint8_t> bytes(pattern, pattern + 4);
    const std::vector<bool> mask(4, true);
    const std::vector<std::uint32_t> found =
        find_all(regions(module.base, static_cast<std::uint64_t>(module.base) + module.size, false), bytes, mask, limit - std::min(count, limit) + 1);
    for (const std::uint32_t at : found) {
        if (count >= limit)
            break;
        disasm::Instruction instruction;
        std::vector<std::uint8_t> code;
        if (in_code(at) && containing(at, target, instruction, code))
            text += listing(instruction, code.data()) + "\n";
        else
            text += describe(at) + "  (data: a pointer to it)\n";
        ++count;
    }
    if (count == 0)
        return text_result("No references to " + describe(target) + " in " + module.name +
                           " (it may be reached through a register, a vtable outside the module, or computed)");
    return text_result(strf("%zu reference%s to %s in %s:\n", count, count == 1 ? "" : "s", describe(target).c_str(), module.name.c_str()) +
                       text);
}

std::uint64_t load_raw(const std::uint8_t* p, size_t width)
{
    std::uint64_t raw = 0;
    std::memcpy(&raw, p, width);
    return raw;
}

size_t width_of(ValueType type)
{
    switch (type) {
    case ValueType::U8:
    case ValueType::I8: return 1;
    case ValueType::U16:
    case ValueType::I16: return 2;
    case ValueType::F64: return 8;
    default: return 4;
    }
}

double as_number(ValueType type, std::uint64_t raw)
{
    switch (type) {
    case ValueType::U8: return static_cast<double>(static_cast<std::uint8_t>(raw));
    case ValueType::I8: return static_cast<double>(static_cast<std::int8_t>(raw));
    case ValueType::U16: return static_cast<double>(static_cast<std::uint16_t>(raw));
    case ValueType::I16: return static_cast<double>(static_cast<std::int16_t>(raw));
    case ValueType::U32: return static_cast<double>(static_cast<std::uint32_t>(raw));
    case ValueType::I32: return static_cast<double>(static_cast<std::int32_t>(raw));
    case ValueType::F32: {
        float f = 0;
        const auto low = static_cast<std::uint32_t>(raw);
        std::memcpy(&f, &low, 4);
        return f;
    }
    default: {
        double d = 0;
        std::memcpy(&d, &raw, 8);
        return d;
    }
    }
}

ValueType value_type(const std::string& name)
{
    const std::string type = lower(name);
    if (type == "u8") return ValueType::U8;
    if (type == "i8") return ValueType::I8;
    if (type == "u16") return ValueType::U16;
    if (type == "i16") return ValueType::I16;
    if (type == "u32") return ValueType::U32;
    if (type == "i32") return ValueType::I32;
    if (type == "f32" || type == "float") return ValueType::F32;
    if (type == "f64" || type == "double") return ValueType::F64;
    fail("type should be u8, i8, u16, i16, u32, i32, f32 or f64");
}

json::Value value_scan(const json::Value& args)
{
    const std::string action = lower(arg_string(args, "action", "new"));
    const std::string compare = lower(arg_string(args, "compare", has(args, "value") ? "eq" : "unknown"));
    const double value = arg_number(args, "value", 0), value2 = arg_number(args, "value2", 0);
    const size_t limit = static_cast<size_t>(arg_int(args, "limit", 30, 0, 1000));
    const std::lock_guard lock(g_scan_lock);
    Scan& scan = g_scan;
    if (action == "clear") {
        scan = Scan{};
        return text_result("Scan cleared");
    }
    const auto check = [&](double now, double before, bool has_before) {
        const bool floating = scan.type == ValueType::F32 || scan.type == ValueType::F64;
        const double tolerance = floating ? arg_number(args, "tolerance", 0.001) : 0;
        if (compare == "eq") return std::fabs(now - value) <= tolerance;
        if (compare == "ne") return std::fabs(now - value) > tolerance;
        if (compare == "gt") return now > value;
        if (compare == "lt") return now < value;
        if (compare == "between") return now >= std::min(value, value2) && now <= std::max(value, value2);
        if (compare == "unknown") return true;
        if (!has_before)
            fail(compare + " compares with the last scan: start with action new");
        if (compare == "changed") return now != before;
        if (compare == "unchanged") return now == before;
        if (compare == "increased") return now > before;
        if (compare == "decreased") return now < before;
        if (compare == "increased_by") return std::fabs(now - before - value) <= std::max(tolerance, 1e-9);
        if (compare == "decreased_by") return std::fabs(before - now - value) <= std::max(tolerance, 1e-9);
        fail("compare should be eq, ne, gt, lt, between, unknown, changed, unchanged, increased, decreased, increased_by or decreased_by");
    };
    if (action == "new") {
        scan = Scan{};
        scan.type = value_type(arg_string(args, "type", "u32"));
        const size_t width = width_of(scan.type);
        const bool aligned = arg_bool(args, "aligned", true);
        const std::vector<Region> where_ = scope_regions(arg_string(args, "scope", "writable"), scan.scope);
        std::uint64_t total = 0;
        for (const Region& region : where_)
            total += region.size;
        if (compare == "unknown" && total > 64ull * 1024 * 1024)
            fail(strf("an unknown starting value keeps every address: %llu MB is too much; give a module as scope, or a value",
                        static_cast<unsigned long long>(total >> 20)));
        constexpr size_t kMax = 4'000'000;
        std::vector<std::uint8_t> chunk;
        for (const Region& region : where_) {
            constexpr size_t kStep = 1 << 20;
            for (std::uint64_t offset = 0; offset < region.size; offset += kStep) {
                const size_t want = static_cast<size_t>(std::min<std::uint64_t>(kStep, region.size - offset));
                chunk.resize(want);
                const std::uint32_t start = region.base + static_cast<std::uint32_t>(offset);
                if (!read(start, chunk.data(), want))
                    continue;
                const size_t step = aligned ? width : 1;
                for (size_t i = (aligned ? (width - start % width) % width : 0); i + width <= want; i += step) {
                    const std::uint64_t raw = load_raw(chunk.data() + i, width);
                    if (!check(as_number(scan.type, raw), 0, false))
                        continue;
                    if (scan.addresses.size() >= kMax)
                        fail(strf("more than %zu matches: narrow it (a value, a module as scope)", kMax));
                    scan.addresses.push_back(start + static_cast<std::uint32_t>(i));
                    scan.values.push_back(raw);
                }
            }
        }
        scan.active = true;
        scan.scans = 1;
    } else if (action == "next") {
        if (!scan.active)
            fail("no scan yet: start with action new");
        const size_t width = width_of(scan.type);
        size_t kept = 0;
        for (size_t i = 0; i < scan.addresses.size(); ++i) {
            std::uint8_t now[8] = {};
            if (!read(scan.addresses[i], now, width))
                continue;
            const std::uint64_t raw = load_raw(now, width);
            if (!check(as_number(scan.type, raw), as_number(scan.type, scan.values[i]), true))
                continue;
            scan.addresses[kept] = scan.addresses[i];
            scan.values[kept] = raw;
            ++kept;
        }
        scan.addresses.resize(kept);
        scan.values.resize(kept);
        ++scan.scans;
    } else if (action != "show") {
        fail("action should be new, next, show or clear");
    }
    if (!scan.active)
        return text_result("No scan yet");
    std::string text = strf("%zu address%s after %u scan%s of %s:\n", scan.addresses.size(), scan.addresses.size() == 1 ? "" : "es",
                              scan.scans, scan.scans == 1 ? "" : "s", scan.scope.c_str());
    const size_t width = width_of(scan.type);
    for (size_t i = 0; i < scan.addresses.size() && i < limit; ++i) {
        std::uint8_t now[8] = {};
        const bool ok = read(scan.addresses[i], now, width);
        text += describe(scan.addresses[i]) + " = " + (ok ? strf("%g", as_number(scan.type, load_raw(now, width))) : std::string("??")) + "\n";
    }
    if (scan.addresses.size() > limit)
        text += strf("... %zu more\n", scan.addresses.size() - limit);
    return text_result(text);
}

// The instruction that ends at `after` (a data breakpoint stops just past the instruction that touched it).
std::string instruction_before(std::uint32_t after)
{
    std::uint8_t buffer[16];
    if (!read(after - 15, buffer, 15))
        return {};
    std::string best;
    unsigned best_length = 0;
    for (unsigned back = 1; back <= 15; ++back) {
        disasm::Instruction instruction;
        if (!disasm::decode(buffer + 15 - back, back, after - back, instruction) || instruction.length != back)
            continue;
        const bool memory = instruction.text.find('[') != std::string::npos;
        if (memory && back > best_length) {
            best = instruction.text + "  at " + where(after - back);
            best_length = back;
        } else if (best.empty() && !memory) {
            best = instruction.text + "  at " + where(after - back);
        }
    }
    return best;
}

json::Value watch(const json::Value& args)
{
    const std::string action = lower(arg_string(args, "action", "hits"));
    const std::lock_guard setup(g_watch_setup);
    static const char* const kKinds[] = {"execute", "write", "?", "access"};
    if (action == "set") {
        const std::uint32_t address = arg_address(args, "address");
        const std::string kind_name = lower(arg_string(args, "kind", "write"));
        unsigned kind = 0;
        if (kind_name == "execute" || kind_name == "exec" || kind_name == "x")
            kind = 0;
        else if (kind_name == "write" || kind_name == "w")
            kind = 1;
        else if (kind_name == "access" || kind_name == "read" || kind_name == "rw")
            kind = 3;
        else
            fail("kind should be execute, write or access (read or write)");
        unsigned length = kind == 0 ? 1 : static_cast<unsigned>(arg_int(args, "size", 4, 1, 4));
        if (length == 3)
            length = 4;
        if (address % length != 0)
            fail(strf("a %u-byte watch needs an address that's a multiple of %u (the CPU's rule)", length, length));
        int slot = has(args, "slot") ? static_cast<int>(arg_int(args, "slot", 0, 0, 3)) : -1;
        if (slot < 0)
            for (int i = 0; i < 4 && slot < 0; ++i)
                if (!g_watches[i].active)
                    slot = i;
        if (slot < 0)
            fail("all 4 hardware breakpoints are in use: clear one (action clear, slot n)");
        {
            const SpinLock lock;
            Watch& w = g_watches[slot];
            w = Watch{};
            w.address = address;
            w.kind = kind;
            w.length = length;
            w.set_ms = GetTickCount64();
            w.active = true;
        }
        if (g_handler == nullptr)
            g_handler = AddVectoredExceptionHandler(1, &on_exception);
        const unsigned threads = apply_watches();
        log::write(L"MCP: watch %d set: %hs %u bytes at 0x%08X (%u threads)", slot, kKinds[kind], length, address, threads);
        return text_result(strf("Watch %d: %s of %u byte%s at %s, in %u threads. Read what hit it with action hits.", slot, kKinds[kind],
                                  length, length == 1 ? "" : "s", describe(address).c_str(), threads));
    }
    if (action == "clear") {
        const bool all = !has(args, "slot");
        const int slot = all ? -1 : static_cast<int>(arg_int(args, "slot", 0, 0, 3));
        {
            const SpinLock lock;
            for (int i = 0; i < 4; ++i)
                if (all || i == slot)
                    g_watches[i].active = false;
        }
        const unsigned threads = apply_watches();
        return text_result(all ? strf("All watches cleared (%u threads)", threads) : strf("Watch %d cleared (%u threads)", slot, threads));
    }
    if (action == "list" || action == "hits") {
        // a copy, so nothing slow happens under the spin lock
        std::unique_ptr<Watch[]> copy(new Watch[4]);
        {
            const SpinLock lock;
            for (int i = 0; i < 4; ++i)
                copy[i] = g_watches[i];
        }
        const int only = has(args, "slot") ? static_cast<int>(arg_int(args, "slot", 0, 0, 3)) : -1;
        const ULONGLONG now = GetTickCount64();
        std::string text;
        for (int i = 0; i < 4; ++i) {
            const Watch& w = copy[i];
            if (!w.active || (only >= 0 && i != only))
                continue;
            text += strf("Watch %d: %s of %u byte%s at %s, set %.1fs ago: %llu hit%s\n", i, kKinds[w.kind], w.length, w.length == 1 ? "" : "s",
                           describe(w.address).c_str(), (now - w.set_ms) / 1000.0, static_cast<unsigned long long>(w.total),
                           w.total == 1 ? "" : "s");
            if (action == "list")
                continue;
            std::vector<const Hit*> hits;
            for (unsigned h = 0; h < w.hit_count; ++h)
                hits.push_back(&w.hits[h]);
            std::sort(hits.begin(), hits.end(), [](const Hit* a, const Hit* b) { return a->count > b->count; });
            for (const Hit* hit : hits) {
                if (w.kind == 0) {
                    text += strf("  %llux called from %s (returns to)\n", static_cast<unsigned long long>(hit->count),
                                   describe(hit->key).c_str());
                    text += strf("    args [esp+4..]: %s %s %s %s %s %s  this (ecx) %s\n", hex8(hit->stack[1]).c_str(), hex8(hit->stack[2]).c_str(),
                                   hex8(hit->stack[3]).c_str(), hex8(hit->stack[4]).c_str(), hex8(hit->stack[5]).c_str(),
                                   hex8(hit->stack[6]).c_str(), hex8(hit->regs[1]).c_str());
                } else {
                    const std::string before = instruction_before(hit->eip);
                    text += strf("  %llux by %s\n", static_cast<unsigned long long>(hit->count),
                                   before.empty() ? ("the instruction before " + describe(hit->eip)).c_str() : before.c_str());
                    std::uint8_t value[4] = {};
                    if (read(w.address, value, w.length))
                        text += "    the value now: " + hex_bytes(value, w.length) + "\n";
                }
                text += strf("    last: eax %s ecx %s edx %s ebx %s esp %s ebp %s esi %s edi %s, thread %lu, %.1fs ago\n",
                               hex8(hit->regs[0]).c_str(), hex8(hit->regs[1]).c_str(), hex8(hit->regs[2]).c_str(), hex8(hit->regs[3]).c_str(),
                               hex8(hit->regs[4]).c_str(), hex8(hit->regs[5]).c_str(), hex8(hit->regs[6]).c_str(), hex8(hit->regs[7]).c_str(),
                               hit->thread, (now - hit->last_ms) / 1000.0);
                if (hit->stack_ok) {
                    std::string calls;
                    for (int s = w.kind == 0 ? 1 : 0; s < 16; ++s)
                        if (hit->stack[s] >= 0x10000 && in_code(hit->stack[s]))
                            calls += (calls.empty() ? "" : ", ") + strf("[esp+%s] ", hex(s * 4).c_str()) + where(hit->stack[s]);
                    if (!calls.empty())
                        text += "    code addresses on the stack (callers, most likely): " + calls + "\n";
                }
            }
            if (w.dropped != 0)
                text += strf("  (%llu more hits at other places, not kept)\n", static_cast<unsigned long long>(w.dropped));
        }
        if (text.empty())
            text = "No watches set";
        return text_result(text);
    }
    fail("action should be set, hits, list or clear");
}

json::Value packets_tool(const json::Value& args)
{
    const std::string direction = lower(arg_string(args, "direction", "both"));
    const bool want_in = direction != "out", want_out = direction != "in";
    if (has(args, "last")) {
        const auto id = static_cast<std::uint16_t>(arg_int(args, "last", 0, 0, 0x1FF));
        std::string text;
        for (const bool incoming : {true, false}) {
            if ((incoming && !want_in) || (!incoming && !want_out))
                continue;
            double at = 0;
            const std::string data = packets::last(incoming, id, &at);
            if (data.empty()) {
                text += strf("No %s packet %s yet\n", incoming ? "incoming" : "outgoing", hex(id, 3).c_str());
                continue;
            }
            text += strf("Last %s packet %s, %zu bytes, %.1fs ago:\n", incoming ? "incoming" : "outgoing", hex(id, 3).c_str(), data.size(),
                           static_cast<double>(GetTickCount64()) / 1000.0 - at);
            for (size_t row = 0; row < data.size(); row += 16)
                text += strf("  +%03zX  ", row) + hex_bytes(reinterpret_cast<const std::uint8_t*>(data.data()) + row, std::min<size_t>(16, data.size() - row)) + "\n";
        }
        return text_result(text);
    }
    std::vector<std::uint16_t> ids;
    if (const json::Value* list = args.find("ids"); list != nullptr && list->is_array())
        for (const json::Value& id : list->array)
            ids.push_back(static_cast<std::uint16_t>(static_cast<std::uint32_t>(address_of(id, "ids")) & 0x1FF));
    const std::uint64_t since = static_cast<std::uint64_t>(arg_number(args, "since", 0));
    const size_t limit = static_cast<size_t>(arg_int(args, "limit", 30, 1, 1000));
    const int shown_bytes = static_cast<int>(arg_int(args, "bytes", 64, -1, 4096));
    std::vector<PacketRecord> picked;
    std::uint64_t latest = 0;
    {
        const std::lock_guard lock(g_packets_lock);
        latest = g_packet_seq;
        for (auto it = g_packets.rbegin(); it != g_packets.rend() && picked.size() < limit; ++it) {
            if (it->seq <= since)
                break;
            if ((it->incoming && !want_in) || (!it->incoming && !want_out))
                continue;
            if (!ids.empty() && std::find(ids.begin(), ids.end(), it->id) == ids.end())
                continue;
            picked.push_back(*it);
        }
    }
    std::reverse(picked.begin(), picked.end());
    const ULONGLONG now = GetTickCount64();
    std::string text = strf("%zu packet%s (newest seq %llu; pass since=%llu for only newer ones):\n", picked.size(), picked.size() == 1 ? "" : "s",
                              static_cast<unsigned long long>(latest), static_cast<unsigned long long>(latest));
    for (const PacketRecord& p : picked) {
        text += strf("#%llu %6.2fs ago %s %s %3zu bytes", static_cast<unsigned long long>(p.seq), (now - p.ms) / 1000.0,
                       p.incoming ? "in " : "out", hex(p.id, 3).c_str(), p.data.size());
        if (shown_bytes != 0) {
            const size_t n = shown_bytes < 0 ? p.data.size() : std::min<size_t>(p.data.size(), static_cast<size_t>(shown_bytes));
            text += ": " + hex_bytes(reinterpret_cast<const std::uint8_t*>(p.data.data()), n) + (n < p.data.size() ? " ..." : "");
        }
        text += "\n";
    }
    return text_result(text);
}

json::Value inject_packet(const json::Value& args)
{
    const std::string direction = lower(arg_string(args, "direction", "out"));
    if (direction != "in" && direction != "out")
        fail("direction should be in (as if the server sent it) or out (to the server)");
    const auto id = static_cast<std::uint16_t>(arg_int(args, "id", 0, 0, 0x1FF));
    std::vector<std::uint8_t> body;
    std::vector<bool> mask;
    if (has(args, "body"))
        parse_pattern(arg_string(args, "body"), body, mask);
    std::vector<std::uint8_t> packet(4 + body.size(), 0);
    std::memcpy(packet.data() + 4, body.data(), body.size());
    if (packet.size() > 0x1FC)
        fail("too big for one packet");
    packets::inject(direction == "in", id, static_cast<std::uint32_t>(packet.size()), packet.data());
    return text_result(strf("Queued %s packet %s (%zu bytes with its header) for the next buffer", direction == "in" ? "an incoming" : "an outgoing",
                              hex(id, 3).c_str(), packet.size()));
}

std::string chat_since(std::uint64_t since, size_t limit, const std::string& contains, std::uint64_t& latest)
{
    std::vector<ChatRecord> picked;
    {
        const std::lock_guard lock(g_chat_lock);
        latest = g_chat_seq;
        for (auto it = g_chat.rbegin(); it != g_chat.rend() && picked.size() < limit; ++it) {
            if (it->seq <= since)
                break;
            if (!contains.empty() && lower(it->text).find(contains) == std::string::npos)
                continue;
            picked.push_back(*it);
        }
    }
    std::reverse(picked.begin(), picked.end());
    std::string text;
    for (const ChatRecord& line : picked)
        text += strf("#%llu [%02u:%02u:%02u] (%d) ", static_cast<unsigned long long>(line.seq), line.time.wHour, line.time.wMinute,
                       line.time.wSecond, line.mode) +
                line.text + "\n";
    return text;
}

json::Value chat_log(const json::Value& args)
{
    std::uint64_t latest = 0;
    const std::string lines = chat_since(static_cast<std::uint64_t>(arg_number(args, "since", 0)),
                                         static_cast<size_t>(arg_int(args, "limit", 50, 1, 2000)), lower(arg_string(args, "contains")), latest);
    return text_result(strf("Chat log (newest seq %llu; pass since=%llu for only newer lines; (n) is the chat mode):\n",
                              static_cast<unsigned long long>(latest), static_cast<unsigned long long>(latest)) +
                       (lines.empty() ? std::string("(nothing)\n") : lines));
}

json::Value run_command(const json::Value& args)
{
    const std::string command = arg_string(args, "command");
    if (command.empty())
        fail("'command' is needed: a line as it would be typed (/echo hi, /vana stats, /ma \"Cure\" <me>)");
    const DWORD wait = static_cast<DWORD>(arg_int(args, "wait_ms", 500, 0, 10000));
    std::uint64_t before = 0;
    {
        const std::lock_guard lock(g_chat_lock);
        before = g_chat_seq;
    }
    bool ready = false;
    need_frame(on_game_thread([&] {
        ready = chat::ready();
        if (ready)
            chat::run(1, sjis(command));
    }));
    if (!ready)
        fail("the game's command line isn't hooked yet (still starting?)");
    if (wait > 0)
        Sleep(wait);
    std::uint64_t latest = 0;
    const std::string lines = chat_since(before, 100, {}, latest);
    return text_result("Ran: " + command + "\n" +
                       (lines.empty() ? strf("(no chat lines within %lu ms)\n", wait) : "Chat lines since:\n" + lines));
}

json::Value lua(const json::Value& args)
{
    const std::string code = arg_string(args, "code");
    if (code.empty())
        fail("'code' is needed");
    const std::string addon = arg_string(args, "addon", "mcp");
    const int depth = static_cast<int>(arg_int(args, "depth", 2, 0, 8));
    std::string output, error;
    bool ok = false;
    need_frame(on_game_thread([&] { ok = addons::eval(addon, code, depth, output, error); }, 30000));
    if (!ok)
        fail((output.empty() ? "" : output + "\n") + error);
    return text_result(output.empty() ? "(nothing)" : output);
}

json::Value screenshot_tool(const json::Value& args)
{
    const bool ui = arg_bool(args, "ui", true); // false: the 3D scene before the game's interface, VanaCore's drawing out too
    const bool overlays = ui && arg_bool(args, "overlays", true);
    const UINT max_width = static_cast<UINT>(arg_int(args, "max_width", 1280, 160, 3840));
    const bool jpeg = lower(arg_string(args, "format", "jpeg")) != "png";
    struct Shot {
        std::mutex lock;
        bool done = false;
        bool ok = false;
        unsigned width = 0, height = 0, stride = 0;
        std::vector<std::uint8_t> pixels;
        std::string error;
        HANDLE event = nullptr;
    };
    auto shot = std::make_shared<Shot>();
    shot->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::weak_ptr<Shot> weak = shot;
    screenshot::capture(overlays, [weak](bool ok, unsigned width, unsigned height, unsigned stride, std::vector<unsigned char> pixels,
                                         std::string error) {
        const std::shared_ptr<Shot> s = weak.lock();
        if (!s)
            return;
        const std::lock_guard lock(s->lock);
        s->ok = ok;
        s->width = width;
        s->height = height;
        s->stride = stride;
        s->pixels = std::move(pixels);
        s->error = std::move(error);
        s->done = true;
        SetEvent(s->event);
    }, ui);
    const bool arrived = WaitForSingleObject(shot->event, 5000) == WAIT_OBJECT_0;
    CloseHandle(shot->event);
    const std::lock_guard lock(shot->lock);
    if (!arrived || !shot->done)
        fail("the game didn't draw a frame within 5 seconds; try again");
    if (!shot->ok)
        fail("the screenshot failed: " + shot->error);
    UINT out_width = shot->width, out_height = shot->height;
    if (out_width > max_width) {
        out_height = std::max<UINT>(1, static_cast<UINT>(static_cast<std::uint64_t>(out_height) * max_width / out_width));
        out_width = max_width;
    }
    std::vector<std::uint8_t> encoded;
    std::string error;
    if (!encode_image(shot->pixels, shot->width, shot->height, shot->stride, out_width, out_height, jpeg, encoded, error))
        fail(error);
    json::Value result;
    json::Value& image = result["content"].push(json::Value::make_object());
    image["type"] = "image";
    image["data"] = base64(encoded);
    image["mimeType"] = jpeg ? "image/jpeg" : "image/png";
    json::Value& caption = result["content"].push(json::Value::make_object());
    caption["type"] = "text";
    caption["text"] = strf("%s: %ux%u, shown at %ux%u%s", ui ? "The game's screen" : "The 3D scene before the game's interface",
                             shot->width, shot->height, out_width, out_height,
                             !ui ? "" : overlays ? ", with VanaCore's drawing" : ", without VanaCore's drawing");
    result["isError"] = false;
    return result;
}

// ---- hello -------------------------------------------------------------------------------------

// Names this chat to the player (mcp.cpp keeps the sessions).
json::Value hello(const json::Value& args)
{
    const std::string answer = name_session(args.get_string("name"));
    if (answer.empty())
        fail("name: a few words for this chat's topic");
    return text_result(answer);
}

// ---- the list ----------------------------------------------------------------------------------

struct Param {
    const char* name;
    const char* type; // "string", "integer", "number", "boolean", "address" (a string or a number), "array"
    const char* description;
    bool required = false;
    const char* choices = nullptr; // "a|b|c"
};

struct Tool {
    const char* name;
    const char* title;
    const char* description;
    std::vector<Param> params;
    json::Value (*run)(const json::Value&);
    bool read_only;
};

const std::vector<Tool>& all_tools()
{
    static const std::vector<Tool> kTools = {
        {"hello", "Hello",
         "Names this chat to the player. Call it once, before any other tool here, with a few words for the chat's topic "
         "(\"alliance list RE\"): the player's chat log and /vana window then show which chat is probing, as #n \"name\".",
         {{"name", "string", "2-6 words: what this chat is about", true}},
         &hello, true},
        {"game_state", "Game state",
         "The logged-in character (name, jobs, HP/MP/TP, zone, position, stats, buffs), the target, the party, and where the game keeps "
         "its entity, party, player and target structures in memory.",
         {}, &game_state, true},
        {"entities", "Entities",
         "Players, NPCs and monsters the client knows about, nearest first: target index, server id, name, kind, HP%, distance in "
         "yalms, position, status and the entity structure's address. Give index for one entity in full.",
         {{"name", "string", "only names containing this (any case)"},
          {"kind", "string", "only this kind", false, "any|pc|npc|monster|self"},
          {"max_distance", "number", "only within this many yalms"},
          {"index", "address", "one entity, by target index, in full"},
          {"limit", "integer", "most rows (default 40)"}},
         &entities, true},
        {"read_memory", "Read memory",
         "Reads the game's memory: a hex dump, values of a type (a row each), pointers (each with what it points at: a function, a "
         "module offset, a string), or a string.",
         {{"address", "address", "where: e.g. \"FFXiMain.dll+0x4A1234\", \"[[party]+0x10]\", \"0x1A2B3C4D\"", true},
          {"size", "integer", "bytes (default 64; 256 for strings; at most 16384)"},
          {"type", "string", "how to show it (default hex)", false, "hex|u8|i8|u16|i16|u32|i32|f32|f64|ptr|string|utf16"}},
         &read_memory, true},
        {"write_memory", "Write memory",
         "Writes into the game's memory, code included (protection lifted while writing). Give hex bytes, text, or a value with a "
         "type. Says what was there before.",
         {{"address", "address", "where", true},
          {"hex", "string", "bytes, e.g. \"90 90 EB 05\""},
          {"text", "string", "a string (Shift-JIS by default, as the game's text is)"},
          {"encoding", "string", "for text", false, "sjis|utf8|utf16"},
          {"terminate", "boolean", "add a zero terminator after text"},
          {"value", "number", "a number, with type"},
          {"type", "string", "the value's type (default u32)", false, "u8|i8|u16|i16|u32|i32|f32|f64"}},
         &write_memory, false},
        {"disassemble", "Disassemble",
         "Disassembles x86 code at an address (Intel syntax), each line with its module offset and file VA (as Ghidra/IDA show it), and "
         "what calls, jumps and memory operands refer to: function names in system DLLs, the value at a global, strings.",
         {{"address", "address", "where to start", true},
          {"count", "integer", "instructions (default 40)"},
          {"stop_at_ret", "boolean", "stop after the first ret"}},
         &disassemble, true},
        {"modules", "Modules",
         "The DLLs loaded in the game (base, size, the base their file asks for); with name, one module's sections.",
         {{"name", "string", "one module, e.g. FFXiMain.dll"}}, &list_modules, true},
        {"find_pattern", "Find a byte pattern",
         "Finds a byte pattern (?? for any byte) in a module's code, as signature scans do: where each match is.",
         {{"pattern", "string", "e.g. \"8B 0D ?? ?? ?? ?? 85 C9 74\"", true},
          {"module", "string", "the module (default FFXiMain.dll), or all for all readable memory"},
          {"limit", "integer", "most matches (default 20)"}},
         &find_pattern, true},
        {"find_references", "Find references",
         "Finds what refers to an address in a module: calls and jumps to it, instructions that use it (push offset, mov, [global]), "
         "and pointers to it in data (vtables, tables). The usual next step after finding a string or a global.",
         {{"target", "address", "the address referred to", true},
          {"module", "string", "where to look (default FFXiMain.dll)"},
          {"limit", "integer", "most results (default 100)"}},
         &find_references, true},
        {"search_memory", "Search memory",
         "Finds a string, bytes or a value in memory. Strings are searched as the game stores them (Shift-JIS) unless encoding says "
         "otherwise. The search's own copies of the text (in this request) can show up among the matches.",
         {{"text", "string", "a string to find"},
          {"encoding", "string", "for text", false, "sjis|utf8|utf16"},
          {"hex", "string", "bytes to find (?? for any byte)"},
          {"value", "number", "a number to find, with type"},
          {"type", "string", "the value's type (default u32)", false, "u8|i8|u16|i16|u32|i32|f32|f64"},
          {"aligned", "boolean", "only at addresses that are multiples of the value's size"},
          {"scope", "string", "all (default), writable, or a module's name"},
          {"limit", "integer", "most matches (default 50)"}},
         &search_memory, true},
        {"value_scan", "Value scan",
         "Finds where the game keeps a value that changes, as Cheat Engine does: action new scans memory for a value (or every value), "
         "then after it changes in game, action next keeps the addresses that changed the same way. Show lists them.",
         {{"action", "string", "what to do (default new)", false, "new|next|show|clear"},
          {"type", "string", "the value's type, for new (default u32)", false, "u8|i8|u16|i16|u32|i32|f32|f64"},
          {"compare", "string", "how to compare (default eq with a value; unknown without one)", false,
           "eq|ne|gt|lt|between|unknown|changed|unchanged|increased|decreased|increased_by|decreased_by"},
          {"value", "number", "the value (eq, ne, gt, lt, between, increased_by, decreased_by)"},
          {"value2", "number", "between's other end"},
          {"tolerance", "number", "how close floats must be for eq (default 0.001)"},
          {"scope", "string", "for new: writable (default), all, or a module's name"},
          {"aligned", "boolean", "for new: only at multiples of the type's size (default true)"},
          {"limit", "integer", "most addresses shown (default 30)"}},
         &value_scan, true},
        {"watch", "Watch (hardware breakpoint)",
         "Hardware breakpoints (4 at a time): see which code writes, reads or runs an address, without stopping the game. set one, play, "
         "then hits lists each instruction that touched it (or for execute, each caller with its arguments), how often, the registers "
         "and the code addresses on the stack.",
         {{"action", "string", "what to do (default hits)", false, "set|hits|list|clear"},
          {"address", "address", "for set: what to watch"},
          {"kind", "string", "for set (default write)", false, "write|access|execute"},
          {"size", "integer", "for set: 1, 2 or 4 bytes (default 4; execute is 1)"},
          {"slot", "integer", "0-3: which breakpoint (clear without slot clears all)"}},
         &watch, false},
        {"packets", "Packets",
         "The packets the game received from the server and sent to it, as it got or sent them (after decryption), newest last: "
         "sequence, age, direction, id, size and bytes. Pass since to see only newer ones, or last with an id for that id's latest "
         "packet in full.",
         {{"direction", "string", "which way (default both)", false, "both|in|out"},
          {"ids", "array", "only these packet ids, e.g. [\"0x00D\", 40]"},
          {"since", "integer", "only packets after this sequence number"},
          {"limit", "integer", "most packets (default 30)"},
          {"bytes", "integer", "bytes of each shown in hex (default 64; -1 all; 0 none)"},
          {"last", "address", "the last packet with this id, in full"}},
         &packets_tool, true},
        {"inject_packet", "Inject a packet",
         "Adds a packet to the next buffer the game sends to the server (out) or receives from it (in). VanaCore writes its 4-byte "
         "header (id, size, sequence); give the bytes after it.",
         {{"direction", "string", "which way", true, "in|out"},
          {"id", "address", "the packet id, e.g. 0x0B5", true},
          {"body", "string", "the bytes after the header, in hex"}},
         &inject_packet, false},
        {"chat_log", "Chat log",
         "Lines that reached the chat log (the game's messages, chat, battle text, VanaCore's own), newest last, with their chat mode. "
         "Pass since for only newer lines.",
         {{"since", "integer", "only lines after this sequence number"},
          {"contains", "string", "only lines containing this (any case)"},
          {"limit", "integer", "most lines (default 50)"}},
         &chat_log, true},
        {"run_command", "Run a command",
         "Runs a line as if it were typed into the chat box: game commands (/target, /ma, /equip), VanaCore's (/vana, /addon) and "
         "addons' commands. Waits a moment and returns the chat lines that followed.",
         {{"command", "string", "the line, e.g. \"/echo hello\"", true},
          {"wait_ms", "integer", "how long to wait for chat lines after (default 500)"}},
         &run_command, false},
        {"lua", "Run Lua",
         "Runs LuaJIT inside the game, on its thread, in VanaCore's addon API: vana.memory (read_uint32, read_string, find...), vana.game "
         "(entity, party, player, target, container...), vana.packets, vana.chat, ffi (ffi.cast a function pointer to call the game's own "
         "functions: __thiscall, __stdcall, __cdecl). Code is tried as an expression first (\"vana.game.player()\"), then as statements "
         "(return what you want back). What it prints and returns comes back, tables shown to depth levels. Globals persist between "
         "calls in the mcp addon's state.",
         {{"code", "string", "Lua", true},
          {"addon", "string", "a loaded addon to run it in (default mcp, VanaCore's own state for this)"},
          {"depth", "integer", "how deep to show returned tables (default 2)"}},
         &lua, false},
        {"screenshot", "Screenshot", "The game's screen, as a picture. ui false: the 3D scene alone, as the frame shows it (effects, upscaler, "
         "colour grade) just before the game draws its interface, so no chat log, menu or name covers it (renderer d3d12 or d3d11).",
         {{"ui", "boolean", "with the game's interface, default true; false leaves VanaCore's drawing out too, and fails with no 3D scene"},
          {"overlays", "boolean", "with VanaCore's own drawing (addons' text, windows), default true"},
          {"max_width", "integer", "scaled to at most this wide (default 1280)"},
          {"format", "string", "default jpeg", false, "jpeg|png"}},
         &screenshot_tool, true},
    };
    return kTools;
}

std::vector<std::string> split(const char* text)
{
    std::vector<std::string> out;
    std::string part;
    for (const char* p = text; *p != '\0'; ++p) {
        if (*p == '|') {
            out.push_back(part);
            part.clear();
        } else {
            part += *p;
        }
    }
    out.push_back(part);
    return out;
}

} // namespace

json::Value list()
{
    json::Value list = json::Value::make_array();
    for (const Tool& tool : all_tools()) {
        json::Value& item = list.push(json::Value::make_object());
        item["name"] = tool.name;
        item["title"] = tool.title;
        item["description"] = tool.description;
        json::Value& schema = item["inputSchema"];
        schema["type"] = "object";
        json::Value& properties = schema["properties"];
        properties = json::Value::make_object();
        json::Value required = json::Value::make_array();
        for (const Param& param : tool.params) {
            json::Value& property = properties[param.name];
            const std::string type = param.type;
            if (type == "address") {
                json::Value& types = property["type"];
                types.push("string");
                types.push("integer");
            } else if (type == "array") {
                property["type"] = "array";
                json::Value& items = property["items"];
                items["type"].push("string");
                items["type"].push("integer");
            } else {
                property["type"] = param.type;
            }
            property["description"] = param.description;
            if (param.choices != nullptr)
                for (const std::string& choice : split(param.choices))
                    property["enum"].push(choice);
            if (param.required)
                required.push(param.name);
        }
        if (!required.array.empty())
            schema["required"] = std::move(required);
        item["annotations"]["readOnlyHint"] = tool.read_only;
        item["annotations"]["title"] = tool.title;
    }
    return list;
}

json::Value call(const std::string& name, const json::Value& arguments)
{
    for (const Tool& tool : all_tools()) {
        if (name != tool.name)
            continue;
        try {
            return tool.run(arguments);
        } catch (const Failure& failure) {
            json::Value result = text_result(failure.what());
            result["isError"] = true;
            return result;
        }
    }
    json::Value result = text_result("No tool " + name);
    result["isError"] = true;
    return result;
}

void on_frame()
{
    const ULONGLONG now = GetTickCount64();
    if (now - g_addresses_at < 2000)
        return;
    g_addresses_at = now;
    const game::Addresses found = game::addresses();
    g_entities = static_cast<std::uint32_t>(found.entities);
    g_entity_count = found.entity_count;
    g_party = static_cast<std::uint32_t>(found.party);
    g_player = static_cast<std::uint32_t>(found.player);
    g_target = static_cast<std::uint32_t>(found.target_pointer);
}

void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length)
{
    if (packet == nullptr || length < 4)
        return;
    PacketRecord record;
    record.ms = GetTickCount64();
    record.incoming = incoming;
    record.id = static_cast<std::uint16_t>((packet[0] | (packet[1] << 8)) & 0x1FF);
    record.data.assign(reinterpret_cast<const char*>(packet), length);
    const std::lock_guard lock(g_packets_lock);
    record.seq = ++g_packet_seq;
    g_packets.push_back(std::move(record));
    if (g_packets.size() > kPacketLog)
        g_packets.pop_front();
}

void on_chat(std::string_view line, int mode)
{
    // colour codes (1E xx, 1F xx) and the name marks (7F FC, 7F FB) out; auto-translate phrases (FD ... FD) kept for the
    // game's parser
    std::string text;
    for (size_t at = 0; at < line.size();) {
        const auto c = static_cast<unsigned char>(line[at]);
        if (c == 0xFD) {
            const size_t end = line.find('\xFD', at + 1);
            const size_t stop = end == std::string_view::npos ? line.size() : end + 1;
            text.append(line.substr(at, stop - at));
            at = stop;
        } else if (c == 0x1E || c == 0x1F || c == 0x7F) {
            at += 2;
        } else if ((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) {
            text.append(line.substr(at, 2));
            at += 2;
        } else {
            text += static_cast<char>(c);
            ++at;
        }
    }
    if (text.find('\xFD') != std::string::npos)
        text = chat::parse_auto_translate(text, false);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    ChatRecord record;
    GetLocalTime(&record.time);
    record.mode = mode;
    record.text = utf8(text);
    const std::lock_guard lock(g_chat_lock);
    record.seq = ++g_chat_seq;
    g_chat.push_back(std::move(record));
    if (g_chat.size() > kChatLog)
        g_chat.pop_front();
}

} // namespace vanacore::mcp::tools
