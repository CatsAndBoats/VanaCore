// How long the damage numbers stay up (damagetext.cpp, [graphics] damage.duration): the game's update of each number's
// maker pointed at VanaCore's life, the popup's own countdown stretched to match; and the preview's and the test hits'
// size (the game's last numbers') and life. damagetext.cpp includes this inside its anonymous namespace, after its
// globals, write_code and peek; it is no file of its own.
#pragma once

// How long a number lives. The object holding a hit's number and colour (the popup's maker, the popup hung from it) adds the
// frame's time (in 60ths of a second) to its age each frame and is gone at 150; the share of its life gone, which its point
// rises 0.5 by, is its age times 1/150. Its update (FFXiMain.dll+0x41E30 in CatsEyeXI's):
//   fst [esi+0ACh] (the age); je; mov ecx,[esi+0B4h]; test ecx,ecx; je; fcomp [150.0]; fnstsw ax; and eax,100h
// and after it, twice: fld [esi+0ACh]; fmul [1/150]. Other code reads the same 150.0, so the three operands are pointed at
// VanaCore's own two floats (damage.duration), the constants left as they are.
constexpr char kLifeCompare[] =
    "D9 96 AC 00 00 00 0F 84 ?? ?? ?? ?? 8B 8E B4 00 00 00 85 C9 0F 84 ?? ?? ?? ?? D8 1D ?? ?? ?? ?? DF E0 25 00 01 00 00";
constexpr std::uintptr_t kLifeCompareAt = 28; // the fcomp's operand
constexpr char kLifeShare[] = "D9 86 AC 00 00 00 D8 0D ?? ?? ?? ??";
constexpr std::uintptr_t kLifeShareAt = 8;     // the fmul's operand
constexpr std::size_t kLifeShareWithin = 0x100; // both, after the compare
constexpr float kGameLife = 150.0f;             // frames of 60 a second: kGameDuration
// The popup counts down a life of its own too (+0x110, from +0x114 when it began; +0x114 at 0: none, it lives as long as its
// maker) and is gone at 1. Stretched by as much at its first draw.
constexpr std::uintptr_t kPopupLeft = 0x110, kPopupLife = 0x114;
constexpr int kLifeLines = 4; // popups' own lives said in the log
constexpr ULONGLONG kStretchKept = 30000; // ms a stretched popup is remembered: past any life damage.duration gives

// The numbers' life: the game's update reads these two once its operands are pointed at them (damage.duration)
float g_life = kGameLife;               // frames of 60 a second
float g_life_share = 1.0f / kGameLife;  // and 1 over it
bool g_life_tried = false;
bool g_life_set = false;
int g_lives_said = 0;
struct Stretched {
    float life;   // the life it was given
    ULONGLONG at; // when
};
std::unordered_map<std::uintptr_t, Stretched> g_stretched; // popups whose own life is stretched
// The game's numbers' digit heights as a share of the screen's height, as each was first drawn: the preview's size
float g_sizes[kSizesKept] = {};
int g_sizes_count = 0, g_sizes_next = 0;

// A popup's own life (its countdown), stretched as its maker's is (damage.duration) when it's met: once, a popup met again
// (not drawn for a while) holding the life it was given
void stretch_life(std::uintptr_t popup, ULONGLONG now)
{
    if (!g_life_set)
        return;
    float* const left = reinterpret_cast<float*>(popup + kPopupLeft);
    float* const life = reinterpret_cast<float*>(popup + kPopupLife);
    const float times = g_life / kGameLife;
    if (const auto given = g_stretched.find(popup); given != g_stretched.end() && given->second.life == *life)
        return;
    const float was_left = *left, was_life = *life;
    const bool own = was_life > 0.0f && was_left > 0.0f;
    if (own && times != 1.0f) {
        *left = was_left * times;
        *life = was_life * times;
        g_stretched[popup] = {*life, now};
    }
    if (g_lives_said < kLifeLines && times != 1.0f) {
        ++g_lives_said;
        if (own)
            log::write(L"Graphics: a number's own life: %.1f of %.1f frames left, made %.1f of %.1f ([graphics] damage.duration)",
                       was_left, was_life, *left, *life);
        else
            log::write(L"Graphics: a number has no life of its own (%.1f of %.1f): it lives as long as its maker, %.0f frames "
                       L"([graphics] damage.duration)",
                       was_left, was_life, g_life);
    }
}

// A game's number's digit height as a share of the screen's height, at its first draw
void note_size(float share)
{
    if (!(share > 0.001f && share < 0.5f))
        return;
    g_sizes[g_sizes_next] = share;
    g_sizes_next = (g_sizes_next + 1) % kSizesKept;
    g_sizes_count = std::min(g_sizes_count + 1, kSizesKept);
}

// The screen's pixels a unit of the game's glyphs, for the preview and the test hits on a screen `height` high: as the
// game's last numbers were drawn (the middle of them), or kPreviewCell before any
float preview_cell(unsigned height)
{
    float share = kPreviewCell;
    if (g_sizes_count > 0) {
        float sorted[kSizesKept];
        std::copy_n(g_sizes, g_sizes_count, sorted);
        std::nth_element(sorted, sorted + g_sizes_count / 2, sorted + g_sizes_count);
        share = sorted[g_sizes_count / 2];
    }
    return static_cast<float>(height) * share / kDigitUnits;
}

// The game's update of a number's maker reads its life (150) and 1 over it from g_life and g_life_share: its three operands
// pointed at them, once they're seen to hold the game's two
bool install_life(HMODULE game)
{
    const std::uintptr_t site = scan::find(game, kLifeCompare);
    const std::uintptr_t first = site != 0 ? scan::find(site, kLifeShareWithin, kLifeShare, 0) : 0;
    const std::uintptr_t second = site != 0 ? scan::find(site, kLifeShareWithin, kLifeShare, 1) : 0;
    if (site == 0 || first == 0 || second == 0) {
        log::write(L"Graphics: couldn't find how long the game keeps the numbers over characters; they stay %.1f s "
                   L"([graphics] damage.duration)",
                   kGameDuration);
        return false;
    }
    const std::uintptr_t compare = site + kLifeCompareAt, share_one = first + kLifeShareAt, share_two = second + kLifeShareAt;
    std::uintptr_t life_at = 0, share_at = 0, share_at_two = 0;
    float life = 0.0f, share = 0.0f;
    if (!peek(compare, &life_at, sizeof life_at) || !peek(share_one, &share_at, sizeof share_at) ||
        !peek(share_two, &share_at_two, sizeof share_at_two) || share_at != share_at_two || !peek(life_at, &life, sizeof life) ||
        !peek(share_at, &share, sizeof share) || life != kGameLife || std::fabs(share * kGameLife - 1.0f) > 1e-4f) {
        log::write(L"Graphics: the numbers over characters don't live %.0f frames in this game (%g, 1 / %g); they stay as it "
                   L"keeps them ([graphics] damage.duration)",
                   kGameLife, life, share != 0.0f ? 1.0f / share : 0.0f);
        return false;
    }
    const std::uintptr_t ours_life = reinterpret_cast<std::uintptr_t>(&g_life);
    const std::uintptr_t ours_share = reinterpret_cast<std::uintptr_t>(&g_life_share);
    if (!write_code(compare, &ours_life, 4) || !write_code(share_one, &ours_share, 4) ||
        !write_code(share_two, &ours_share, 4)) {
        log::write(L"Graphics: couldn't change how long the game keeps the numbers over characters ([graphics] damage.duration)");
        return false;
    }
    log::write(L"Graphics: how long the numbers over characters stay is VanaCore's, at FFXiMain.dll+0x%X (and +0x%X, +0x%X) "
               L"([graphics] damage.duration)",
               static_cast<unsigned>(compare - reinterpret_cast<std::uintptr_t>(game)),
               static_cast<unsigned>(share_one - reinterpret_cast<std::uintptr_t>(game)),
               static_cast<unsigned>(share_two - reinterpret_cast<std::uintptr_t>(game)));
    return true;
}

// How long a test hit lives (ms): damage.duration
ULONGLONG test_life()
{
    return static_cast<ULONGLONG>(std::lround(g_settings.duration * 1000.0f));
}
