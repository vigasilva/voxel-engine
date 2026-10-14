#include "terrain.h"
#include <math.h>
#include <stdlib.h>
/* 32 columns per 2 m chunk: alternating 62/63 mm footprints, 1 mm heights.
   The BSP collapses uniform interiors; millimeter grains only refine the surface. */
#define COLUMNS 32
typedef struct {
    int height[COLUMNS][COLUMNS];
    uint16_t deep[COLUMNS][COLUMNS], middle[COLUMNS][COLUMNS], top[COLUMNS][COLUMNS];
    int y_mm;
} Terrain;
static uint32_t noise(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ (x >> 16);
}
static int column(int mm) {
    return ((mm + 1) * COLUMNS - 1) / DETAIL_SIZE_MM;
}
static uint16_t material_at(const Terrain *t, int x, int z, int y) {
    int h = t->height[z][x];
    if (y >= h)
        return MAT_AIR;
    if (y >= h - 40)
        return t->top[z][x];
    if (y >= h - 750)
        return t->middle[z][x];
    return t->deep[z][x];
}
static bool generate_region(MmBox b, Voxel *value, int *axis, int *cut, void *context) {
    Terrain *t = context;
    int x0 = column(b.lo[0]), x1 = column(b.hi[0] - 1);
    int z0 = column(b.lo[2]), z1 = column(b.hi[2] - 1);
    int y0 = t->y_mm + b.lo[1], y1 = t->y_mm + b.hi[1] - 1;
    uint16_t first = material_at(t, x0, z0, y0);
    bool uniform = true;
    for (int z = z0; z <= z1 && uniform; z++)
        for (int x = x0; x <= x1; x++)
            if (material_at(t, x, z, y0) != first || material_at(t, x, z, y1) != first) {
                uniform = false;
                break;
            }
    if (uniform) {
        *value = (Voxel){first, 0, 293.15f};
        return true;
    }
    if (x0 != x1 || z0 != z1) {
        *axis = x1 - x0 >= z1 - z0 ? 0 : 2;
        int low = *axis == 0 ? x0 : z0, high = *axis == 0 ? x1 : z1;
        *cut = ((low + high) / 2 + 1) * DETAIL_SIZE_MM / COLUMNS;
        return true;
    }
    int h = t->height[z0][x0], boundaries[] = {h - 750, h - 40, h};
    for (int i = 0; i < 3; i++)
        if (boundaries[i] > y0 && boundaries[i] <= y1) {
            *axis = 1;
            *cut = boundaries[i] - t->y_mm;
            return true;
        }
    return false;
}
/* Integer periodic phases keep nearby samples precise at any int64 anchor.
   The height field repeats every 2^24 base cells (2,097.152 km). */
static double wave(uint64_t address, double offset, unsigned harmonic) {
    double phase = ((double)(address & 0xffffffu) + offset) * (6.2831853071795864769 / 16777216.0);
    return phase * harmonic;
}
int terrain_height_mm(const World *w, CellCoord cell, double x_offset, double z_offset) {
    uint64_t x = (uint64_t)cell.x - (uint64_t)w->origin.x;
    uint64_t z = (uint64_t)cell.z - (uint64_t)w->origin.z;
    double phase = (w->seed % 1000) * .01;
    return (int)lround(VOXEL_SIZE_MM * (20 + 6 * sin(wave(x, x_offset, 160211) + phase) +
                                        4 * cos(wave(z, z_offset, 200264) - phase) +
                                        4 * sin(wave(x + z, x_offset + z_offset, 106807))) +
                       5 * sin(wave(x, x_offset, 3471260) + phase) * cos(wave(z, z_offset, 4539292)));
}
static int floor_div(int value, int divisor) {
    int q = value / divisor, r = value % divisor;
    return q - (r < 0);
}
Voxel terrain_voxel(const World *w, CellCoord cell, int x_mm, int y_mm, int z_mm) {
    int64_t dx, dy, dz;
    if (!coord_sub(cell.x, w->origin.x, &dx) || !coord_sub(cell.y, w->origin.y, &dy) ||
        !coord_sub(cell.z, w->origin.z, &dz) || (dx < -WORLD_X - CHUNK_SIZE || dx > WORLD_X + CHUNK_SIZE) ||
        (dy < -WORLD_Y - CHUNK_SIZE || dy > WORLD_Y + CHUNK_SIZE) ||
        (dz < -WORLD_Z - CHUNK_SIZE || dz > WORLD_Z + CHUNK_SIZE))
        return (Voxel){MAT_AIR, 0, 293.15f};
    int gx = (int)dx * 125 + x_mm, gy = (int)dy * 125 + y_mm, gz = (int)dz * 125 + z_mm;
    if (gy >= 4259)
        return (Voxel){MAT_AIR, 0, 293.15f};
    int ix = floor_div(gx * 32, 2000), iz = floor_div(gz * 32, 2000);
    /* Unequal integer footprints: floor(2000*k/32) is the exact boundary. */
    if (gx >= floor_div((ix + 1) * 2000, 32))
        ix++;
    if (gz >= floor_div((iz + 1) * 2000, 32))
        iz++;
    int h = terrain_height_mm(w, w->origin, (ix + .5) * .5, (iz + .5) * .5);
    uint16_t material = MAT_AIR;
    uint32_t n = noise((uint32_t)(floor_div(ix * 2000, 32) / 500) +
                       131u * (uint32_t)(floor_div(iz * 2000, 32) / 500) + w->seed);
    if (gy < h - 750)
        material = n % 53 == 0 ? MAT_IRON : n % 59 == 0 ? MAT_COPPER : MAT_STONE;
    else if (gy < h - 40)
        material = n % 17 == 0 ? MAT_CLAY : MAT_SOIL;
    else if (gy < h)
        material = floor_div(iz * 2000, 32) < 2500 ? MAT_SAND : MAT_GRASS;
    uint32_t grain = noise(w->seed ^ ((uint32_t)ix * 73856093u) ^ ((uint32_t)iz * 19349663u));
    int size = 1 + (int)((grain >> 4) % 4), px = floor_div(ix * 2000, 32) + 12 + (int)((grain >> 8) % 32),
        pz = floor_div(iz * 2000, 32) + 12 + (int)((grain >> 16) % 32);
    int local_h = h % 2000;
    if (grain % 8 == 0 && local_h >= 0 && local_h + 4 <= 2000 && gy >= h && gy < h + size && gx >= px &&
        gx < px + size && gz >= pz && gz < pz + size)
        material = grain % 31 == 0 ? MAT_COPPER : MAT_STONE;
    return (Voxel){material, 0, 293.15f};
}
bool terrain_empty_plane(const World *w, int chunk, int axis, int slice, MmRect rect, DetailRectFn fn,
                         void *ctx) {
    CellCoord origin = world_chunk_origin(w, chunk);
    int ua = (axis + 1) % 3, va = (axis + 2) % 3;
    /* Boundary planes contain no surface grains: generation keeps them inset. */
    for (int v = rect.lo[1]; v < rect.hi[1];) {
        int vh = va == 1 ? rect.hi[1] : ((column(v) + 1) * 2000 / 32);
        if (vh > rect.hi[1])
            vh = rect.hi[1];
        for (int u = rect.lo[0]; u < rect.hi[0];) {
            int uh = ua == 1 ? rect.hi[0] : ((column(u) + 1) * 2000 / 32);
            if (uh > rect.hi[0])
                uh = rect.hi[0];
            int p[3] = {0};
            p[axis] = slice;
            p[ua] = u;
            p[va] = v;
            int ix = column(p[0]), iz = column(p[2]);
            int h = terrain_height_mm(w, origin, (ix + .5) * .5, (iz + .5) * .5) -
                    (int)(origin.y - w->origin.y) * 125;
            MmRect part = {{u, v}, {uh, vh}};
            if (axis == 1) {
                if (slice >= h && !fn(part, ctx))
                    return false;
            } else {
                int dim = ua == 1 ? 0 : 1;
                if (part.lo[dim] < h)
                    part.lo[dim] = h;
                if (part.lo[dim] < part.hi[dim] && !fn(part, ctx))
                    return false;
            }
            u = uh;
        }
        v = vh;
    }
    return true;
}
DetailChunk *terrain_build_chunk(const World *w, int c) {
    uint32_t seed = w->seed;

    CellCoord o = world_chunk_origin(w, c);
    int ox = (int)(o.x - w->origin.x) * VOXEL_SIZE_MM;
    int oz = (int)(o.z - w->origin.z) * VOXEL_SIZE_MM;
    Terrain t = {.y_mm = (int)(o.y - w->origin.y) * VOXEL_SIZE_MM};
    for (int z = 0; z < COLUMNS; z++)
        for (int x = 0; x < COLUMNS; x++) {
            t.height[z][x] = terrain_height_mm(w, o, (x + .5) * .5, (z + .5) * .5);
            uint32_t n = noise((uint32_t)((ox + x * DETAIL_SIZE_MM / COLUMNS) / 500) +
                               131u * (uint32_t)((oz + z * DETAIL_SIZE_MM / COLUMNS) / 500) + seed);
            t.deep[z][x] = n % 53 == 0 ? MAT_IRON : n % 59 == 0 ? MAT_COPPER : MAT_STONE;
            t.middle[z][x] = n % 17 == 0 ? MAT_CLAY : MAT_SOIL;
            t.top[z][x] = oz + z * DETAIL_SIZE_MM / COLUMNS < 2500 ? MAT_SAND : MAT_GRASS;
        }
    DetailChunk *result = detail_build(generate_region, &t);
    if (!result)
        return NULL;
    for (int z = 0; z < COLUMNS; z++)
        for (int x = 0; x < COLUMNS; x++) {
            uint32_t n = noise(seed ^ ((uint32_t)(ox / 2000 * COLUMNS + x) * 73856093u) ^
                               ((uint32_t)(oz / 2000 * COLUMNS + z) * 19349663u));
            int h = t.height[z][x] - t.y_mm;
            /* Keep grains in their surface chunk; empty upper chunks remain one node. */
            if (n % 8 || h < 0 || h + 4 > DETAIL_SIZE_MM)
                continue;
            int size = 1 + (int)((n >> 4) % 4);
            int px = x * DETAIL_SIZE_MM / COLUMNS + 12 + (int)((n >> 8) % 32);
            int pz = z * DETAIL_SIZE_MM / COLUMNS + 12 + (int)((n >> 16) % 32);
            MmBox grain = {{px, h, pz}, {px + size, h + size, pz + size}};
            Voxel v = {n % 31 == 0 ? MAT_COPPER : MAT_STONE, 0, 293.15f};
            if (!detail_paint_build(result, grain, v)) {
                detail_destroy(result);
                return NULL;
            }
        }
    return result;
}
bool world_generate_detailed(World *w, uint32_t seed) {
    World next;
    world_init(&next);
    next.origin = w->origin;
    next.seed = seed;
    for (int c = 0; c < CHUNK_COUNT; c++) {
        next.details[c] = terrain_build_chunk(&next, c);
        if (!next.details[c]) {
            world_destroy(&next);
            return false;
        }
    }
    world_destroy(w);
    *w = next;
    return true;
}
