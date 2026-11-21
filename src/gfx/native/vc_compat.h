/* Forced into every file of the graphics library (/FI): the upstream files print their diagnostics
 * with fprintf(stderr, ...), and a game process has no console to show them. They go to VanaCore's
 * log instead (native.c); fprintf to a real file (FFXI_DRAWLOG) is unchanged. */
#pragma once

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

int vc_fprintf(FILE* f, const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#ifndef VC_COMPAT_NO_FPRINTF
#define fprintf vc_fprintf
#endif
