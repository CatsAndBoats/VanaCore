#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>

// The game's text: Shift-JIS (code page 932) with a row of FFXI's own for the European letters, Windows-1252's 0x80 to 0xFF:
// 85 40 to 85 7E and 85 80 for 0x80 to 0xBF (7F is no trail byte), 85 9F to 85 DE for 0xC0 to 0xFF. It's what polcore makes
// of a letter typed in the chat line (its converter, polcore+0x2E6F0, which FFXiMain's 0x101B4F70 calls through 0x1030F904:
// ñ, the keyboard's 0xF1, is 85 D0) and what the game's font draws them as (cells 639 to 767: "ñ Ñ á é í ó ú ü ¿ ¡" in the
// chat log, 2026-11-07). Code page 932 has none of them: Windows read 85 D0 as "・" and wrote "ñ" as an n and "¿" as a ?, so
// a Spanish line pasted, copied, logged or handed to an addon or plugin lost its letters (reported by a player from Mexico,
// 2026-11-07). Here they're converted around code page 932, which has the rest as before.
namespace vanacore::gametext {

namespace detail {

constexpr UINT kCodePage = 932;

// Windows-1252's letters for 0x80 to 0x9F (0 where it has none); 0xA0 to 0xFF are Unicode's own
constexpr wchar_t kHigh[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

// Windows-1252's byte `b` (0x80 to 0xFF) as the trail byte after 85
constexpr std::uint8_t trail_of(unsigned b)
{
    return static_cast<std::uint8_t>(b < 0xBF ? b - 0x80 + 0x40 : b == 0xBF ? 0x80 : b - 0xC0 + 0x9F);
}

// The Windows-1252 byte trail byte `t` after 85 stands for; 0 if it isn't the row's
constexpr unsigned byte_of(std::uint8_t t)
{
    return t >= 0x40 && t <= 0x7E ? t - 0x40u + 0x80u : t == 0x80 ? 0xBFu : t >= 0x9F && t <= 0xDE ? t - 0x9Fu + 0xC0u : 0u;
}

struct Table {
    wchar_t letter[128] = {}; // Windows-1252's 0x80 + i as Unicode (0: none)
    bool row[128] = {};       // and written in the row going out: code page 932 hasn't it (×, °, “ and the like it has)
};

inline Table build()
{
    Table table;
    for (unsigned i = 0; i < 128; ++i) {
        const wchar_t letter = i < 32 ? kHigh[i] : static_cast<wchar_t>(0x80 + i);
        table.letter[i] = letter;
        // A no-break space and a soft hyphen aren't the row's: from_wide makes them a space and nothing
        if (letter == 0 || letter == 0xA0 || letter == 0xAD)
            continue;
        char bytes[4] = {};
        BOOL fallback = FALSE;
        const int n = WideCharToMultiByte(kCodePage, WC_NO_BEST_FIT_CHARS, &letter, 1, bytes, sizeof bytes, nullptr, &fallback);
        table.row[i] = n <= 0 || fallback;
    }
    return table;
}

inline const Table& table()
{
    static const Table kTable = build();
    return kTable;
}

// The Windows-1252 byte the row writes `c` as, else 0
inline unsigned row_byte(wchar_t c)
{
    const Table& t = table();
    if (c >= 0xA0 && c <= 0xFF)
        return t.row[c - 0x80] ? c : 0u;
    for (unsigned i = 0; i < 32; ++i)
        if (t.letter[i] == c && t.row[i])
            return 0x80 + i;
    return 0;
}

} // namespace detail

// The game's text as UTF-16
inline std::wstring to_wide(std::string_view game)
{
    std::wstring out;
    out.reserve(game.size());
    std::size_t run = 0; // the first byte code page 932 hasn't converted yet
    const auto flush = [&](std::size_t end) {
        const int size = static_cast<int>(end - run);
        const int n = size > 0 ? MultiByteToWideChar(detail::kCodePage, 0, game.data() + run, size, nullptr, 0) : 0;
        if (n > 0) {
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(n));
            MultiByteToWideChar(detail::kCodePage, 0, game.data() + run, size, out.data() + at, n);
        }
    };
    for (std::size_t i = 0; i < game.size();) {
        const auto lead = static_cast<std::uint8_t>(game[i]);
        if (lead < 0x80 || i + 1 >= game.size() || !IsDBCSLeadByteEx(detail::kCodePage, lead)) {
            ++i;
            continue;
        }
        const unsigned byte = lead == 0x85 ? detail::byte_of(static_cast<std::uint8_t>(game[i + 1])) : 0u;
        const wchar_t letter = byte != 0 ? detail::table().letter[byte - 0x80] : L'\0';
        if (letter != 0) {
            flush(i);
            out += letter;
            run = i + 2;
        }
        i += 2;
    }
    flush(game.size());
    return out;
}

// UTF-16 as the game's text (what neither has: code page 932's ?). Text copied from a web page or Discord has no-break
// spaces and soft hyphens, which code page 932 made ?: a space, and nothing.
inline std::string from_wide(std::wstring_view wide)
{
    std::string out;
    out.reserve(wide.size());
    std::size_t run = 0;
    const auto flush = [&](std::size_t end) {
        const int size = static_cast<int>(end - run);
        const int n = size > 0 ? WideCharToMultiByte(detail::kCodePage, 0, wide.data() + run, size, nullptr, 0, nullptr, nullptr) : 0;
        if (n > 0) {
            const std::size_t at = out.size();
            out.resize(at + static_cast<std::size_t>(n));
            WideCharToMultiByte(detail::kCodePage, 0, wide.data() + run, size, out.data() + at, n, nullptr, nullptr);
        }
    };
    for (std::size_t i = 0; i < wide.size(); ++i) {
        const wchar_t c = wide[i];
        const unsigned byte = c < 0x80 || c == 0xA0 || c == 0xAD ? 0u : detail::row_byte(c);
        if (byte == 0 && c != 0xA0 && c != 0xAD)
            continue;
        flush(i);
        if (c == 0xA0) {
            out += ' ';
        } else if (byte != 0) {
            out += '\x85';
            out += static_cast<char>(detail::trail_of(byte));
        }
        run = i + 1;
    }
    flush(wide.size());
    return out;
}

// UTF-8 and UTF-16
inline std::wstring wide_of(std::string_view utf8)
{
    const int n = utf8.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

inline std::string utf8_of(std::wstring_view wide)
{
    const int n = wide.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// The game's text as UTF-8, and back
inline std::string to_utf8(std::string_view game)
{
    return utf8_of(to_wide(game));
}

inline std::string from_utf8(std::string_view utf8)
{
    return from_wide(wide_of(utf8));
}

} // namespace vanacore::gametext
