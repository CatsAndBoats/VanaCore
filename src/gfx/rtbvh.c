/* A bounding-volume tree of triangles (rtbvh.h): split at the median of the triangles' centres along the widest side of those
 * centres, leaves of up to 4 triangles. Built once a zone from its collision, not per frame. */
#include "rtbvh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LEAF 4
#define STACK 192

typedef struct
{
    float bmin[3];
    uint32_t a;
    float bmax[3];
    uint32_t count;
} Node;

typedef struct
{
    uint32_t node;
    int b, e;
} Work;

static const float* corner(const float* v, uint32_t stride, uint32_t tri, int c)
{
    return v + ((size_t)tri * 3 + (size_t)c) * stride;
}

/* idx[lo..hi) arranged so idx[nth] has the nth smallest centre along `axis` (centres: three floats a triangle), the smaller before it */
static void select_nth(uint32_t* idx, const float* cen, int axis, int lo, int hi, int nth)
{
#define KEY(i) cen[(size_t)idx[i] * 3 + axis]
    while (hi - lo > 1)
    {
        const float pivot = KEY((lo + hi) / 2);
        int i = lo, j = hi - 1;
        while (i <= j)
        {
            while (KEY(i) < pivot)
                ++i;
            while (KEY(j) > pivot)
                --j;
            if (i <= j)
            {
                const uint32_t t = idx[i];
                idx[i] = idx[j], idx[j] = t;
                ++i, --j;
            }
        }
        if (nth <= j)
            hi = j + 1;
        else if (nth >= i)
            lo = i;
        else
            return;
    }
#undef KEY
}

uint8_t* rtbvh_build_ex(const float* verts, uint32_t stride, uint32_t ntris, const uint32_t* payload, uint32_t pwords, uint32_t* bytes,
    uint32_t* node_bytes, uint32_t* payload_at)
{
    if (!verts || !ntris || stride < 3 || ntris > (1u << 24))
        return NULL;
    const uint64_t cap = 2ull * ntris + 2;
    const uint64_t total = cap * sizeof(Node) + (uint64_t)ntris * (36 + 4ull * pwords);
    if (total > 0x7FFFFFFFull / 2)
        return NULL;
    float* cen = (float*)malloc((size_t)ntris * 3 * sizeof(float));
    uint32_t* idx = (uint32_t*)malloc((size_t)ntris * sizeof(uint32_t));
    Node* nodes = (Node*)malloc((size_t)cap * sizeof(Node));
    if (!cen || !idx || !nodes)
    {
        free(cen), free(idx), free(nodes);
        return NULL;
    }
    for (uint32_t t = 0; t < ntris; ++t)
    {
        idx[t] = t;
        for (int k = 0; k < 3; ++k)
            cen[(size_t)t * 3 + k] = (corner(verts, stride, t, 0)[k] + corner(verts, stride, t, 1)[k] + corner(verts, stride, t, 2)[k]) * (1.0f / 3.0f);
    }
    Work st[STACK];
    int sp = 0;
    uint32_t nn = 1;
    st[sp++] = (Work){ 0, 0, (int)ntris };
    while (sp)
    {
        const Work w = st[--sp];
        Node* n = &nodes[w.node];
        float lo[3] = { INFINITY, INFINITY, INFINITY }, hi[3] = { -INFINITY, -INFINITY, -INFINITY };
        float clo[3] = { INFINITY, INFINITY, INFINITY }, chi[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (int i = w.b; i < w.e; ++i)
        {
            const uint32_t t = idx[i];
            for (int c = 0; c < 3; ++c)
                for (int k = 0; k < 3; ++k)
                {
                    const float x = corner(verts, stride, t, c)[k];
                    if (x < lo[k])
                        lo[k] = x;
                    if (x > hi[k])
                        hi[k] = x;
                }
            for (int k = 0; k < 3; ++k)
            {
                const float x = cen[(size_t)t * 3 + k];
                if (x < clo[k])
                    clo[k] = x;
                if (x > chi[k])
                    chi[k] = x;
            }
        }
        memcpy(n->bmin, lo, sizeof lo), memcpy(n->bmax, hi, sizeof hi);
        const int count = w.e - w.b;
        if (count <= LEAF || sp > STACK - 4)
        {
            n->a = (uint32_t)w.b, n->count = (uint32_t)count;
            continue;
        }
        int axis = 0;
        for (int k = 1; k < 3; ++k)
            if (chi[k] - clo[k] > chi[axis] - clo[axis])
                axis = k;
        const int mid = (w.b + w.e) / 2;
        if (chi[axis] > clo[axis])
            select_nth(idx, cen, axis, w.b, w.e, mid);
        n->a = nn, n->count = 0;
        st[sp++] = (Work){ nn + 1, mid, w.e };
        st[sp++] = (Work){ nn, w.b, mid };
        nn += 2;
    }
    const uint32_t nb = nn * (uint32_t)sizeof(Node);
    const uint32_t size = nb + ntris * (36 + 4 * pwords);
    uint8_t* out = (uint8_t*)malloc(size);
    if (out)
    {
        memcpy(out, nodes, nb);
        float* tri = (float*)(out + nb);
        for (uint32_t i = 0; i < ntris; ++i, tri += 9)
        {
            const float* a = corner(verts, stride, idx[i], 0);
            const float* b = corner(verts, stride, idx[i], 1);
            const float* c = corner(verts, stride, idx[i], 2);
            for (int k = 0; k < 3; ++k)
                tri[k] = a[k], tri[3 + k] = b[k] - a[k], tri[6 + k] = c[k] - a[k];
        }
        if (payload && pwords)
        {
            uint32_t* pl = (uint32_t*)(out + nb + ntris * 36);
            for (uint32_t i = 0; i < ntris; ++i)
                memcpy(pl + (size_t)i * pwords, payload + (size_t)idx[i] * pwords, 4u * pwords);
        }
        *bytes = size, *node_bytes = nb;
        if (payload_at)
            *payload_at = nb + ntris * 36;
    }
    free(cen), free(idx), free(nodes);
    return out;
}

uint8_t* rtbvh_build(const float* verts, uint32_t stride, uint32_t ntris, uint32_t* bytes, uint32_t* node_bytes)
{
    return rtbvh_build_ex(verts, stride, ntris, NULL, 0, bytes, node_bytes, NULL);
}
