#include "equipdesc.hpp"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <string_view>

namespace vanacore::equipdesc {
namespace {

constexpr const char* kElements[8] = {"Fire", "Ice", "Wind", "Earth", "Lightning", "Water", "Light", "Dark"};

bool digit(char c) { return c >= '0' && c <= '9'; }
bool letter(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool word_char(char c) { return letter(c) || digit(c) || c == '_'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// A number at `at` (the generator's [+-] ?\d+%?): its length, 0 for none. Ends at `end`.
std::size_t number_at(std::string_view text, std::size_t at, std::size_t end)
{
    if (at >= end || (text[at] != '+' && text[at] != '-'))
        return 0;
    std::size_t i = at + 1;
    if (i + 1 < end && text[i] == ' ' && digit(text[i + 1]))
        ++i;
    if (i >= end || !digit(text[i]))
        return 0;
    while (i < end && digit(text[i]))
        ++i;
    if (i < end && text[i] == '%')
        ++i;
    return i - at;
}

// Where the last number in [from, end) ends, scanning as a regex's finditer does; `from` for none
std::size_t last_number_end(std::string_view text, std::size_t from, std::size_t end)
{
    std::size_t last = from;
    for (std::size_t i = from; i < end;) {
        if (const std::size_t n = number_at(text, i, end); n != 0) {
            last = i + n;
            i += n;
        } else {
            ++i;
        }
    }
    return last;
}

// "DEF:", "DMG:" or "Delay:" with its number at `colon` (the word before it, then [+-]?\d after spaces)
bool fixed_label(std::string_view text, std::size_t colon)
{
    std::size_t start = colon;
    while (start > 0 && (letter(text[start - 1]) || text[start - 1] == '.'))
        --start;
    const std::string_view word = text.substr(start, colon - start);
    if (word != "DEF" && word != "DMG" && word != "Delay")
        return false;
    std::size_t i = colon + 1;
    while (i < text.size() && text[i] == ' ')
        ++i;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
        ++i;
    return i < text.size() && digit(text[i]);
}

// The unconditional part: up to the first label with a colon that isn't DEF/DMG/Delay with its number, from the start of
// its line or just after the last number before it on that line
std::string_view cut(std::string_view text)
{
    for (std::size_t colon = text.find(':'); colon != std::string_view::npos; colon = text.find(':', colon + 1)) {
        if (fixed_label(text, colon))
            continue;
        const std::size_t newline = text.rfind('\n', colon == 0 ? 0 : colon - 1);
        const std::size_t start = newline == std::string_view::npos || newline >= colon ? 0 : newline + 1;
        return text.substr(0, last_number_end(text, start, colon));
    }
    return text;
}

// The length of DEF:n, DMG:n or Delay:n at `at` (the generator's \b(?:DEF|DMG|Delay):\s*[+-]?\d+), 0 for none
std::size_t fixed_at(std::string_view text, std::size_t at)
{
    if (at > 0 && word_char(text[at - 1]))
        return 0;
    std::size_t j = 0;
    for (const std::string_view label : {std::string_view("DEF:"), std::string_view("DMG:"), std::string_view("Delay:")})
        if (text.substr(at, label.size()) == label) {
            j = at + label.size();
            break;
        }
    if (j == 0)
        return 0;
    while (j < text.size() && space(text[j]))
        ++j;
    if (j < text.size() && (text[j] == '+' || text[j] == '-'))
        ++j;
    if (j >= text.size() || !digit(text[j]))
        return 0;
    while (j < text.size() && digit(text[j]))
        ++j;
    return j - at;
}

// Each DEF:n, DMG:n and Delay:n as a space
std::string without_fixed(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::size_t n = fixed_at(text, i); n != 0) {
            out += ' ';
            i += n;
        } else {
            out += text[i];
            ++i;
        }
    }
    return out;
}

// The words before a number: its line's last, quotes as spaces, lower case, single spaces, " ,:;" taken off both ends
std::string label_of(std::string_view raw)
{
    if (const std::size_t newline = raw.rfind('\n'); newline != std::string_view::npos)
        raw = raw.substr(newline + 1);
    std::string out;
    bool gap = false;
    for (char c : raw) {
        if (c == '"' || space(c)) {
            gap = true;
            continue;
        }
        if (gap)
            out += ' ';
        gap = false;
        out += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (gap)
        out += ' ';
    const std::size_t first = out.find_first_not_of(" ,:;");
    if (first == std::string::npos)
        return {};
    return out.substr(first, out.find_last_not_of(" ,:;") - first + 1);
}

int value_of(std::string_view token)
{
    long long value = 0;
    for (char c : token)
        if (digit(c))
            value = value < INT_MAX ? value * 10 + (c - '0') : value;
    if (value > INT_MAX)
        value = INT_MAX;
    return token[0] == '-' ? -static_cast<int>(value) : static_cast<int>(value);
}

} // namespace

std::string ascii(const char* raw)
{
    std::string out;
    if (raw == nullptr)
        return out;
    const auto* b = reinterpret_cast<const unsigned char*>(raw);
    std::size_t n = 0;
    while (b[n] != 0)
        ++n;
    for (std::size_t i = 0; i < n;) {
        const unsigned char c = b[i];
        if (c == 0xEF && i + 1 < n) { // an element icon: its name; another 0xEF pair: nothing
            if (b[i + 1] >= 0x1F && b[i + 1] <= 0x26)
                out += kElements[b[i + 1] - 0x1F];
            i += 2;
        } else if (c == 0xFD && i + 1 < n) { // an auto-translate bracket and the 4 bytes after it: left out
            i += 1 + std::min<std::size_t>(4, n - 1 - i);
        } else if (((c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC)) && i + 1 < n) {
            // a double-byte letter: not ASCII. One cp932 hasn't (85 54 before CatsEyeXI's "Quick Draw"): its first byte
            // alone, as Python's decoding replaces it, the next read again
            wchar_t wide[2];
            const bool letter_pair = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(b + i), 2, wide, 2) != 0;
            i += letter_pair ? 2 : 1;
        } else {
            if (c < 0x80)
                out += static_cast<char>(c);
            ++i;
        }
    }
    return out;
}

std::uint32_t fingerprint(const std::string& text)
{
    std::uint32_t hash = 0x811C9DC5u;
    for (const char c : text)
        if (letter(c) || digit(c) || c == '+' || c == '-' || c == '%')
            hash = (hash ^ static_cast<unsigned char>(c)) * 0x01000193u;
    return hash;
}

std::vector<Said> parse(const std::string& text)
{
    const std::string body = without_fixed(cut(text));
    const std::string_view view(body);
    std::vector<Said> out;
    std::size_t previous = 0;
    for (std::size_t i = 0; i < view.size();) {
        const std::size_t n = number_at(view, i, view.size());
        if (n == 0) {
            ++i;
            continue;
        }
        std::string label = label_of(view.substr(previous, i - previous));
        const std::string_view token = view.substr(i, n);
        previous = i + n;
        i += n;
        if (label.empty())
            continue;
        if (token.back() == '%')
            label += '%';
        out.push_back({std::move(label), value_of(token)});
    }
    return out;
}

} // namespace vanacore::equipdesc
