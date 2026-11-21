/* [client] hide.windows: any of the game's windows left out of the draw by its layout's name. hudwin.cpp includes this
 * inside its namespace, after its helpers (open_window), whose names it uses: reload reads the key (hidden_read), skip
 * asks hidden_named first for every window drawn. It is no file of its own. */
#pragma once

namespace {

// [client] hide.windows: layouts' names, the 8 letters after "menu    " (persona the Status window, money the gil window,
// level the EXP window, equip the Equipment window, profile, btlskill, mgcskill, trdskill, ...: the game's menu table, 0x2C-byte
// entries at VA 0x10370288 with the name at +0xC, 323 of them read from the dump 2026-11-06), spaces or commas between. A
// window of one is left out of the draw, as <id>.hide leaves the table's out (and left_out still runs its calc). name@other:
// only while a window of the layout other is up too (persona@equip, money@equip: the Status and gil windows the Equipment
// screen opens beside it, OpenEquip 0x10132D70, the Status screen's staying). Any window, whether the table, g_looks or
// another file has a switch for it or not: the HUD Manager's Status Window and Gil switches are made of it
struct Hidden {
    char name[16 + 1];
    char with[16 + 1]; // its other's, "" for always
    bool logged;
};
constexpr size_t kHiddenAtMost = 64;
Hidden g_hidden[kHiddenAtMost] = {};
size_t g_hidden_count = 0;

bool hide_separator(wchar_t c)
{
    return c == L' ' || c == L',' || c == L';' || c == L'\t';
}

// "menu    " and the word (its first 8 letters, lower case, the rest spaces) into `out`
void layout_named(const wchar_t* word, const wchar_t* end, char (&out)[16 + 1])
{
    std::memcpy(out, "menu            ", sizeof out);
    for (int i = 0; i < 8 && word + i < end; ++i) {
        const wchar_t c = word[i] >= L'A' && word[i] <= L'Z' ? word[i] + (L'a' - L'A') : word[i];
        out[8 + i] = static_cast<char>(c & 0x7F);
    }
}

void hidden_read(const wchar_t* ini)
{
    wchar_t text[1024] = {};
    GetPrivateProfileStringW(L"client", L"hide.windows", L"", text, 1024, ini);
    g_hidden_count = 0;
    for (const wchar_t* p = text; *p != 0 && g_hidden_count < kHiddenAtMost;) {
        while (*p != 0 && hide_separator(*p))
            ++p;
        const wchar_t* word = p;
        while (*p != 0 && !hide_separator(*p))
            ++p;
        if (p == word)
            break;
        const wchar_t* at = word;
        while (at < p && *at != L'@')
            ++at;
        if (at == word)
            continue; // ("@equip" alone: no window named)
        Hidden& h = g_hidden[g_hidden_count++];
        layout_named(word, at, h.name);
        h.with[0] = 0;
        if (at + 1 < p)
            layout_named(at + 1, p, h.with);
        h.logged = false;
    }
}

// The window of `layout` is one hide.windows leaves out now
bool hidden_named(const char* layout)
{
    for (size_t i = 0; i < g_hidden_count; ++i) {
        Hidden& h = g_hidden[i];
        if (std::memcmp(layout + kLayoutName, h.name, 16) != 0)
            continue;
        if (h.with[0] != 0 && open_window(h.with) == nullptr)
            continue;
        if (!h.logged) {
            h.logged = true;
            log::write(L"%.16hs: left out of the window draw ([client] hide.windows)", h.name);
        }
        return true;
    }
    return false;
}

} // namespace
