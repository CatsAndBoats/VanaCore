#include "nameplates_debug.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "d3d8.hpp"
#include "log.hpp"

namespace vanacore::nameplates::debug {
namespace {

// [graphics] nameplates.trace
struct Trace {
    float asked = 0.0f;  // the setting as last seen
    ULONGLONG until = 0; // tracing till then (GetTickCount64); 0: not tracing
    unsigned frame = 0;
    LARGE_INTEGER start{}, frequency{};
    std::string rows;
};
Trace g_trace;

// VanaCore's logs folder: beside this DLL's folder's VanaCore.exe, logs\.
std::wstring logs_folder()
{
    HMODULE module = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&logs_folder), &module) ||
        GetModuleFileNameW(module, path, MAX_PATH) == 0)
        return {};
    std::wstring folder = path;
    folder.resize(folder.find_last_of(L'\\') + 1);
    return folder + L"logs\\";
}

} // namespace

void note_letter_state(void* device, std::uint32_t colour, float depth, const char* text)
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    static unsigned said = 0;
    static DWORD last[14] = {};
    if (said >= 6)
        return;
    DWORD now[14] = {};
    call(device, slot::GetPixelShader, &now[0]);
    const DWORD stages[] = {d3d8::tss::ColorOp, d3d8::tss::ColorArg1, d3d8::tss::ColorArg2,
                            d3d8::tss::AlphaOp, d3d8::tss::AlphaArg1, d3d8::tss::AlphaArg2};
    for (unsigned i = 0; i < 6; ++i)
        call(device, slot::GetTextureStageState, DWORD{0}, stages[i], &now[1 + i]);
    const DWORD states[] = {d3d8::rs::AlphaBlendEnable, d3d8::rs::SrcBlend,   d3d8::rs::DestBlend, d3d8::rs::BlendOp,
                            d3d8::rs::AlphaTestEnable,  d3d8::rs::AlphaFunc, d3d8::rs::FogEnable};
    for (unsigned i = 0; i < 7; ++i)
        call(device, slot::GetRenderState, states[i], &now[7 + i]);
    if (said > 0 && std::memcmp(now, last, sizeof now) == 0)
        return;
    std::memcpy(last, now, sizeof now);
    ++said;
    wchar_t name[64] = {};
    for (unsigned i = 0; i < 63 && text != nullptr && text[i] != '\0'; ++i)
        name[i] = static_cast<wchar_t>(static_cast<unsigned char>(text[i]));
    log::write(L"Graphics: a sharp name's letters are drawn with pixel shader %u, colour op %u(%X,%X), alpha op %u(%X,%X), "
               L"blend %u %u/%u op %u, alpha test %u func %u, fog %u; colour %08X, depth %g, \"%s\" ([graphics] "
               L"nameplates.font=sharp)",
               now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], now[8], now[9], now[10], now[11], now[12],
               now[13], colour, static_cast<double>(depth), name);
}

void trace_frame(float asked)
{
    ++g_trace.frame;
    const ULONGLONG now = GetTickCount64();
    if (asked != g_trace.asked) {
        g_trace.asked = asked;
        if (asked > 0.0f && g_trace.until == 0) {
            g_trace.until = now + static_cast<ULONGLONG>(asked * 1000.0f);
            g_trace.frame = 0;
            QueryPerformanceFrequency(&g_trace.frequency);
            QueryPerformanceCounter(&g_trace.start);
            g_trace.rows = "frame,ms,actor,name,game_size,letters_px,picture_px,scale,at_x,at_y,left,top,width,height\n";
            log::write(L"Graphics: tracing the names over characters' heads for %g s ([graphics] nameplates.trace)",
                       static_cast<double>(asked));
        }
    }
    if (g_trace.until == 0 || now < g_trace.until)
        return;
    g_trace.until = 0;
    const std::wstring folder = logs_folder();
    const std::wstring file = folder + L"nameplates-trace-" + log::file_stamp() + L".csv";
    CreateDirectoryW(folder.c_str(), nullptr);
    const HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    const bool ok = out != INVALID_HANDLE_VALUE &&
                    WriteFile(out, g_trace.rows.data(), static_cast<DWORD>(g_trace.rows.size()), &wrote, nullptr);
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    log::write(ok ? L"Graphics: the names over characters' heads traced into %ls ([graphics] nameplates.trace)"
                  : L"Graphics: couldn't write the names' trace to %ls ([graphics] nameplates.trace)",
               file.c_str());
    g_trace.rows.clear();
    g_trace.rows.shrink_to_fit();
}

void trace_row(const Row& row)
{
    if (g_trace.until == 0 || g_trace.rows.size() > (64u << 20))
        return;
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const double ms = static_cast<double>(now.QuadPart - g_trace.start.QuadPart) * 1000.0 /
                      static_cast<double>(g_trace.frequency.QuadPart);
    char text[400];
    std::snprintf(text, sizeof text, "%u,%.3f,%p,\"%.40s\",%.6f,%.4f,%.3f,%.5f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", g_trace.frame,
                  ms, row.actor, row.text != nullptr ? row.text : "", static_cast<double>(row.size),
                  static_cast<double>(row.exact), static_cast<double>(row.picture_px), static_cast<double>(row.scale),
                  static_cast<double>(row.at_x), static_cast<double>(row.at_y), static_cast<double>(row.left),
                  static_cast<double>(row.top), static_cast<double>(row.width), static_cast<double>(row.height));
    g_trace.rows += text;
}

} // namespace vanacore::nameplates::debug
