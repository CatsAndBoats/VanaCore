/* The guest heap, for a native process: this process's own heap (native.c). */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16-byte aligned; zeroed when `zero`. 0 on failure. */
uint32_t gheap_alloc(uint32_t size, int zero);
void gheap_free(uint32_t p);

#ifdef __cplusplus
}
#endif
