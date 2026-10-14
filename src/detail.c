#include "detail.h"
#include <stdlib.h>
#include <string.h>
#include <float.h>
static MmBox root_box(void) {
    return (MmBox){{0, 0, 0}, {DETAIL_SIZE_MM, DETAIL_SIZE_MM, DETAIL_SIZE_MM}};
}
static bool leaf(const DetailNode *n) {
    return n->child[0] == NULL;
}
static DetailNode *new_node(DetailChunk *d, Voxel value) {
    if (!d->unlimited && d->nodes >= DETAIL_MAX_NODES)
        return NULL;
    DetailNode *n = calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->value = value;
    d->nodes++;
    return n;
}
static void free_node(DetailChunk *d, DetailNode *n) {
    if (!n)
        return;
    free_node(d, n->child[0]);
    free_node(d, n->child[1]);
    free(n);
    d->nodes--;
}
DetailChunk *detail_create(Voxel value) {
    if (!voxel_valid(value))
        return NULL;
    DetailChunk *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->root = new_node(d, value);
    if (!d->root) {
        free(d);
        return NULL;
    }
    return d;
}
void detail_destroy(DetailChunk *d) {
    if (d) {
        free_node(d, d->root);
        free(d);
    }
}
static void collapse(DetailChunk *d, DetailNode *n) {
    if (leaf(n) || !leaf(n->child[0]) || !leaf(n->child[1]) ||
        !voxel_equal(n->child[0]->value, n->child[1]->value))
        return;
    n->value = n->child[0]->value;
    free_node(d, n->child[0]);
    free_node(d, n->child[1]);
    n->child[0] = n->child[1] = NULL;
}
static bool split(DetailChunk *d, DetailNode *n, int axis, int cut) {
    DetailNode *a = new_node(d, n->value), *b = new_node(d, n->value);
    if (!a || !b) {
        free_node(d, a);
        free_node(d, b);
        return false;
    }
    n->axis = axis;
    n->cut = cut;
    n->child[0] = a;
    n->child[1] = b;
    return true;
}
static DetailNode *clone_node(DetailChunk *d, const DetailNode *source) {
    DetailNode *n = new_node(d, source->value);
    if (!n)
        return NULL;
    n->axis = source->axis;
    n->cut = source->cut;
    if (!leaf(source)) {
        n->child[0] = clone_node(d, source->child[0]);
        n->child[1] = clone_node(d, source->child[1]);
        if (!n->child[0] || !n->child[1]) {
            free_node(d, n);
            return NULL;
        }
    }
    return n;
}
DetailChunk *detail_clone(const DetailChunk *source) {
    if (!source)
        return NULL;
    DetailChunk *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->unlimited = source->unlimited;
    d->root = clone_node(d, source->root);
    if (!d->root) {
        free(d);
        return NULL;
    }
    return d;
}
static DetailNode *build_node(DetailChunk *d, MmBox box, DetailBuildFn fn, void *ctx, int depth) {
    Voxel value = {MAT_AIR, 0, 293.15f};
    int axis = -1, cut = 0;
    if (!fn(box, &value, &axis, &cut, ctx) || !voxel_valid(value))
        return NULL;
    DetailNode *n = new_node(d, value);
    if (!n)
        return NULL;
    if (axis == -1)
        return n;
    if (axis < 0 || axis > 2 || cut <= box.lo[axis] || cut >= box.hi[axis] || depth >= DETAIL_MAX_DEPTH) {
        free_node(d, n);
        return NULL;
    }
    n->axis = axis;
    n->cut = cut;
    MmBox low = box, high = box;
    low.hi[axis] = high.lo[axis] = cut;
    n->child[0] = build_node(d, low, fn, ctx, depth + 1);
    n->child[1] = build_node(d, high, fn, ctx, depth + 1);
    if (!n->child[0] || !n->child[1]) {
        free_node(d, n);
        return NULL;
    }
    collapse(d, n);
    return n;
}
static DetailChunk *build(DetailBuildFn generator, void *context, bool unlimited) {
    if (!generator)
        return NULL;
    DetailChunk *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    d->unlimited = unlimited;
    d->root = build_node(d, root_box(), generator, context, 0);
    if (!d->root) {
        free(d);
        return NULL;
    }
    return d;
}
DetailChunk *detail_build(DetailBuildFn generator, void *context) {
    return build(generator, context, false);
}
DetailChunk *detail_build_unlimited(DetailBuildFn generator, void *context) {
    return build(generator, context, true);
}
static bool overlaps(MmBox a, MmBox b) {
    for (int i = 0; i < 3; i++)
        if (a.hi[i] <= b.lo[i] || a.lo[i] >= b.hi[i])
            return false;
    return true;
}
static bool contains(MmBox a, MmBox b) {
    for (int i = 0; i < 3; i++)
        if (a.lo[i] > b.lo[i] || a.hi[i] < b.hi[i])
            return false;
    return true;
}
static bool paint(DetailChunk *d, DetailNode *n, MmBox bounds, MmBox box, Voxel value, int depth) {
    if (!overlaps(bounds, box) || (leaf(n) && voxel_equal(n->value, value)))
        return true;
    if (contains(box, bounds)) {
        free_node(d, n->child[0]);
        free_node(d, n->child[1]);
        n->child[0] = n->child[1] = NULL;
        n->value = value;
        return true;
    }
    if (leaf(n)) {
        if (depth >= DETAIL_MAX_DEPTH)
            return false;
        int axis = -1, cut = 0;
        for (int i = 0; i < 3; i++) {
            if (box.lo[i] > bounds.lo[i] && box.lo[i] < bounds.hi[i]) {
                axis = i;
                cut = box.lo[i];
                break;
            }
            if (box.hi[i] > bounds.lo[i] && box.hi[i] < bounds.hi[i]) {
                axis = i;
                cut = box.hi[i];
                break;
            }
        }
        if (axis < 0 || !split(d, n, axis, cut))
            return false;
    }
    MmBox low = bounds, high = bounds;
    low.hi[n->axis] = n->cut;
    high.lo[n->axis] = n->cut;
    if (!paint(d, n->child[0], low, box, value, depth + 1) ||
        !paint(d, n->child[1], high, box, value, depth + 1))
        return false;
    collapse(d, n);
    return true;
}
bool detail_paint_build(DetailChunk *d, MmBox box, Voxel value) {
    if (!d || !voxel_valid(value))
        return false;
    for (int i = 0; i < 3; i++)
        if (box.lo[i] < 0 || box.hi[i] > DETAIL_SIZE_MM || box.lo[i] >= box.hi[i])
            return false;
    return paint(d, d->root, root_box(), box, value, 0);
}
bool detail_paint(DetailChunk *d, MmBox box, Voxel value) {
    if (!d || !voxel_valid(value))
        return false;
    for (int i = 0; i < 3; i++)
        if (box.lo[i] < 0 || box.hi[i] > DETAIL_SIZE_MM || box.lo[i] >= box.hi[i])
            return false;
    /* Prototype edits are transactional, including allocation/depth-budget failures. */
    DetailChunk next = {0};
    next.root = clone_node(&next, d->root);
    if (!next.root)
        return false;
    if (!paint(&next, next.root, root_box(), box, value, 0)) {
        free_node(&next, next.root);
        return false;
    }
    free_node(d, d->root);
    *d = next;
    return true;
}
Voxel detail_get(const DetailChunk *d, int x, int y, int z) {
    if (!d || x < 0 || y < 0 || z < 0 || x >= DETAIL_SIZE_MM || y >= DETAIL_SIZE_MM || z >= DETAIL_SIZE_MM)
        return (Voxel){MAT_AIR, 0, 293.15f};
    int point[3] = {x, y, z};
    const DetailNode *n = d->root;
    while (!leaf(n))
        n = n->child[point[n->axis] >= n->cut];
    return n->value;
}
static bool visit(const DetailNode *n, MmBox bounds, DetailLeafFn fn, void *context) {
    if (leaf(n))
        return fn(bounds, n->value, context);
    MmBox low = bounds, high = bounds;
    low.hi[n->axis] = n->cut;
    high.lo[n->axis] = n->cut;
    return visit(n->child[0], low, fn, context) && visit(n->child[1], high, fn, context);
}
bool detail_visit(const DetailChunk *d, DetailLeafFn fn, void *context) {
    return d && fn && visit(d->root, root_box(), fn, context);
}
static bool empty_plane(const DetailNode *n, int axis, int slice, MmRect rect, DetailRectFn fn, void *ctx) {
    if (rect.lo[0] >= rect.hi[0] || rect.lo[1] >= rect.hi[1])
        return true;
    if (leaf(n))
        return material_get(n->value.material)->solid ? true : fn(rect, ctx);
    if (n->axis == axis)
        return empty_plane(n->child[slice >= n->cut], axis, slice, rect, fn, ctx);
    int dim = n->axis == (axis + 1) % 3 ? 0 : 1;
    MmRect low = rect, high = rect;
    if (low.hi[dim] > n->cut)
        low.hi[dim] = n->cut;
    if (high.lo[dim] < n->cut)
        high.lo[dim] = n->cut;
    return empty_plane(n->child[0], axis, slice, low, fn, ctx) &&
           empty_plane(n->child[1], axis, slice, high, fn, ctx);
}
bool detail_empty_plane(const DetailChunk *d, int axis, int slice, MmRect rect, DetailRectFn fn, void *ctx) {
    if (!d || !fn || axis < 0 || axis > 2 || slice < 0 || slice >= DETAIL_SIZE_MM)
        return false;
    for (int i = 0; i < 2; i++)
        if (rect.lo[i] < 0 || rect.hi[i] > DETAIL_SIZE_MM)
            return false;
    return empty_plane(d->root, axis, slice, rect, fn, ctx);
}
static bool intersect(MmBox box, const double *o, const double *v, double max, double *enter, double *leave,
                      int *normal) {
    *enter = 0;
    *leave = max;
    normal[0] = normal[1] = normal[2] = 0;
    for (int a = 0; a < 3; a++) {
        if (fabs(v[a]) < 1e-12) {
            if (o[a] < box.lo[a] || o[a] >= box.hi[a])
                return false;
            continue;
        }
        double t0 = (box.lo[a] - o[a]) / v[a], t1 = (box.hi[a] - o[a]) / v[a];
        int sign = -1;
        if (t0 > t1) {
            double t = t0;
            t0 = t1;
            t1 = t;
            sign = 1;
        }
        if (t0 >= *enter) {
            *enter = t0;
            normal[0] = normal[1] = normal[2] = 0;
            normal[a] = sign;
        }
        if (t1 < *leave)
            *leave = t1;
        if (*enter > *leave)
            return false;
    }
    return *leave >= 0;
}
static void ray_node(const DetailNode *n, MmBox bounds, const double *o, const double *v, double max,
                     DetailHit *best) {
    double enter, leave;
    int normal[3];
    if (!intersect(bounds, o, v, max, &enter, &leave, normal) || (best->hit && enter >= best->distance_mm))
        return;
    if (leaf(n)) {
        if (!material_get(n->value.material)->solid)
            return;
        best->hit = true;
        best->distance_mm = enter;
        best->voxel = n->value;
        for (int a = 0; a < 3; a++) {
            double p = o[a] + v[a] * enter - normal[a] * 1e-5;
            int cell = (int)floor(p);
            if (cell < bounds.lo[a])
                cell = bounds.lo[a];
            if (cell >= bounds.hi[a])
                cell = bounds.hi[a] - 1;
            best->mm[a] = cell;
            best->normal[a] = normal[a];
        }
        return;
    }
    MmBox low = bounds, high = bounds;
    low.hi[n->axis] = n->cut;
    high.lo[n->axis] = n->cut;
    int first = v[n->axis] < 0 ? 1 : 0;
    ray_node(n->child[first], first ? high : low, o, v, max, best);
    ray_node(n->child[1 - first], first ? low : high, o, v, max, best);
}
DetailHit detail_raycast(const DetailChunk *d, DVec3 origin, Vec3 direction, double max_mm) {
    DetailHit hit = {0};
    if (!d || !isfinite(origin.x) || !isfinite(origin.y) || !isfinite(origin.z) || !isfinite(max_mm) ||
        max_mm < 0)
        return hit;
    Vec3 v = vnormalize(direction);
    if (!isfinite(v.x) || !isfinite(v.y) || !isfinite(v.z) || vdot(v, v) < .5f)
        return hit;
    double o[3] = {origin.x, origin.y, origin.z}, dir[3] = {v.x, v.y, v.z};
    ray_node(d->root, root_box(), o, dir, max_mm, &hit);
    return hit;
}
typedef struct {
    uint16_t material;
    uint64_t volume;
} Volume;
static bool add_volume(MmBox b, Voxel v, void *ctx) {
    Volume *sum = ctx;
    if (v.material == sum->material)
        sum->volume += (uint64_t)(b.hi[0] - b.lo[0]) * (b.hi[1] - b.lo[1]) * (b.hi[2] - b.lo[2]);
    return true;
}
uint64_t detail_material_volume(const DetailChunk *d, uint16_t material) {
    Volume sum = {material, 0};
    detail_visit(d, add_volume, &sum);
    return sum.volume;
}
size_t detail_storage_bytes(const DetailChunk *d) {
    return d ? sizeof(*d) + d->nodes * sizeof(DetailNode) : 0;
}
static size_t node_encoded_size(const DetailNode *n) {
    return leaf(n) ? 9 : 3 + node_encoded_size(n->child[0]) + node_encoded_size(n->child[1]);
}
size_t detail_encoded_size(const DetailChunk *d) {
    return node_encoded_size(d->root);
}
static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void encode_node(const DetailNode *n, unsigned char *data, size_t *at) {
    if (leaf(n)) {
        data[(*at)++] = 0;
        put32(data + *at, (uint32_t)n->value.material | ((uint32_t)n->value.flags << 16));
        *at += 4;
        uint32_t bits;
        memcpy(&bits, &n->value.temperature_k, 4);
        put32(data + *at, bits);
        *at += 4;
    } else {
        data[(*at)++] = (unsigned char)(n->axis + 1);
        data[(*at)++] = (unsigned char)n->cut;
        data[(*at)++] = (unsigned char)(n->cut >> 8);
        encode_node(n->child[0], data, at);
        encode_node(n->child[1], data, at);
    }
}
void detail_encode(const DetailChunk *d, unsigned char *data, size_t *at) {
    encode_node(d->root, data, at);
}
static bool decode_node(DetailChunk *d, DetailNode *n, MmBox bounds, const unsigned char *data, size_t length,
                        size_t *at, int depth) {
    if (*at >= length)
        return false;
    int tag = data[(*at)++];
    if (tag == 0) {
        if (length - *at < 8)
            return false;
        uint32_t packed = get32(data + *at), bits = get32(data + *at + 4);
        *at += 8;
        Voxel value = {(uint16_t)packed, (uint16_t)(packed >> 16), 0};
        memcpy(&value.temperature_k, &bits, 4);
        if (!voxel_valid(value))
            return false;
        n->value = value;
        return true;
    }
    if (tag < 1 || tag > 3 || length - *at < 2 || depth >= DETAIL_MAX_DEPTH)
        return false;
    int axis = tag - 1, cut = data[*at] | data[*at + 1] << 8;
    *at += 2;
    if (cut <= bounds.lo[axis] || cut >= bounds.hi[axis] || !split(d, n, axis, cut))
        return false;
    MmBox low = bounds, high = bounds;
    low.hi[axis] = cut;
    high.lo[axis] = cut;
    if (!decode_node(d, n->child[0], low, data, length, at, depth + 1) ||
        !decode_node(d, n->child[1], high, data, length, at, depth + 1))
        return false;
    collapse(d, n);
    return true;
}
static DetailChunk *decode(const unsigned char *data, size_t length, size_t *at, bool unlimited) {
    DetailChunk *d = detail_create((Voxel){MAT_AIR, 0, 293.15f});
    if (!d)
        return NULL;
    d->unlimited = unlimited;
    if (!decode_node(d, d->root, root_box(), data, length, at, 0)) {
        detail_destroy(d);
        return NULL;
    }
    return d;
}

DetailChunk *detail_decode(const unsigned char *data, size_t length, size_t *at) {
    return decode(data, length, at, false);
}
DetailChunk *detail_decode_unlimited(const unsigned char *data, size_t length, size_t *at) {
    return decode(data, length, at, true);
}
