#include "clipboard.hpp"

#include <string>

#include "chat.hpp"
#include "compat/ashita/host.hpp"
#include "gametext.hpp"
#include "log.hpp"

namespace vanacore::clipboard {
namespace {

constexpr UINT kShiftJis = 932;     // FFXI's text encoding
constexpr size_t kLineLimit = 120;  // bytes the game's chat line takes

// Other programs (clipboard history, say) hold the clipboard for a moment after it changes: a few
// tries, a few milliseconds apart.
bool open_clipboard(HWND window)
{
    for (int attempt = 0; attempt < 10; ++attempt) {
        if (OpenClipboard(window))
            return true;
        Sleep(5);
    }
    return false;
}

// Half-width katakana (U+FF61-FF9F) as full-width: FFXI's font draws other symbols for Shift-JIS's single-byte kana.
// The voiced marks ﾞ and ﾟ join the kana before them where Shift-JIS has the result (ｶﾞ is ガ, ﾊﾟ is パ, ｳﾞ is ヴ).
std::wstring full_width_kana(const std::wstring& text)
{
    static constexpr wchar_t kFull[] = L"。「」、・ヲァィゥェォャュョッーアイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワン゛゜";
    static_assert(sizeof(kFull) / sizeof(wchar_t) == 0xFF9F - 0xFF61 + 2);
    std::wstring out;
    out.reserve(text.size());
    for (const wchar_t c : text) {
        if (c < 0xFF61 || c > 0xFF9F) {
            out += c;
            continue;
        }
        const wchar_t full = kFull[c - 0xFF61];
        const wchar_t last = out.empty() ? 0 : out.back();
        const bool ka_to = last >= L'カ' && last <= L'ト' && last != L'ッ' && (last < L'ッ' ? last % 2 == 1 : last % 2 == 0);
        const bool ha_ho = last >= L'ハ' && last <= L'ホ' && (last - L'ハ') % 3 == 0;
        if (c == 0xFF9E && (ka_to || ha_ho))
            out.back() = static_cast<wchar_t>(last + 1);
        else if (c == 0xFF9E && last == L'ウ')
            out.back() = L'ヴ';
        else if (c == 0xFF9F && ha_ho)
            out.back() = static_cast<wchar_t>(last + 2);
        else
            out += full;
    }
    return out;
}

std::wstring read_clipboard(HWND window)
{
    std::wstring text;
    if (!open_clipboard(window)) {
        log::write(L"Paste: the clipboard is in use by another program (error %lu)", GetLastError());
        return text;
    }
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = chars;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return full_width_kana(text);
}

bool write_clipboard(HWND window, const std::wstring& text)
{
    if (!open_clipboard(window))
        return false;
    EmptyClipboard();
    bool written = false;
    if (HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t))) {
        if (auto* chars = static_cast<wchar_t*>(GlobalLock(data))) {
            wcscpy_s(chars, text.size() + 1, text.c_str());
            GlobalUnlock(data);
            written = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
        }
        if (!written)
            GlobalFree(data);
    }
    CloseClipboard();
    return written;
}

// Code page 932 has no ñ, é or ¿: a Spanish line pasted lost them (ñ an n, ¿ a ?), and copied they came out "・". The game
// has them in a row of its own (gametext.hpp).
std::string to_shift_jis(const std::wstring& text)
{
    return gametext::from_wide(text);
}

std::wstring from_shift_jis(const std::string& text)
{
    return gametext::to_wide(text);
}

} // namespace

std::string get_text(HWND window)
{
    return to_shift_jis(read_clipboard(window));
}

bool set_text(HWND window, const std::string& text)
{
    return write_clipboard(window, from_shift_jis(text));
}

bool paste(HWND window)
{
    bool open = false;
    std::string current;
    if (!chat::input_line(open, current) || !open)
        return false;
    // The first line, tabs as spaces, no other control characters.
    std::wstring text;
    for (const wchar_t c : read_clipboard(window)) {
        if (c == L'\r' || c == L'\n')
            break;
        if (c == L'\t')
            text += L' ';
        else if (c >= 0x20)
            text += c;
    }
    const std::string bytes = to_shift_jis(text);
    // As much as the line has room for, never half a double-byte character.
    const size_t room = kLineLimit > current.size() ? kLineLimit - current.size() : 0;
    size_t fits = 0;
    while (fits < bytes.size()) {
        const auto byte = static_cast<BYTE>(bytes[fits]);
        const size_t size = IsDBCSLeadByteEx(kShiftJis, byte) && fits + 1 < bytes.size() ? 2 : 1;
        if (fits + size > room)
            break;
        fits += size;
    }
    // Into the line at its caret, as Ashita's /paste does. Posted as typed (a WM_CHAR a byte), Japanese came out
    // scrambled: PostMessageA takes each byte through Unicode in the keyboard's code page, and a lead or trail byte on
    // its own doesn't come back as itself.
    const bool inserted = fits > 0 && ashita::input_insert(bytes.substr(0, fits));
    log::write(L"Paste: %zu of %zu bytes into the chat line", inserted ? fits : 0, bytes.size());
    return true;
}

bool copy(HWND window)
{
    bool open = false;
    std::string line;
    if (!chat::input_line(open, line) || !open || line.empty())
        return false;
    // Without colour codes and auto-translate phrases (which only the game can spell out).
    std::string plain;
    for (size_t at = 0; at < line.size();) {
        const auto byte = static_cast<BYTE>(line[at]);
        if (byte == 0x1E || byte == 0x1F) {
            at += 2;
        } else if (byte == 0xFD) {
            const size_t end = line.find('\xFD', at + 1);
            at = end == std::string::npos ? line.size() : end + 1;
        } else {
            const size_t size = IsDBCSLeadByteEx(kShiftJis, byte) && at + 1 < line.size() ? 2 : 1;
            plain.append(line, at, size);
            at += size;
        }
    }
    const bool copied = write_clipboard(window, from_shift_jis(plain));
    log::write(L"Copy: the chat line (%zu bytes) %ls", plain.size(), copied ? L"copied" : L"couldn't be copied");
    return copied;
}

} // namespace vanacore::clipboard
