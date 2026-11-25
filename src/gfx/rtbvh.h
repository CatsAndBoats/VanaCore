/* rtbvh.h: a bounding-volume tree of triangles, for the ray-traced reflections (refl_rt) the pixel shader walks (fx_ssr's bvh_trace).
 * The game is a 32-bit process, where Windows shows no DXR to the card (the tier is 0), so the rays are traced in the shader
 * against a tree in a plain buffer. */
#pragma once
#include <stdint.h>

/* The tree of `ntris` triangles, their corners three to a triangle `stride_floats` floats apart (x, y, z first), in the world.
 * One malloc'd blob (free it): the nodes, 32 bytes each (bounds min xyz, a; bounds max xyz, count: a leaf has count > 0 and its
 * triangles from a, an inner node's children are a and a + 1), root first, then the triangles in the leaves' order, 36 bytes
 * each: a corner, then the edges to the other two. *bytes the blob's size, *node_bytes where the triangles start. NULL: nothing
 * to build, or no room. */
uint8_t* rtbvh_build(const float* verts, uint32_t stride_floats, uint32_t ntris, uint32_t* bytes, uint32_t* node_bytes);
/* The same with `pwords` 32-bit words of a triangle's own (`payload`, in the triangles' order as given), kept in the tree's order
 * after the triangles: triangle i's from *payload_at + i * 4 * pwords. */
uint8_t* rtbvh_build_ex(const float* verts, uint32_t stride_floats, uint32_t ntris, const uint32_t* payload, uint32_t pwords, uint32_t* bytes,
    uint32_t* node_bytes, uint32_t* payload_at);
