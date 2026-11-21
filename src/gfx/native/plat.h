/* The platform calls d3d8.c makes (texture packs), on Windows (native.c). */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

extern const char plat_path_sep;

/* A whole file, malloc'd (the caller frees); NULL if it cannot be read. */
unsigned char* plat_read_file(const char* path, size_t* size);
/* 0 on success or when it exists already */
int plat_mkdir(const char* path);

typedef struct PlatDir PlatDir;
PlatDir* plat_dir_open(const char* path);
const char* plat_dir_next(PlatDir* d); /* file names, NULL at the end */
void plat_dir_close(PlatDir* d);

#ifdef __cplusplus
}
#endif
