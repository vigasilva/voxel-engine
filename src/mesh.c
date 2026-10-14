#include "mesh.h"
#include "detail.h"
#include "terrain.h"
#include <stdlib.h>
#include <string.h>
#define HALO_SIZE (CHUNK_SIZE + 2)
static size_t halo_index(int x, int y, int z) {
    return ((size_t)(y + 1) * HALO_SIZE + (z + 1)) * HALO_SIZE + (x + 1);
}
static bool reserve(Mesh *m, size_t n) {
    if (n <= m->capacity)
        return true;
    size_t cap = m->capacity ? m->capacity * 2 : 256;
    while (cap < n)
        cap *= 2;
    Vertex *p = realloc(m->vertices, cap * sizeof(*p));
    if (!p)
        return false;
    m->vertices = p;
    m->capacity = cap;
    return true;
}
static bool emit_quad_mm(Mesh *m, int axis, int sign, int plane, int u, int v, int width, int height,
                         uint16_t material) {
    if (!reserve(m, m->count + 6))
        return false;
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    float corners[4][3] = {{0}};
    for (int i = 0; i < 4; i++) {
        corners[i][axis] = (float)plane / VOXEL_SIZE_MM;
        corners[i][ua] = (float)(u + ((i == 1 || i == 2) ? width : 0)) / VOXEL_SIZE_MM;
        corners[i][va] = (float)(v + (i >= 2 ? height : 0)) / VOXEL_SIZE_MM;
    }
    const int positive[] = {0, 1, 2, 0, 2, 3}, negative[] = {0, 3, 2, 0, 2, 1};
    const int *indices = sign > 0 ? positive : negative;
    const Material *mat = material_get(material);
    Vec3 color = v3(mat->color[0], mat->color[1], mat->color[2]);
    if (material == MAT_GRASS && !(axis == 1 && sign > 0))
        color = v3(.38f, .29f, .16f);
    float normal[3] = {0};
    normal[axis] = (float)sign;
    for (int i = 0; i < 6; i++) {
        const float *p = corners[indices[i]];
        m->vertices[m->count++] = (Vertex){v3(p[0], p[1], p[2]), v3(normal[0], normal[1], normal[2]), color};
    }
    m->surface_area_mm2 += (uint64_t)width * height;
    return true;
}
static bool emit_quad(Mesh *m, int axis, int sign, int plane, int u, int v, int width, int height,
                      uint16_t material) {
    return emit_quad_mm(m, axis, sign, plane * VOXEL_SIZE_MM, u * VOXEL_SIZE_MM, v * VOXEL_SIZE_MM,
                        width * VOXEL_SIZE_MM, height * VOXEL_SIZE_MM, material);
}
typedef struct {
    int axis, sign, plane, material;
    MmRect rect;
} SurfacePatch;
typedef struct {
    SurfacePatch *items;
    size_t count, capacity;
} Patches;
static int patch_u_compare(const void *a, const void *b) {
    const SurfacePatch *x = a, *y = b;
    int xv[] = {x->axis, x->sign, x->plane, x->material, x->rect.lo[1], x->rect.hi[1], x->rect.lo[0]};
    int yv[] = {y->axis, y->sign, y->plane, y->material, y->rect.lo[1], y->rect.hi[1], y->rect.lo[0]};
    for (int i = 0; i < 7; i++)
        if (xv[i] != yv[i])
            return xv[i] < yv[i] ? -1 : 1;
    return 0;
}
static int patch_v_compare(const void *a, const void *b) {
    const SurfacePatch *x = a, *y = b;
    int xv[] = {x->axis, x->sign, x->plane, x->material, x->rect.lo[0], x->rect.hi[0], x->rect.lo[1]};
    int yv[] = {y->axis, y->sign, y->plane, y->material, y->rect.lo[0], y->rect.hi[0], y->rect.lo[1]};
    for (int i = 0; i < 7; i++)
        if (xv[i] != yv[i])
            return xv[i] < yv[i] ? -1 : 1;
    return 0;
}
static void merge_patches(Patches *p, int dimension) {
    if (!p->count)
        return;
    qsort(p->items, p->count, sizeof(*p->items), dimension == 0 ? patch_u_compare : patch_v_compare);
    size_t out = 0;
    for (size_t i = 0; i < p->count; i++) {
        SurfacePatch *x = &p->items[i], *last = out ? &p->items[out - 1] : NULL;
        int other = 1 - dimension;
        if (last && x->axis == last->axis && x->sign == last->sign && x->plane == last->plane &&
            x->material == last->material && x->rect.lo[other] == last->rect.lo[other] &&
            x->rect.hi[other] == last->rect.hi[other] && x->rect.lo[dimension] == last->rect.hi[dimension])
            last->rect.hi[dimension] = x->rect.hi[dimension];
        else
            p->items[out++] = *x;
    }
    p->count = out;
}
typedef struct {
    Mesh *mesh;
    const World *world;
    CellCoord origin;
    int axis, sign, plane, chunk;
    uint16_t material;
    Patches *patches;
} FaceContext;
static bool emit_patch(MmRect rect, void *context) {
    FaceContext *f = context;
    if (f->patches) {
        Patches *p = f->patches;
        if (p->count == p->capacity) {
            size_t capacity = p->capacity ? p->capacity * 2 : 256;
            SurfacePatch *items = realloc(p->items, capacity * sizeof(*items));
            if (!items)
                return false;
            p->items = items;
            p->capacity = capacity;
        }
        p->items[p->count++] = (SurfacePatch){f->axis, f->sign, f->plane, f->material, rect};
        return true;
    }
    return emit_quad_mm(f->mesh, f->axis, f->sign, f->plane, rect.lo[0], rect.lo[1], rect.hi[0] - rect.lo[0],
                        rect.hi[1] - rect.lo[1], f->material);
}
static bool outside_patches(FaceContext *f, MmRect rect) {
    int axis = f->axis, ua = (axis + 1) % 3, va = (axis + 2) % 3;
    int64_t global[3] = {f->origin.x, f->origin.y, f->origin.z};
    global[axis] += f->sign > 0 ? CHUNK_SIZE : -1;
    if (!world_inside(f->world, global[0], global[1], global[2]))
        return emit_patch(rect, f);
    int cx = (int)(global[0] - f->world->origin.x) / CHUNK_SIZE;
    int cy = (int)(global[1] - f->world->origin.y) / CHUNK_SIZE;
    int cz = (int)(global[2] - f->world->origin.z) / CHUNK_SIZE;
    int index = world_chunk_index(cx, cy, cz);
    if (f->world->pending[index])
        return terrain_empty_plane(f->world, index, axis, f->sign > 0 ? 0 : 1999, rect, emit_patch, f);
    if (f->world->details[index])
        return detail_empty_plane(f->world->details[index], axis, f->sign > 0 ? 0 : DETAIL_SIZE_MM - 1, rect,
                                  emit_patch, f);
    const VoxelNode *neighbor = &f->world->chunks[index];
    if (!neighbor->detail)
        return material_get(neighbor->value.material)->solid ? true : emit_patch(rect, f);
    for (int v = rect.lo[1] / VOXEL_SIZE_MM; v <= (rect.hi[1] - 1) / VOXEL_SIZE_MM; v++)
        for (int u = rect.lo[0] / VOXEL_SIZE_MM; u <= (rect.hi[0] - 1) / VOXEL_SIZE_MM; u++) {
            int64_t point[3] = {global[0], global[1], global[2]};
            point[ua] += u;
            point[va] += v;
            if (material_get(world_get(f->world, point[0], point[1], point[2]).material)->solid)
                continue;
            MmRect part = rect;
            if (part.lo[0] < u * VOXEL_SIZE_MM)
                part.lo[0] = u * VOXEL_SIZE_MM;
            if (part.hi[0] > (u + 1) * VOXEL_SIZE_MM)
                part.hi[0] = (u + 1) * VOXEL_SIZE_MM;
            if (part.lo[1] < v * VOXEL_SIZE_MM)
                part.lo[1] = v * VOXEL_SIZE_MM;
            if (part.hi[1] > (v + 1) * VOXEL_SIZE_MM)
                part.hi[1] = (v + 1) * VOXEL_SIZE_MM;
            if (!emit_patch(part, f))
                return false;
        }
    return true;
}
static bool mesh_detail_leaf(MmBox bounds, Voxel value, void *context) {
    if (!material_get(value.material)->solid)
        return true;
    FaceContext *f = context;
    f->material = value.material;
    for (int axis = 0; axis < 3; axis++)
        for (int sign = -1; sign <= 1; sign += 2) {
            int ua = (axis + 1) % 3, va = (axis + 2) % 3;
            f->axis = axis;
            f->sign = sign;
            f->plane = sign > 0 ? bounds.hi[axis] : bounds.lo[axis];
            MmRect rect = {{bounds.lo[ua], bounds.lo[va]}, {bounds.hi[ua], bounds.hi[va]}};
            int sample = f->plane + (sign < 0 ? -1 : 0);
            if (sample < 0 || sample >= DETAIL_SIZE_MM) {
                if (!outside_patches(f, rect))
                    return false;
            } else if (!detail_empty_plane(f->world->details[f->chunk], axis, sample, rect, emit_patch, f))
                return false;
        }
    return true;
}
static int detail_neighbor(const World *w, int chunk, int axis, int sign) {

    int c[3] = {chunk % WORLD_CHUNKS_X, chunk / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z),
                (chunk / WORLD_CHUNKS_X) % WORLD_CHUNKS_Z};
    c[axis] += sign;
    if (c[0] < 0 || c[0] >= WORLD_CHUNKS_X || c[1] < 0 || c[1] >= WORLD_CHUNKS_Y || c[2] < 0 ||
        c[2] >= WORLD_CHUNKS_Z)
        return -1;
    int index = world_chunk_index(c[0], c[1], c[2]);
    return w->details[index] ? index : -1;
}
bool mesh_build_chunk(Mesh *m, const World *w, int chunk) {
    if (chunk < 0 || chunk >= CHUNK_COUNT)
        return false;
    m->count = 0;
    m->terrain_field = false;
    m->unit_faces = 0;
    m->surface_area_mm2 = 0;
    if (w->details[chunk]) {
        Patches patches = {0};
        FaceContext face = {.mesh = m,
                            .world = w,
                            .origin = world_chunk_origin(w, chunk),
                            .chunk = chunk,
                            .patches = &patches};
        bool ok = detail_visit(w->details[chunk], mesh_detail_leaf, &face);
        if (ok) {
            size_t before;
            do {
                before = patches.count;
                merge_patches(&patches, 0);
                merge_patches(&patches, 1);
            } while (patches.count < before);
            for (size_t i = 0; ok && i < patches.count; i++) {
                SurfacePatch p = patches.items[i];
                ok = emit_quad_mm(m, p.axis, p.sign, p.plane, p.rect.lo[0], p.rect.lo[1],
                                  p.rect.hi[0] - p.rect.lo[0], p.rect.hi[1] - p.rect.lo[1],
                                  (uint16_t)p.material);
            }
        }
        free(patches.items);
        return ok;
    }
    if (!w->chunks[chunk].detail && !material_get(w->chunks[chunk].value.material)->solid)
        return true;
    bool solid[MAT_COUNT];
    for (int i = 0; i < MAT_COUNT; i++)
        solid[i] = material_get((uint16_t)i)->solid;
    /* Read storage once into a one-cell halo, then sweep cached materials. */
    uint16_t halo[HALO_SIZE * HALO_SIZE * HALO_SIZE];
    CellCoord origin = world_chunk_origin(w, chunk);
    for (int y = -1; y <= CHUNK_SIZE; y++)
        for (int z = -1; z <= CHUNK_SIZE; z++)
            for (int x = -1; x <= CHUNK_SIZE; x++)
                halo[halo_index(x, y, z)] = world_get(w, origin.x + x, origin.y + y, origin.z + z).material;
    uint16_t mask[CHUNK_SIZE * CHUNK_SIZE];
    for (int axis = 0; axis < 3; axis++)
        for (int sign = -1; sign <= 1; sign += 2)
            for (int slice = 0; slice < CHUNK_SIZE; slice++) {
                int ua = (axis + 1) % 3, va = (axis + 2) % 3;
                bool fine_boundary =
                    detail_neighbor(w, chunk, axis, sign) >= 0 && slice == (sign > 0 ? CHUNK_SIZE - 1 : 0);
                for (int v = 0; v < CHUNK_SIZE; v++)
                    for (int u = 0; u < CHUNK_SIZE; u++) {
                        int p[3] = {0};
                        p[axis] = slice;
                        p[ua] = u;
                        p[va] = v;
                        uint16_t material = halo[halo_index(p[0], p[1], p[2])];
                        p[axis] += sign;
                        uint16_t adjacent = halo[halo_index(p[0], p[1], p[2])];
                        bool visible = solid[material] && !solid[adjacent] && !fine_boundary;
                        mask[v * CHUNK_SIZE + u] = visible ? material : 0;
                        m->unit_faces += visible;
                    }
                for (int v = 0; v < CHUNK_SIZE; v++)
                    for (int u = 0; u < CHUNK_SIZE;) {
                        uint16_t material = mask[v * CHUNK_SIZE + u];
                        if (!material) {
                            u++;
                            continue;
                        }
                        int width = 1;
                        while (u + width < CHUNK_SIZE && mask[v * CHUNK_SIZE + u + width] == material)
                            width++;
                        int height = 1;
                        for (; v + height < CHUNK_SIZE; height++) {
                            bool same = true;
                            for (int i = 0; i < width; i++)
                                if (mask[(v + height) * CHUNK_SIZE + u + i] != material) {
                                    same = false;
                                    break;
                                }
                            if (!same)
                                break;
                        }
                        if (!emit_quad(m, axis, sign, slice + (sign > 0), u, v, width, height, material))
                            return false;
                        for (int j = 0; j < height; j++)
                            memset(&mask[(v + j) * CHUNK_SIZE + u], 0, (size_t)width * sizeof(mask[0]));
                        u += width;
                    }
                if (fine_boundary) {
                    FaceContext face = {.mesh = m,
                                        .world = w,
                                        .axis = axis,
                                        .sign = sign,
                                        .plane = (sign > 0 ? DETAIL_SIZE_MM : 0)};
                    for (int v = 0; v < CHUNK_SIZE; v++)
                        for (int u = 0; u < CHUNK_SIZE; u++) {
                            int p[3] = {0};
                            p[axis] = slice;
                            p[ua] = u;
                            p[va] = v;
                            face.material = halo[halo_index(p[0], p[1], p[2])];
                            if (!solid[face.material])
                                continue;
                            MmRect rect = {{u * VOXEL_SIZE_MM, v * VOXEL_SIZE_MM},
                                           {(u + 1) * VOXEL_SIZE_MM, (v + 1) * VOXEL_SIZE_MM}};
                            if (!detail_empty_plane(w->details[detail_neighbor(w, chunk, axis, sign)], axis,
                                                    sign > 0 ? 0 : DETAIL_SIZE_MM - 1, rect, emit_patch,
                                                    &face))
                                return false;
                        }
                }
            }
    return true;
}
bool mesh_build_chunk_lod(Mesh *m, const World *w, int chunk, int lod) {
    if (chunk < 0 || chunk >= CHUNK_COUNT || lod < 0 || lod > 4)
        return false;
    if (lod == 0)
        return mesh_build_chunk(m, w, chunk);
    int step = 125 << (lod - 1), n = 2000 / step;
    if ((w->pending[chunk] && chunk / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z) == 3) ||
        (w->details[chunk] && w->details[chunk]->nodes == 1 &&
         !material_get(w->details[chunk]->root->value.material)->solid) ||
        (!w->pending[chunk] && !w->details[chunk] && !w->chunks[chunk].detail &&
         !material_get(w->chunks[chunk].value.material)->solid)) {
        m->count = 0;
        m->terrain_field = false;
        m->unit_faces = 0;
        m->surface_area_mm2 = 0;
        return true;
    }
    uint16_t grid[18 * 18 * 18], mask[16 * 16];
    int stride = n + 2;
    for (int y = -1; y <= n; y++)
        for (int z = -1; z <= n; z++)
            for (int x = -1; x <= n; x++)
                grid[((y + 1) * stride + z + 1) * stride + x + 1] =
                    world_get_chunk_mm(w, chunk, x * step + step / 2, y * step + step / 2,
                                       z * step + step / 2)
                        .material;
    m->count = 0;
    m->terrain_field = false;
    m->unit_faces = 0;
    m->surface_area_mm2 = 0;
    for (int axis = 0; axis < 3; axis++)
        for (int sign = -1; sign <= 1; sign += 2)
            for (int slice = 0; slice < n; slice++) {
                int ua = (axis + 1) % 3, va = (axis + 2) % 3;
                for (int v = 0; v < n; v++)
                    for (int u = 0; u < n; u++) {
                        int p[3] = {0};
                        p[axis] = slice;
                        p[ua] = u;
                        p[va] = v;
                        uint16_t a = grid[((p[1] + 1) * stride + p[2] + 1) * stride + p[0] + 1];
                        p[axis] += sign;
                        uint16_t b = grid[((p[1] + 1) * stride + p[2] + 1) * stride + p[0] + 1];
                        mask[v * n + u] = material_get(a)->solid && !material_get(b)->solid ? a : 0;
                    }
                for (int v = 0; v < n; v++)
                    for (int u = 0; u < n;) {
                        uint16_t mat = mask[v * n + u];
                        if (!mat) {
                            u++;
                            continue;
                        }
                        int width = 1, height = 1;
                        while (u + width < n && mask[v * n + u + width] == mat)
                            width++;
                        while (v + height < n) {
                            bool same = true;
                            for (int i = 0; i < width; i++)
                                if (mask[(v + height) * n + u + i] != mat) {
                                    same = false;
                                    break;
                                }
                            if (!same)
                                break;
                            height++;
                        }
                        if (!emit_quad_mm(m, axis, sign, (slice + (sign > 0)) * step, u * step, v * step,
                                          width * step, height * step, mat))
                            return false;
                        for (int j = 0; j < height; j++)
                            memset(mask + (v + j) * n + u, 0, (size_t)width * sizeof(*mask));
                        u += width;
                    }
            }
    /* Terrain skirts on vertical chunk faces hide fine/coarse height gaps. */
    for (int axis = 0; axis <= 2; axis += 2)
        for (int sign = -1; sign <= 1; sign += 2)
            for (int i = 0; i < n; i++) {
                int x = axis == 0 ? (sign < 0 ? 0 : n - 1) : i, z = axis == 2 ? (sign < 0 ? 0 : n - 1) : i;
                int y = n - 1;
                while (y >= 0 && !material_get(grid[((y + 1) * stride + z + 1) * stride + x + 1])->solid)
                    y--;
                if (y < 0)
                    continue;
                uint16_t mat = grid[((y + 1) * stride + z + 1) * stride + x + 1];
                int top = (y + 1) * step, low = top - 2 * step;
                if (low < 0)
                    low = 0;
                if (!emit_quad_mm(m, axis, sign, sign > 0 ? 2000 : 0, axis == 0 ? low : i * step,
                                  axis == 0 ? i * step : low, axis == 0 ? top - low : step,
                                  axis == 0 ? step : top - low, mat))
                    return false;
            }
    return true;
}
void mesh_destroy(Mesh *m) {
    free(m->vertices);
    free(m->terrain_colors);
    *m = (Mesh){0};
}
