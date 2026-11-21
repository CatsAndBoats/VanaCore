/* What VanaCore (in the game's 32-bit process) and VanaCore.Dlss.exe --hw (64-bit) share for the graphics card's
 * readings: a block of memory named VANA_HW_MAP<game's pid>. NVIDIA's NVML (nvml.dll) is 64-bit only, so the helper
 * reads it once a second and writes it here; the game reads it for /vana stats and /vana telemetry. Both sides
 * compile it (C, fixed-size fields, no pointers). */
#ifndef VANA_HW_IPC_H
#define VANA_HW_IPC_H

#include <stdint.h>

#define VANA_HW_VERSION 1
#define VANA_HW_MAP L"Local\\VanaCoreHw."

enum {
    VANA_HW_STARTING = 0,
    VANA_HW_RUNNING = 1,
    VANA_HW_UNAVAILABLE = 2, /* no reading (no NVIDIA card, no NVML): message says why */
};

typedef struct VanaHwShared {
    uint32_t version;
    volatile int32_t state; /* VANA_HW_* */
    char message[160];
    volatile uint32_t tick; /* polls made: changes each second while it runs */
    char name[96];
    uint32_t gpu_mhz, gpu_max_mhz;
    int32_t temp;                /* C */
    uint32_t power_mw, limit_mw; /* what it draws, and the limit it's held to */
    uint32_t busy;               /* % of the time something ran on it */
    uint64_t vram_used, vram_total; /* bytes, all programs' */
    uint64_t throttle;           /* NVML's reasons the clocks are held down (bits) */
} VanaHwShared;

#endif
