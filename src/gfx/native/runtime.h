/* What d3d8.c needs of the recompiler's runtime, for a native 32-bit process (VanaCore.Core inside
 * the game): the game's memory is this process's memory, so a guest address is a pointer and
 * reads and writes are plain loads and stores. The upstream runtime (xi-on-mac runtime/guest.h)
 * maps a 4 GB guest window into a 64-bit process instead; the names are kept so the ported files
 * stay close to upstream. */
#pragma once

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The registers a shim reads and writes: the stack pointer at the call (its return address) and
 * the return value. native.c's trampolines fill it from the real x86 stack. */
typedef struct Guest
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
} Guest;

#define GUEST_PTR(a) ((unsigned char*)(uintptr_t)(uint32_t)(a))

static __inline uint8_t rd8(uint32_t a) { return *GUEST_PTR(a); }
static __inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, GUEST_PTR(a), 2); return v; }
static __inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, GUEST_PTR(a), 4); return v; }
static __inline float rdf32(uint32_t a) { float v; memcpy(&v, GUEST_PTR(a), 4); return v; }
static __inline void wr8(uint32_t a, uint8_t v) { *GUEST_PTR(a) = v; }
static __inline void wr16(uint32_t a, uint16_t v) { memcpy(GUEST_PTR(a), &v, 2); }
static __inline void wr32(uint32_t a, uint32_t v) { memcpy(GUEST_PTR(a), &v, 4); }
static __inline void wrf32(uint32_t a, float v) { memcpy(GUEST_PTR(a), &v, 4); }

/* One line (or more) into VanaCore's log (vanacore_gfx_set_log). */
void rt_log(const char* fmt, ...);

/* FFXiMain.dll's address range, for FFXI_DRAWLOG's return addresses; 0 until it is known. */
extern uint32_t rt_image_lo, rt_image_hi;

#ifdef __cplusplus
}
#endif
