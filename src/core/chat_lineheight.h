// chat.cpp's, inside its namespace after the chat log file's globals: [client] chat.lineheight, the chat window's row
// spacing (the draw's four immediates, the collapsed window's height, the expanded window's rows) and apply_line_height.

// [client] chat.lineheight: the chat window's row spacing, 16 pixels in the game (FFXiMain's chat draw, 0x10160da0,
// steps each row by an immediate 0x10: `add edi,-10h`, `sub edi,10h`, `add eax,-10h`, `sub edx,10h`). The four
// immediates are patched in place, so the rows spread out (the window's own height stays: fewer rows show).
constexpr int kLineHeightDefault = 16;
constexpr int kLineHeightMax = 64;
struct LineSite {
    const char* pattern;
    bool negative; // the immediate is the negated height (an `add`)
    std::uintptr_t at = 0;
};
LineSite g_line_sites[4] = {
    {"83 C7 F0 8B 4C 24 10 8D 54 24 14 8D 44 24 34 52 8B 49 18 50 E8", true},
    {"83 EF 10 3B EB 7C ?? EB 08 8B 44 24 28 89 44 24 34 E8", false},
    {"83 C0 F0 89 44 24 2C 8B 44 24 10 8D 4C 24 30 8D 54 24 34 51 8B 48 18 52", true},
    {"83 EA 10 3B C1 89 44 24 14 89 54 24 2C 0F 8C ?? ?? ?? ?? E8", false},
};
bool g_line_found = false;
int g_line_height = kLineHeightDefault;

// The chat window's own sizing (0x10161490): its height is lines * 16 less 128 (8 rows' worth) over the base the window has
// (`sub edi,8 ... shl edi,4` is (lines - 8) * 16). With another row spacing L the part per line is L: `imul edi,edi,L` and
// `add edi,-128` take their places (the same bytes), so a full window is as tall as the lines in it.
std::uintptr_t g_height_site = 0;
void write_code(std::uintptr_t at, const void* bytes, size_t size);

// The expanded log's rows (the log window's +0x58, set at 0x10137278 as it opens out): (screen height - 448) / 16 + 23, in
// the game's 16 pixel rows. Spread to another spacing, fewer fit, and the game drew the rest above the window: scrolled up,
// it stopped with its first rows out of sight. And the screen's height isn't the window's (chat.move sizes it), so rows
// were short of it too: a gap of empty rows over the first line. `sar eax,4; add eax,17h` (6 bytes) becomes a call to this:
// the window's height (ecx, just stored at +0x5C) over the spacing.
std::uintptr_t g_expand_site = 0;
constexpr std::uint8_t kExpandGame[6] = {0xC1, 0xF8, 0x04, 0x83, 0xC0, 0x17};
__declspec(naked) void expanded_rows()
{
    __asm {
        mov eax, ecx            ; the window's height
        push edx                ; (edx and ecx are the game's after the call)
        push ecx
        mov ecx, g_line_height
        cdq
        idiv ecx
        pop ecx
        pop edx
        ret
    }
}

void apply_line_height(int height)
{
    if (!g_line_found || height == g_line_height)
        return;
    for (const LineSite& site : g_line_sites) {
        auto* immediate = reinterpret_cast<std::uint8_t*>(site.at + 2);
        DWORD old = 0;
        if (!VirtualProtect(immediate, 1, PAGE_EXECUTE_READWRITE, &old))
            continue;
        *immediate = static_cast<std::uint8_t>(site.negative ? -height : height);
        VirtualProtect(immediate, 1, old, &old);
        FlushInstructionCache(GetCurrentProcess(), immediate, 1);
    }
    g_line_height = height;
    if (g_height_site != 0) {
        const bool game = height == kLineHeightDefault;
        const std::uint8_t first[3] = {0x6B, 0xFF, static_cast<std::uint8_t>(height)}; // imul edi,edi,height
        const std::uint8_t first_game[3] = {0x83, 0xEF, 0x08};                         // sub edi,8
        const std::uint8_t second[3] = {0x83, 0xC7, 0x80};                             // add edi,-128
        const std::uint8_t second_game[3] = {0xC1, 0xE7, 0x04};                        // shl edi,4
        write_code(g_height_site, game ? first_game : first, 3);
        write_code(g_height_site + 7, game ? second_game : second, 3);
    }
    if (g_expand_site != 0) {
        std::uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        const std::int32_t rel = static_cast<std::int32_t>(reinterpret_cast<std::uintptr_t>(&expanded_rows) - (g_expand_site + 5));
        std::memcpy(call + 1, &rel, 4);
        write_code(g_expand_site, height == kLineHeightDefault ? kExpandGame : call, 6);
    }
    log::write(L"Chat: row spacing %d px ([client] chat.lineheight)", height);
}
