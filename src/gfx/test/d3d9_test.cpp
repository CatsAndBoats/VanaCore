// renderer=d3d9 (src/core/d3d9.cpp) for d3d8_test.c: its IDirect3D8, and VanaCore's log as lines on the
// console.
#include <cstdarg>
#include <cstdio>

#include "../../core/d3d9.hpp"
#include "../../core/log.hpp"

namespace vanacore::log {

void write(const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    std::wprintf(L"  [d3d9] ");
    std::vwprintf(format, args);
    std::wprintf(L"\n");
    va_end(args);
}

} // namespace vanacore::log

extern "C" void* vanacore_d3d9_test_create(void)
{
    return vanacore::d3d9::direct3d_create(false);
}
