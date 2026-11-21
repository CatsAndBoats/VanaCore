/* COM methods for a native process (native.c): upstream (xi-on-mac runtime/portable/thunk.h) gives
 * every import and COM method a synthetic guest address that the recompiled game calls. Here the
 * game calls real function pointers, so thunk_for hands out a small stub per method that loads the
 * x86 stack into a Guest and runs the shim registered for the name. A shim reads its arguments off
 * the stack and pops them itself (stdcall), exactly as upstream. */
#pragma once

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*Shim)(Guest* g);

typedef struct ShimDef
{
    const char* dll; /* NULL: any DLL */
    const char* name;
    Shim fn;
} ShimDef;

/* Adds a NULL-terminated table of shims. */
void thunk_register(const ShimDef* defs);
/* The address of a stub that runs dll!name's shim, made on first request. A name with no shim gets
 * one that returns D3DERR_INVALIDCALL, pops the arguments the D3D8 ABI gives the method, and logs
 * the name once. */
uint32_t thunk_for(const char* dll, const char* name);

/* Writing shims. Arguments are counted from 0 (the first parameter: `this` for a COM method). */
#define ARG(n) rd32(g->esp + 4u + 4u * (n))
#define ARGP(n) ((void*)GUEST_PTR(ARG(n)))
#define ARGS(n) ((const char*)GUEST_PTR(ARG(n)))
/* stdcall return: pops the return address and nargs arguments */
#define RET(v, nargs) do { g->eax = (uint32_t)(v); g->esp += 4u + 4u * (nargs); return; } while (0)
/* cdecl return: the caller pops the arguments */
#define RETC(v) do { g->eax = (uint32_t)(v); g->esp += 4u; return; } while (0)

#ifdef __cplusplus
}
#endif
