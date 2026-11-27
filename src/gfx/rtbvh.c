/* A bounding-volume tree of triangles (rtbvh.h), leaves of up to 4 triangles. A node is split where its two sides cost a ray least
 * (the surface area heuristic: each side's box by its area and its triangle count, tried between BINS slices of the centres' span
 * on each axis). Split at the median of the centres along their widest side, as it was, a zone's big floor and wall triangles sat
 * in boxes with its small things, the boxes overlapped, and a ray went through several times the nodes it needed: the walk was
 * most of the ray-traced reflections' time. The median is still the split where the heuristic finds none, and where a deeper
 * uneven split could take a leaf past DEPTH. Built once a zone, not per frame. */
#include "rtbvh.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LEAF 4
#define STACK 192
#define BINS 16
/* no node deeper than this: the shader's walk (bvh_trace) keeps one node by for each level it has gone down, 30 at most, and
 * passes by what it has no room for - a hole in the reflection */
#define DEPTH 28

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
    int b, e, depth;
} Work;

typedef struct
{
    float lo[3], hi[3];
    int n;
} Bin;

static void bin_clear(Bin* b)
{
    b->n = 0;
    for (int k = 0; k < 3; ++k)
        b->lo[k] = INFINITY, b->hi[k] = -INFINITY;
}

/* `b` grown to hold the box lo, hi (three floats each) and n more triangles */
static void bin_add(Bin* b, const float* lo, const float* hi, int n)
{
    b->n += n;
    for (int k = 0; k < 3; ++k)
    {
        if (lo[k] < b->lo[k])
            b->lo[k] = lo[k];
        if (hi[k] > b->hi[k])
            b->hi[k] = hi[k];
    }
}

/* half a box's surface area (the chance a ray meets it, to scale) */
static float bin_area(const Bin* b)
{
    const float x = b->hi[0] - b->lo[0], y = b->hi[1] - b->lo[1], z = b->hi[2] - b->lo[2];
    return x * y + y * z + z * x;
}

/* which of BINS slices a centre `c` falls in, the slices from `lo` on, `k` slices to a unit */
static int bin_of(float c, float lo, float k)
{
    const int s = (int)((c - lo) * k);
    return s < 0 ? 0 : s > BINS - 1 ? BINS - 1 : s;
}

/* how many times `count` halves (rounded up) down to 1: the levels a median split of that many needs at most */
static int levels(int count)
{
    int n = 0;
    while (n < 30 && (1 << n) < count)
        ++n;
    return n;
}

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

/* idx[b..e) split in two by the surface area heuristic: -1 for no split found, else where the second side starts, the first
 * side's triangles moved before it. cen the centres (three floats a triangle), box the triangles' boxes (six: min, max), clo and
 * chi the span of these triangles' centres. Each axis is cut into BINS slices; of the planes between slices, the one whose two
 * sides' boxes, each by its area and count, come to least. */
static int sah_split(uint32_t* idx, const float* cen, const float* box, int b, int e, const float* clo, const float* chi)
{
    float least = INFINITY, lo = 0, kk = 0;
    int axis = -1, at = 0;
    for (int ax = 0; ax < 3; ++ax)
    {
        const float span = chi[ax] - clo[ax];
        if (!(span > 0))
            continue;
        const float k = (float)BINS / span;
        Bin bins[BINS];
        for (int s = 0; s < BINS; ++s)
            bin_clear(&bins[s]);
        for (int i = b; i < e; ++i)
        {
            const size_t t = idx[i];
            bin_add(&bins[bin_of(cen[t * 3 + ax], clo[ax], k)], box + t * 6, box + t * 6 + 3, 1);
        }
        /* cost[s], n[s]: the slices from s on, together */
        float cost[BINS];
        int n[BINS];
        Bin side;
        bin_clear(&side);
        for (int s = BINS - 1; s > 0; --s)
        {
            bin_add(&side, bins[s].lo, bins[s].hi, bins[s].n);
            n[s] = side.n, cost[s] = side.n ? bin_area(&side) * (float)side.n : 0;
        }
        bin_clear(&side);
        for (int s = 1; s < BINS; ++s)
        {
            bin_add(&side, bins[s - 1].lo, bins[s - 1].hi, bins[s - 1].n);
            if (!side.n || !n[s])
                continue;
            const float c = bin_area(&side) * (float)side.n + cost[s];
            if (c < least)
                least = c, axis = ax, at = s, lo = clo[ax], kk = k;
        }
    }
    if (axis < 0)
        return -1;
    int i = b, j = e - 1;
    while (i <= j)
    {
        if (bin_of(cen[(size_t)idx[i] * 3 + axis], lo, kk) < at)
            ++i;
        else
        {
            const uint32_t t = idx[i];
            idx[i] = idx[j], idx[j] = t;
            --j;
        }
    }
    return i > b && i < e ? i : -1;
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
    float* box = (float*)malloc((size_t)ntris * 6 * sizeof(float));
    uint32_t* idx = (uint32_t*)malloc((size_t)ntris * sizeof(uint32_t));
    Node* nodes = (Node*)malloc((size_t)cap * sizeof(Node));
    if (!cen || !box || !idx || !nodes)
    {
        free(cen), free(box), free(idx), free(nodes);
        return NULL;
    }
    for (uint32_t t = 0; t < ntris; ++t)
    {
        idx[t] = t;
        for (int k = 0; k < 3; ++k)
        {
            const float x0 = corner(verts, stride, t, 0)[k], x1 = corner(verts, stride, t, 1)[k], x2 = corner(verts, stride, t, 2)[k];
            cen[(size_t)t * 3 + k] = (x0 + x1 + x2) * (1.0f / 3.0f);
            box[(size_t)t * 6 + k] = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
            box[(size_t)t * 6 + 3 + k] = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
        }
    }
    Work st[STACK];
    int sp = 0;
    uint32_t nn = 1;
    st[sp++] = (Work){ 0, 0, (int)ntris, 0 };
    while (sp)
    {
        const Work w = st[--sp];
        Node* n = &nodes[w.node];
        float lo[3] = { INFINITY, INFINITY, INFINITY }, hi[3] = { -INFINITY, -INFINITY, -INFINITY };
        float clo[3] = { INFINITY, INFINITY, INFINITY }, chi[3] = { -INFINITY, -INFINITY, -INFINITY };
        for (int i = w.b; i < w.e; ++i)
        {
            const uint32_t t = idx[i];
            for (int k = 0; k < 3; ++k)
            {
                if (box[(size_t)t * 6 + k] < lo[k])
                    lo[k] = box[(size_t)t * 6 + k];
                if (box[(size_t)t * 6 + 3 + k] > hi[k])
                    hi[k] = box[(size_t)t * 6 + 3 + k];
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
        /* (an uneven split only while the larger side, halved from there on, still ends by DEPTH; a median split's sides always do) */
        int mid = w.depth + 1 + levels(count) <= DEPTH ? sah_split(idx, cen, box, w.b, w.e, clo, chi) : -1;
        if (mid < 0)
        {
            int axis = 0;
            for (int k = 1; k < 3; ++k)
                if (chi[k] - clo[k] > chi[axis] - clo[axis])
                    axis = k;
            mid = (w.b + w.e) / 2;
            if (chi[axis] > clo[axis])
                select_nth(idx, cen, axis, w.b, w.e, mid);
        }
        n->a = nn, n->count = 0;
        st[sp++] = (Work){ nn + 1, mid, w.e, w.depth + 1 };
        st[sp++] = (Work){ nn, w.b, mid, w.depth + 1 };
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
    free(cen), free(box), free(idx), free(nodes);
    return out;
}

uint8_t* rtbvh_build(const float* verts, uint32_t stride, uint32_t ntris, uint32_t* bytes, uint32_t* node_bytes)
{
    return rtbvh_build_ex(verts, stride, ntris, NULL, 0, bytes, node_bytes, NULL);
}
