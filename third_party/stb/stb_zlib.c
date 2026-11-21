/* stb_image.h's zlib decoder alone, for VanaCore's zip reader (src/core/zipread.cpp): no image format, no stdio. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_ZLIB
#define STBI_SUPPORT_ZLIB
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"
