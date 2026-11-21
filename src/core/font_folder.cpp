#include "font_folder.hpp"

#include <windows.h>

#include <shellapi.h>

#include <dwrite_3.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.hpp"

namespace vanacore::fonts {
namespace {

// A loaded font's bytes (OpenType, unpacked), its family, and its icons: name -> character.
struct Font {
    std::vector<std::uint8_t> data;
    std::wstring family;
    std::wstring file; // its file's name in the fonts folder
    HANDLE handle = nullptr; // what AddFontMemResourceEx gave, to unload it with
    std::unordered_map<std::string, char32_t> icons;
    bool read = false; // icons looked for
};
std::vector<Font> g_fonts;

// ---- OpenType, read as far as names, characters and ligatures -------------------------------------

struct Reader {
    const std::vector<std::uint8_t>& d;
    bool ok(std::size_t at, std::size_t n) const { return at <= d.size() && n <= d.size() - at; }
    unsigned u16(std::size_t at) const { return ok(at, 2) ? (d[at] << 8 | d[at + 1]) : 0; }
    std::uint32_t u32(std::size_t at) const
    {
        return ok(at, 4) ? (std::uint32_t{d[at]} << 24 | std::uint32_t{d[at + 1]} << 16 | std::uint32_t{d[at + 2]} << 8 | d[at + 3]) : 0;
    }
};

// A table's offset, 0 if the font hasn't it.
std::size_t table(const Reader& r, const char (&tag)[5])
{
    const unsigned count = r.u16(4);
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t record = 12 + i * 16;
        if (r.ok(record, 16) && std::memcmp(&r.d[record], tag, 4) == 0)
            return r.u32(record + 8);
    }
    return 0;
}

// The family (name 1, Windows, English), as GDI knows it.
std::wstring family_name(const Reader& r)
{
    const std::size_t name = table(r, "name");
    if (name == 0)
        return {};
    const unsigned count = r.u16(name + 2), strings = r.u16(name + 4);
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t record = name + 6 + i * 12;
        if (r.u16(record) != 3 || r.u16(record + 6) != 1 || (r.u16(record + 4) & 0xFF) != 0x09)
            continue;
        const unsigned length = r.u16(record + 8), offset = r.u16(record + 10);
        const std::size_t at = name + strings + offset;
        std::wstring text;
        for (unsigned k = 0; k + 1 < length && r.ok(at + k, 2); k += 2)
            text += static_cast<wchar_t>(r.u16(at + k));
        return text;
    }
    return {};
}

// Character -> glyph from the Unicode cmap (format 12, else 4), as glyph -> characters too.
void characters(const Reader& r, std::unordered_map<unsigned, char32_t>& glyph_to_char,
                std::unordered_map<char32_t, unsigned>& char_to_glyph)
{
    const std::size_t cmap = table(r, "cmap");
    if (cmap == 0)
        return;
    std::size_t best = 0;
    unsigned best_format = 0;
    const unsigned count = r.u16(cmap + 2);
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t record = cmap + 4 + i * 8;
        const unsigned platform = r.u16(record), encoding = r.u16(record + 2);
        const std::size_t sub = cmap + r.u32(record + 4);
        const unsigned format = r.u16(sub);
        const bool unicode = platform == 0 || (platform == 3 && (encoding == 1 || encoding == 10));
        if (unicode && (format == 12 || (format == 4 && best_format != 12)))
            best = sub, best_format = format;
    }
    const auto add = [&](char32_t c, unsigned glyph) {
        if (glyph == 0)
            return;
        char_to_glyph.emplace(c, glyph);
        // the first character of a glyph wins, except a private-use one (an icon's) over a letter
        const bool icon = (c >= 0xE000 && c <= 0xF8FF) || c >= 0xF0000;
        auto [it, added] = glyph_to_char.emplace(glyph, c);
        if (!added && icon && !((it->second >= 0xE000 && it->second <= 0xF8FF) || it->second >= 0xF0000))
            it->second = c;
    };
    if (best_format == 12) {
        const std::uint32_t groups = r.u32(best + 12);
        for (std::uint32_t g = 0; g < groups && r.ok(best + 16 + g * 12, 12); ++g) {
            const std::size_t at = best + 16 + g * 12;
            const std::uint32_t first = r.u32(at), last = r.u32(at + 4), glyph = r.u32(at + 8);
            for (std::uint32_t c = first; c <= last && c - first < 0x10000; ++c)
                add(c, glyph + (c - first));
        }
    } else if (best_format == 4) {
        const unsigned segments = r.u16(best + 6) / 2;
        const std::size_t ends = best + 14, starts = ends + segments * 2 + 2, deltas = starts + segments * 2,
                          ranges = deltas + segments * 2;
        for (unsigned s = 0; s < segments; ++s) {
            const unsigned end = r.u16(ends + s * 2), start = r.u16(starts + s * 2), delta = r.u16(deltas + s * 2),
                           range = r.u16(ranges + s * 2);
            for (unsigned c = start; c <= end && c != 0xFFFF; ++c) {
                unsigned glyph = 0;
                if (range == 0) {
                    glyph = (c + delta) & 0xFFFF;
                } else {
                    glyph = r.u16(ranges + s * 2 + range + (c - start) * 2);
                    if (glyph != 0)
                        glyph = (glyph + delta) & 0xFFFF;
                }
                add(c, glyph);
            }
        }
    }
}

// Coverage: glyph -> its index, for each glyph it covers.
void coverage(const Reader& r, std::size_t at, std::vector<std::pair<unsigned, unsigned>>& out)
{
    const unsigned format = r.u16(at);
    if (format == 1) {
        const unsigned count = r.u16(at + 2);
        for (unsigned i = 0; i < count; ++i)
            out.emplace_back(r.u16(at + 4 + i * 2), i);
    } else if (format == 2) {
        const unsigned count = r.u16(at + 2);
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t range = at + 4 + i * 6;
            const unsigned first = r.u16(range), last = r.u16(range + 2), index = r.u16(range + 4);
            for (unsigned g = first; g <= last && g - first < 0x10000; ++g)
                out.emplace_back(g, index + (g - first));
        }
    }
}

// Every ligature (GSUB lookup type 4, or 7 wrapping it) whose parts spell a name and whose glyph is a
// character: an icon font's icons.
void ligatures(Font& font)
{
    const Reader r{font.data};
    std::unordered_map<unsigned, char32_t> glyph_to_char;
    std::unordered_map<char32_t, unsigned> char_to_glyph;
    characters(r, glyph_to_char, char_to_glyph);
    const std::size_t gsub = table(r, "GSUB");
    if (gsub == 0)
        return;
    const std::size_t lookups = gsub + r.u16(gsub + 8);
    const unsigned lookup_count = r.u16(lookups);
    const auto letter = [&](unsigned glyph) -> char {
        const auto it = glyph_to_char.find(glyph);
        if (it == glyph_to_char.end() || it->second <= 0x20 || it->second >= 0x7F)
            return 0;
        // icon names are lower case (a font may give A and a one glyph, and A is found first)
        return static_cast<char>(it->second >= 'A' && it->second <= 'Z' ? it->second - 'A' + 'a' : it->second);
    };
    for (unsigned l = 0; l < lookup_count; ++l) {
        const std::size_t lookup = lookups + r.u16(lookups + 2 + l * 2);
        const unsigned type = r.u16(lookup), subtables = r.u16(lookup + 4);
        for (unsigned s = 0; s < subtables; ++s) {
            std::size_t sub = lookup + r.u16(lookup + 6 + s * 2);
            unsigned sub_type = type;
            if (type == 7) { // extension: the real subtable further on
                sub_type = r.u16(sub + 2);
                sub = sub + r.u32(sub + 4);
            }
            if (sub_type != 4 || r.u16(sub) != 1)
                continue;
            std::vector<std::pair<unsigned, unsigned>> firsts;
            coverage(r, sub + r.u16(sub + 2), firsts);
            const unsigned sets = r.u16(sub + 4);
            for (const auto& [first, index] : firsts) {
                if (index >= sets)
                    continue;
                const char head = letter(first);
                if (head == 0)
                    continue;
                const std::size_t set = sub + r.u16(sub + 6 + index * 2);
                const unsigned count = r.u16(set);
                for (unsigned k = 0; k < count; ++k) {
                    const std::size_t lig = set + r.u16(set + 2 + k * 2);
                    const unsigned glyph = r.u16(lig), parts = r.u16(lig + 2);
                    const auto target = glyph_to_char.find(glyph);
                    if (target == glyph_to_char.end() || !((target->second >= 0xE000 && target->second <= 0xF8FF) || target->second >= 0xF0000))
                        continue;
                    std::string name(1, head);
                    bool spelled = true;
                    for (unsigned p = 1; p < parts && spelled; ++p) {
                        const char c = letter(r.u16(lig + 4 + (p - 1) * 2));
                        spelled = c != 0;
                        name += c;
                    }
                    if (spelled)
                        font.icons.emplace(std::move(name), target->second);
                }
            }
        }
    }
}

// ---- Loading --------------------------------------------------------------------------------------

// .woff / .woff2 into OpenType, with DirectWrite (Windows 10 1703 and later)
bool unpack(std::vector<std::uint8_t>& data, DWRITE_CONTAINER_TYPE type)
{
    const HMODULE dwrite = LoadLibraryW(L"dwrite.dll");
    const auto create = dwrite != nullptr ? reinterpret_cast<decltype(&DWriteCreateFactory)>(GetProcAddress(dwrite, "DWriteCreateFactory"))
                                          : nullptr;
    IDWriteFactory5* factory = nullptr;
    if (create == nullptr ||
        FAILED(create(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(&factory))))
        return false;
    IDWriteFontFileStream* stream = nullptr;
    bool ok = SUCCEEDED(factory->UnpackFontFile(type, data.data(), static_cast<UINT32>(data.size()), &stream));
    if (ok) {
        UINT64 size = 0;
        const void* fragment = nullptr;
        void* context = nullptr;
        ok = SUCCEEDED(stream->GetFileSize(&size)) && size < (1u << 30) &&
             SUCCEEDED(stream->ReadFileFragment(&fragment, 0, size, &context));
        if (ok) {
            const auto* bytes = static_cast<const std::uint8_t*>(fragment);
            data.assign(bytes, bytes + size);
            stream->ReleaseFileFragment(context);
        }
        stream->Release();
    }
    factory->Release();
    return ok;
}

bool ends_with(const std::wstring& text, const wchar_t* suffix)
{
    const std::size_t n = wcslen(suffix);
    return text.size() >= n && _wcsicmp(text.c_str() + text.size() - n, suffix) == 0;
}

bool font_file(const std::wstring& name)
{
    return ends_with(name, L".woff2") || ends_with(name, L".woff") || ends_with(name, L".ttf") || ends_with(name, L".otf") ||
           ends_with(name, L".ttc");
}

std::wstring g_folder; // <VanaCore>\fonts\

// Font file `path` (`name` its file name) loaded into the process, its family read: false, and why, if it isn't a font
bool load(const std::wstring& path, const std::wstring& name, std::wstring& why)
{
    std::ifstream file(path, std::ios::binary);
    Font font;
    font.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (font.data.empty()) {
        why = L"it couldn't be read";
        return false;
    }
    const bool woff2 = ends_with(name, L".woff2"), woff = ends_with(name, L".woff");
    if ((woff2 || woff) && !unpack(font.data, woff2 ? DWRITE_CONTAINER_TYPE_WOFF2 : DWRITE_CONTAINER_TYPE_WOFF)) {
        why = L"it couldn't be unpacked (DirectWrite on Windows 10 1703 or later unpacks .woff and .woff2)";
        return false;
    }
    DWORD count = 0;
    font.handle = AddFontMemResourceEx(font.data.data(), static_cast<DWORD>(font.data.size()), nullptr, &count);
    if (font.handle == nullptr || count == 0) {
        why = L"it isn't a font Windows could load";
        return false;
    }
    font.file = name;
    font.family = family_name(Reader{font.data});
    log::write(L"Fonts: %ls loaded (\"%ls\")", name.c_str(), font.family.c_str());
    g_fonts.push_back(std::move(font));
    return true;
}

// Its icons looked for, once (and its bytes let go: GDI has its own copy)
void read(Font& font)
{
    if (font.read)
        return;
    font.read = true;
    ligatures(font);
    font.data.clear();
    font.data.shrink_to_fit();
    if (!font.icons.empty())
        log::write(L"Fonts: %zu icons in \"%ls\"", font.icons.size(), font.family.c_str());
}

} // namespace

void install(const wchar_t* install_dir)
{
    if (install_dir == nullptr || install_dir[0] == L'\0')
        return;
    g_folder = std::wstring(install_dir) + L"\\fonts\\";
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((g_folder + L"*").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::wstring name = found.cFileName;
        std::wstring why;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && font_file(name) && !load(g_folder + name, name, why))
            log::write(L"Fonts: %ls: %ls", name.c_str(), why.c_str());
    } while (FindNextFileW(search, &found));
    FindClose(search);
}

bool add(const std::wstring& path, std::wstring& family, std::wstring& why)
{
    const std::size_t slash = path.find_last_of(L"\\/");
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    if (g_folder.empty() || !font_file(name)) {
        why = g_folder.empty() ? L"VanaCore's fonts folder isn't known" : L"it isn't a .ttf, .otf, .ttc, .woff or .woff2 file";
        return false;
    }
    const std::wstring kept = g_folder + name;
    CreateDirectoryW(g_folder.c_str(), nullptr);
    // (the same file picked again, or one of the folder's own: it's there already)
    if (_wcsicmp(kept.c_str(), path.c_str()) != 0 && !CopyFileW(path.c_str(), kept.c_str(), TRUE) &&
        GetLastError() != ERROR_FILE_EXISTS) {
        why = L"it couldn't be copied into " + g_folder;
        return false;
    }
    const std::size_t before = g_fonts.size();
    if (!load(kept, name, why))
        return false;
    family = g_fonts.back().family;
    for (std::size_t i = 0; i < before; ++i)
        if (g_fonts[i].family == family) { // loaded already: the one copy kept
            g_fonts.pop_back();
            break;
        }
    if (family.empty())
        why = L"it has no family name";
    return !family.empty();
}

std::vector<std::wstring> families()
{
    std::vector<std::wstring> out;
    for (Font& font : g_fonts) {
        read(font);
        if (font.icons.empty() && !font.family.empty())
            out.push_back(font.family);
    }
    return out;
}

bool remove(const std::wstring& family, std::wstring& why)
{
    for (auto font = g_fonts.begin(); font != g_fonts.end(); ++font) {
        read(*font);
        if (font->family != family || !font->icons.empty()) // (the icon fonts aren't the user's to delete)
            continue;
        const std::wstring path = g_folder + font->file;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            // to the Recycle Bin (SHFileOperation's path list ends in two nulls)
            std::wstring from = path;
            from.push_back(L'\0');
            SHFILEOPSTRUCTW operation{};
            operation.wFunc = FO_DELETE;
            operation.pFrom = from.c_str();
            operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            if (SHFileOperationW(&operation) != 0 || operation.fAnyOperationsAborted) {
                why = L"its file couldn't be moved to the Recycle Bin";
                return false;
            }
        }
        if (font->handle != nullptr)
            RemoveFontMemResourceEx(font->handle);
        log::write(L"Fonts: %ls removed (\"%ls\")", font->file.c_str(), font->family.c_str());
        g_fonts.erase(font);
        return true;
    }
    why = L"it isn't one of the fonts added";
    return false;
}

char32_t icon(std::string_view name, std::wstring* family)
{
    const std::string key(name);
    for (Font& font : g_fonts) {
        read(font);
        const auto it = font.icons.find(key);
        if (it != font.icons.end()) {
            if (family != nullptr)
                *family = font.family;
            return it->second;
        }
    }
    return 0;
}

} // namespace vanacore::fonts
