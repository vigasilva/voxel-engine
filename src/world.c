#include "world.h"
#include "detail.h"
#include "terrain.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <limits.h>
_Static_assert(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128,
               "Save format requires binary32");
_Static_assert(sizeof(Voxel) == 8, "Voxel encoding and storage accounting require eight-byte cells");
static Voxel air(void) {
    return (Voxel){MAT_AIR, 0, 293.15f};
}
bool voxel_equal(Voxel a, Voxel b) {
    return a.material == b.material && a.flags == b.flags &&
           memcmp(&a.temperature_k, &b.temperature_k, 4) == 0;
}
bool voxel_valid(Voxel v) {
    return v.material < MAT_COUNT && v.flags == 0 && isfinite(v.temperature_k) && v.temperature_k >= 0 &&
           v.temperature_k <= 1000000;
}
static size_t detail_bytes(int size) {
    return size == 2 ? 8 * sizeof(Voxel) : 8 * sizeof(VoxelNode);
}
static void node_free(World *w, VoxelNode *n, int size) {
    if (!n->detail)
        return;
    if (size > 2) {
        VoxelNode *children = n->detail;
        for (int i = 0; i < 8; i++)
            node_free(w, &children[i], size / 2);
    }
    free(n->detail);
    w->allocated_bytes -= detail_bytes(size);
    n->detail = NULL;
}
static bool node_split(World *w, VoxelNode *n, int size) {
    n->detail = malloc(detail_bytes(size));
    if (!n->detail)
        return false;
    w->allocated_bytes += detail_bytes(size);
    if (size == 2) {
        Voxel *cells = n->detail;
        for (int i = 0; i < 8; i++)
            cells[i] = n->value;
    } else {
        VoxelNode *children = n->detail;
        for (int i = 0; i < 8; i++)
            children[i] = (VoxelNode){n->value, NULL};
    }
    return true;
}
static void node_collapse(World *w, VoxelNode *n, int size) {
    if (!n->detail)
        return;
    Voxel value;
    if (size == 2) {
        Voxel *cells = n->detail;
        value = cells[0];
        for (int i = 1; i < 8; i++)
            if (!voxel_equal(value, cells[i]))
                return;
    } else {
        VoxelNode *children = n->detail;
        if (children[0].detail)
            return;
        value = children[0].value;
        for (int i = 1; i < 8; i++)
            if (children[i].detail || !voxel_equal(value, children[i].value))
                return;
    }
    node_free(w, n, size);
    n->value = value;
}
static Voxel node_get(const VoxelNode *n, int size, int x, int y, int z) {
    while (n->detail) {
        int half = size / 2, index = (x >= half) + 2 * (y >= half) + 4 * (z >= half);
        if (size == 2)
            return ((Voxel *)n->detail)[index];
        n = &((VoxelNode *)n->detail)[index];
        x %= half;
        y %= half;
        z %= half;
        size = half;
    }
    return n->value;
}
static bool node_set(World *w, VoxelNode *n, int size, int x, int y, int z, Voxel v) {
    if (!n->detail && voxel_equal(n->value, v))
        return true;
    if (size == 1) {
        n->value = v;
        return true;
    }
    if (!n->detail && !node_split(w, n, size))
        return false;
    int half = size / 2, index = (x >= half) + 2 * (y >= half) + 4 * (z >= half);
    if (size == 2)
        ((Voxel *)n->detail)[index] = v;
    else if (!node_set(w, &((VoxelNode *)n->detail)[index], half, x % half, y % half, z % half, v))
        return false;
    node_collapse(w, n, size);
    return true;
}
static size_t chunk_cell_index(int x, int y, int z) {
    return ((size_t)y * CHUNK_SIZE + z) * CHUNK_SIZE + x;
}
static bool node_build(World *w, VoxelNode *n, const Voxel *cells, int size, int x, int y, int z) {
    Voxel first = cells[chunk_cell_index(x, y, z)];
    bool uniform = true;
    for (int dy = 0; dy < size && uniform; dy++)
        for (int dz = 0; dz < size && uniform; dz++)
            for (int dx = 0; dx < size; dx++)
                if (!voxel_equal(first, cells[chunk_cell_index(x + dx, y + dy, z + dz)])) {
                    uniform = false;
                    break;
                }
    n->value = first;
    if (uniform)
        return true;
    if (!node_split(w, n, size))
        return false;
    if (size == 2) {
        for (int i = 0; i < 8; i++)
            ((Voxel *)n->detail)[i] =
                cells[chunk_cell_index(x + (i & 1), y + ((i >> 1) & 1), z + ((i >> 2) & 1))];
    } else {
        int half = size / 2;
        for (int i = 0; i < 8; i++)
            if (!node_build(w, &((VoxelNode *)n->detail)[i], cells, half, x + (i & 1) * half,
                            y + ((i >> 1) & 1) * half, z + ((i >> 2) & 1) * half))
                return false;
    }
    return true;
}
bool world_init(World *w) {
    memset(w, 0, sizeof(*w));
    for (int i = 0; i < CHUNK_COUNT; i++)
        w->chunks[i].value = air();
    w->detail_chunk = -1;
    w->workshop_chunk = -1;
    world_mark_all_dirty(w);
    return true;
}
void world_destroy(World *w) {
    for (int i = 0; i < CHUNK_COUNT; i++) {
        detail_destroy(w->details[i]);
        w->details[i] = NULL;
    }
    for (int i = 0; i < CHUNK_COUNT; i++)
        node_free(w, &w->chunks[i], CHUNK_SIZE);
    memset(w, 0, sizeof(*w));
}
static bool origin_valid(CellCoord o) {
    /* A one-cell margin permits neighbor access without overflowing a global address. */
    return o.x > INT64_MIN && o.y > INT64_MIN && o.z > INT64_MIN && o.x <= INT64_MAX - WORLD_X - 1 &&
           o.y <= INT64_MAX - WORLD_Y - 1 && o.z <= INT64_MAX - WORLD_Z - 1;
}
bool world_rebase(World *w, CellCoord o) {
    if (!origin_valid(o))
        return false;
    w->origin = o;
    return true;
}
bool world_inside(const World *w, int64_t x, int64_t y, int64_t z) {
    return x >= w->origin.x && x < w->origin.x + WORLD_X && y >= w->origin.y && y < w->origin.y + WORLD_Y &&
           z >= w->origin.z && z < w->origin.z + WORLD_Z;
}
int world_chunk_index(int cx, int cy, int cz) {
    return (cy * WORLD_CHUNKS_Z + cz) * WORLD_CHUNKS_X + cx;
}
CellCoord world_chunk_origin(const World *w, int chunk) {
    int cx = chunk % WORLD_CHUNKS_X, cz = (chunk / WORLD_CHUNKS_X) % WORLD_CHUNKS_Z,
        cy = chunk / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z);
    return cell_coord(w->origin.x + cx * CHUNK_SIZE, w->origin.y + cy * CHUNK_SIZE,
                      w->origin.z + cz * CHUNK_SIZE);
}
Voxel world_get(const World *w, int64_t x, int64_t y, int64_t z) {
    if (!world_inside(w, x, y, z))
        return air();
    int lx = (int)(x - w->origin.x), ly = (int)(y - w->origin.y), lz = (int)(z - w->origin.z);
    int chunk = world_chunk_index(lx / CHUNK_SIZE, ly / CHUNK_SIZE, lz / CHUNK_SIZE);
    if (w->pending[chunk])
        return terrain_voxel(w, world_chunk_origin(w, chunk), (lx % CHUNK_SIZE) * 125 + 62,
                             (ly % CHUNK_SIZE) * 125 + 62, (lz % CHUNK_SIZE) * 125 + 62);
    if (w->details[chunk])
        return detail_get(w->details[chunk], (lx % CHUNK_SIZE) * VOXEL_SIZE_MM + VOXEL_SIZE_MM / 2,
                          (ly % CHUNK_SIZE) * VOXEL_SIZE_MM + VOXEL_SIZE_MM / 2,
                          (lz % CHUNK_SIZE) * VOXEL_SIZE_MM + VOXEL_SIZE_MM / 2);
    return node_get(&w->chunks[chunk], CHUNK_SIZE, lx % CHUNK_SIZE, ly % CHUNK_SIZE, lz % CHUNK_SIZE);
}
Voxel world_get_chunk_mm(const World *w, int chunk, int x, int y, int z) {
    if (chunk < 0 || chunk >= CHUNK_COUNT || x < -2000 || y < -2000 || z < -2000 || x >= 4000 || y >= 4000 ||
        z >= 4000)
        return air();
    CellCoord o = world_chunk_origin(w, chunk);
    if (x < 0 || y < 0 || z < 0 || x >= 2000 || y >= 2000 || z >= 2000) {
        int gx = (chunk % WORLD_CHUNKS_X) * 2000 + x;
        int gy = (chunk / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z)) * 2000 + y;
        int gz = ((chunk / WORLD_CHUNKS_X) % WORLD_CHUNKS_Z) * 2000 + z;
        if (gx < 0 || gy < 0 || gz < 0 || gx >= WORLD_X * 125 || gy >= WORLD_Y * 125 || gz >= WORLD_Z * 125)
            return air();
        int next = world_chunk_index(gx / 2000, gy / 2000, gz / 2000);
        return world_get_chunk_mm(w, next, gx % 2000, gy % 2000, gz % 2000);
    }
    if (w->pending[chunk])
        return terrain_voxel(w, o, x, y, z);
    if (w->details[chunk])
        return detail_get(w->details[chunk], x, y, z);
    return node_get(&w->chunks[chunk], 16, x / 125, y / 125, z / 125);
}
void world_mark_all_dirty(World *w) {
    for (int i = 0; i < CHUNK_COUNT; i++)
        w->dirty[i] = true;
}
static void dirty_cell(World *w, int64_t x, int64_t y, int64_t z) {
    if (world_inside(w, x, y, z))
        w->dirty[world_chunk_index((int)(x - w->origin.x) / CHUNK_SIZE, (int)(y - w->origin.y) / CHUNK_SIZE,
                                   (int)(z - w->origin.z) / CHUNK_SIZE)] = true;
}
bool world_set(World *w, int64_t x, int64_t y, int64_t z, Voxel v) {
    if (!world_inside(w, x, y, z) || !voxel_valid(v))
        return false;
    Voxel previous = world_get(w, x, y, z);
    int lx = (int)(x - w->origin.x), ly = (int)(y - w->origin.y), lz = (int)(z - w->origin.z);
    int chunk = world_chunk_index(lx / CHUNK_SIZE, ly / CHUNK_SIZE, lz / CHUNK_SIZE);
    if (w->pending[chunk] && !world_realize_chunk(w, chunk))
        return false;
    if (w->details[chunk]) {
        MmBox box;
        int local[3] = {lx % CHUNK_SIZE, ly % CHUNK_SIZE, lz % CHUNK_SIZE};
        for (int i = 0; i < 3; i++) {
            box.lo[i] = local[i] * VOXEL_SIZE_MM;
            box.hi[i] = box.lo[i] + VOXEL_SIZE_MM;
        }
        return world_paint_chunk_mm(w, chunk, box, v);
    }
    if (!node_set(w, &w->chunks[chunk], CHUNK_SIZE, lx % CHUNK_SIZE, ly % CHUNK_SIZE, lz % CHUNK_SIZE, v))
        return false;
    w->modified[chunk] = true;
    if (previous.material != v.material || previous.flags != v.flags) {
        dirty_cell(w, x, y, z);
        dirty_cell(w, x + 1, y, z);
        dirty_cell(w, x - 1, y, z);
        dirty_cell(w, x, y + 1, z);
        dirty_cell(w, x, y - 1, z);
        dirty_cell(w, x, y, z + 1);
        dirty_cell(w, x, y, z - 1);
    }
    return true;
}
bool world_fill(World *w, Voxel v) {
    if (!voxel_valid(v))
        return false;
    for (int i = 0; i < CHUNK_COUNT; i++) {
        detail_destroy(w->details[i]);
        w->details[i] = NULL;
    }
    w->detail = NULL;
    w->detail_chunk = -1;
    w->workshop_chunk = -1;
    w->procedural = false;
    memset(w->pending, 0, sizeof(w->pending));
    memset(w->modified, 0, sizeof(w->modified));
    for (int i = 0; i < CHUNK_COUNT; i++) {
        node_free(w, &w->chunks[i], CHUNK_SIZE);
        w->chunks[i].value = v;
    }
    world_mark_all_dirty(w);
    return true;
}
size_t world_storage_bytes(const World *w) {
    size_t bytes = sizeof(*w) + w->allocated_bytes;
    for (int i = 0; i < CHUNK_COUNT; i++)
        bytes += detail_storage_bytes(w->details[i]);
    return bytes;
}
static uint32_t hash(uint32_t v) {
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}
bool world_generate(World *w, uint32_t seed) {
    World fresh;
    world_init(&fresh);
    fresh.origin = w->origin;
    fresh.seed = seed;
    int heights[WORLD_Z][WORLD_X];
    float phase = (float)(seed % 1000) * .01f;
    for (int z = 0; z < WORLD_Z; z++)
        for (int x = 0; x < WORLD_X; x++)
            heights[z][x] = 20 + (int)(6 * sinf(x * .06f + phase) + 4 * cosf(z * .075f - phase) +
                                       4 * sinf((x + z) * .04f));
    Voxel cells[CHUNK_VOLUME];
    for (int c = 0; c < CHUNK_COUNT; c++) {
        CellCoord global = world_chunk_origin(&fresh, c);
        int ox = (int)(global.x - fresh.origin.x), oy = (int)(global.y - fresh.origin.y),
            oz = (int)(global.z - fresh.origin.z);
        for (int y = 0; y < CHUNK_SIZE; y++)
            for (int z = 0; z < CHUNK_SIZE; z++)
                for (int x = 0; x < CHUNK_SIZE; x++) {
                    int gx = ox + x, gy = oy + y, gz = oz + z, h = heights[gz][gx];
                    uint16_t m = MAT_AIR;
                    if (gy < h) {
                        m = gy >= h - 2 ? MAT_GRASS : (gy >= h - 6 ? MAT_SOIL : MAT_STONE);
                        if (gz < 20 && gy >= h - 6)
                            m = MAT_SAND;
                        uint32_t n = hash((uint32_t)(gx / 6) + 131u * (uint32_t)(gz / 6) +
                                          1723u * (uint32_t)(gy / 4) + seed);
                        if (gy < h - 8 && n % 29 == 0)
                            m = MAT_IRON;
                        else if (gy < h - 8 && n % 31 == 0)
                            m = MAT_COPPER;
                        else if (gy >= h - 8 && gy < h - 2 && n % 13 == 0)
                            m = MAT_CLAY;
                    }
                    cells[chunk_cell_index(x, y, z)] = (Voxel){m, 0, 293.15f};
                }
        if (!node_build(&fresh, &fresh.chunks[c], cells, CHUNK_SIZE, 0, 0, 0)) {
            world_destroy(&fresh);
            return false;
        }
    }
    /* The stock showcase fits the original presets; the full catalogue is
       available through the editor without growing this fixed laboratory. */
    for (int m = 1; m <= MAT_ICE; m++) {
        int x = 30 + m * 8, z = 64, y = WORLD_Y - 1;
        while (y > 0 &&
               world_get(&fresh, fresh.origin.x + x, fresh.origin.y + y - 1, fresh.origin.z + z).material ==
                   MAT_AIR)
            y--;
        for (int dz = 0; dz < 2; dz++)
            for (int dx = 0; dx < 2; dx++)
                for (int k = 0; k < 6 && y + k < WORLD_Y; k++)
                    if (!world_set(&fresh, fresh.origin.x + x + dx, fresh.origin.y + y + k,
                                   fresh.origin.z + z + dz, (Voxel){(uint16_t)m, 0, 293.15f})) {
                        world_destroy(&fresh);
                        return false;
                    }
    }
    world_destroy(w);
    *w = fresh;
    return true;
}
static void detail_dirty_neighbors(World *w, int chunk) {
    CellCoord o = world_chunk_origin(w, chunk);
    dirty_cell(w, o.x, o.y, o.z);
    dirty_cell(w, o.x - 1, o.y, o.z);
    dirty_cell(w, o.x + CHUNK_SIZE, o.y, o.z);
    dirty_cell(w, o.x, o.y - 1, o.z);
    dirty_cell(w, o.x, o.y + CHUNK_SIZE, o.z);
    dirty_cell(w, o.x, o.y, o.z - 1);
    dirty_cell(w, o.x, o.y, o.z + CHUNK_SIZE);
}
static void detail_dirty_box(World *w, int chunk, MmBox box) {
    w->dirty[chunk] = true;
    CellCoord o = world_chunk_origin(w, chunk);
    int64_t point[3] = {o.x, o.y, o.z};
    for (int a = 0; a < 3; a++) {
        if (box.lo[a] == 0) {
            point[a]--;
            dirty_cell(w, point[0], point[1], point[2]);
            point[a]++;
        }
        if (box.hi[a] == DETAIL_SIZE_MM) {
            point[a] += CHUNK_SIZE;
            dirty_cell(w, point[0], point[1], point[2]);
            point[a] -= CHUNK_SIZE;
        }
    }
}
bool world_generate_lazy(World *w, uint32_t seed) {
    World next;
    world_init(&next);
    next.origin = w->origin;
    next.seed = seed;
    next.procedural = true;
    memset(next.pending, 1, sizeof(next.pending));
    world_destroy(w);
    *w = next;
    return true;
}
bool world_realize_chunk(World *w, int chunk) {
    if (chunk < 0 || chunk >= CHUNK_COUNT)
        return false;
    if (!w->pending[chunk])
        return true;
    DetailChunk *d = terrain_build_chunk(w, chunk);
    if (!d)
        return false;
    w->details[chunk] = d;
    w->pending[chunk] = false;
    detail_dirty_neighbors(w, chunk);
    return true;
}
void world_release_chunk(World *w, int chunk) {
    if (!w->procedural || chunk < 0 || chunk >= CHUNK_COUNT || w->modified[chunk] ||
        w->workshop_chunk == chunk || !w->details[chunk])
        return;
    detail_destroy(w->details[chunk]);
    w->details[chunk] = NULL;
    w->pending[chunk] = true;
    if (w->detail_chunk == chunk) {
        w->detail = NULL;
        w->detail_chunk = -1;
    }
    detail_dirty_neighbors(w, chunk);
}
bool world_prepare_ray(World *w, WorldPosition origin, Vec3 direction, double maximum) {
    if (!isfinite(maximum) || maximum < 0 || !isfinite(direction.x) || !isfinite(direction.y) ||
        !isfinite(direction.z))
        return false;
    Vec3 d = vnormalize(direction);
    if (vdot(d, d) < .5f)
        return false;
    DVec3 o;
    if (!position_relative_cells(origin, w->origin, &o))
        return false;
    double ray[3] = {d.x, d.y, d.z}, start[3] = {o.x, o.y, o.z};
    typedef struct {
        int chunk;
        double enter;
    } Candidate;
    Candidate candidates[CHUNK_COUNT];
    int count = 0;
    for (int c = 0; c < CHUNK_COUNT; c++)
        if (w->pending[c]) {
            CellCoord p = world_chunk_origin(w, c);
            double low[3] = {(double)(p.x - w->origin.x), (double)(p.y - w->origin.y),
                             (double)(p.z - w->origin.z)};
            double enter = 0, leave = maximum;
            for (int a = 0; a < 3; a++) {
                if (fabs(ray[a]) < 1e-8) {
                    if (start[a] < low[a] || start[a] >= low[a] + 16)
                        leave = -1;
                } else {
                    double t0 = (low[a] - start[a]) / ray[a], t1 = (low[a] + 16 - start[a]) / ray[a];
                    if (t0 > t1) {
                        double t = t0;
                        t0 = t1;
                        t1 = t;
                    }
                    enter = fmax(enter, t0);
                    leave = fmin(leave, t1);
                }
            }
            if (enter <= leave) {
                candidates[count++] = (Candidate){c, enter};
            }
        }
    for (int i = 1; i < count; i++) {
        Candidate item = candidates[i];
        int j = i;
        while (j > 0 && candidates[j - 1].enter > item.enter) {
            candidates[j] = candidates[j - 1];
            j--;
        }
        candidates[j] = item;
    }
    for (int i = 0; i < count; i++) {
        RayHit hit = world_raycast(w, origin, direction, maximum);
        if (hit.hit && hit.fine && hit.distance < candidates[i].enter)
            break;
        if (!world_realize_chunk(w, candidates[i].chunk))
            return false;
    }
    return true;
}
static bool copy_coarse(DetailChunk *d, const VoxelNode *n, int size, int x, int y, int z) {
    if (!n->detail) {
        MmBox b = {{x * VOXEL_SIZE_MM, y * VOXEL_SIZE_MM, z * VOXEL_SIZE_MM},
                   {(x + size) * VOXEL_SIZE_MM, (y + size) * VOXEL_SIZE_MM, (z + size) * VOXEL_SIZE_MM}};
        return detail_paint_build(d, b, n->value);
    }
    int half = size / 2;
    for (int i = 0; i < 8; i++) {
        int cx = x + (i & 1) * half, cy = y + ((i >> 1) & 1) * half, cz = z + ((i >> 2) & 1) * half;
        if (size == 2) {
            VoxelNode child = {((Voxel *)n->detail)[i], NULL};
            if (!copy_coarse(d, &child, 1, cx, cy, cz))
                return false;
        } else if (!copy_coarse(d, &((VoxelNode *)n->detail)[i], half, cx, cy, cz))
            return false;
    }
    return true;
}
bool world_enable_detail(World *w, int chunk) {
    if (chunk < 0 || chunk >= CHUNK_COUNT)
        return false;
    if (w->pending[chunk] && !world_realize_chunk(w, chunk))
        return false;
    if (w->details[chunk]) {
        w->detail = w->details[chunk];
        w->detail_chunk = chunk;
        return true;
    }
    DetailChunk *d = detail_create(w->chunks[chunk].value);
    if (!d)
        return false;
    if (!copy_coarse(d, &w->chunks[chunk], CHUNK_SIZE, 0, 0, 0)) {
        detail_destroy(d);
        return false;
    }
    node_free(w, &w->chunks[chunk], CHUNK_SIZE);
    w->chunks[chunk].value = air();
    w->details[chunk] = d;
    w->detail = d;
    w->detail_chunk = chunk;
    detail_dirty_neighbors(w, chunk);
    return true;
}
size_t world_detail_count(const World *w) {
    size_t count = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        count += w->details[i] != NULL;
    return count;
}
size_t world_detail_nodes(const World *w) {
    size_t count = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (w->details[i])
            count += w->details[i]->nodes;
    return count;
}
bool world_paint_chunk_mm(World *w, int chunk, MmBox box, Voxel value) {
    if (chunk < 0 || chunk >= CHUNK_COUNT || !voxel_valid(value))
        return false;
    int base[3] = {chunk % WORLD_CHUNKS_X, chunk / (WORLD_CHUNKS_X * WORLD_CHUNKS_Z),
                   (chunk / WORLD_CHUNKS_X) % WORLD_CHUNKS_Z};
    int sizes[3] = {WORLD_CHUNKS_X, WORLD_CHUNKS_Y, WORLD_CHUNKS_Z}, lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        if (box.lo[a] < -VOXEL_SIZE_MM || box.hi[a] > DETAIL_SIZE_MM + VOXEL_SIZE_MM ||
            box.lo[a] >= box.hi[a])
            return false;
        lo[a] = base[a] + (box.lo[a] < 0 ? -1 : box.lo[a] / DETAIL_SIZE_MM);
        hi[a] = base[a] + (box.hi[a] - 1 < 0 ? -1 : (box.hi[a] - 1) / DETAIL_SIZE_MM);
        if (lo[a] < 0 || hi[a] >= sizes[a])
            return false;
    }
    DetailChunk *next[27] = {0};
    int indices[27], count = 0;
    MmBox boxes[27];
    for (int y = lo[1]; y <= hi[1]; y++)
        for (int z = lo[2]; z <= hi[2]; z++)
            for (int x = lo[0]; x <= hi[0]; x++) {
                int index = world_chunk_index(x, y, z), coord[3] = {x, y, z};
                indices[count] = index;
                next[count] = w->pending[index]   ? terrain_build_chunk(w, index)
                              : w->details[index] ? detail_clone(w->details[index])
                                                  : detail_create(w->chunks[index].value);
                if (!next[count])
                    goto failed;
                if (!w->pending[index] && !w->details[index] &&
                    !copy_coarse(next[count], &w->chunks[index], CHUNK_SIZE, 0, 0, 0))
                    goto failed;
                MmBox local;
                for (int a = 0; a < 3; a++) {
                    int offset = (coord[a] - base[a]) * DETAIL_SIZE_MM;
                    local.lo[a] = box.lo[a] - offset;
                    if (local.lo[a] < 0)
                        local.lo[a] = 0;
                    local.hi[a] = box.hi[a] - offset;
                    if (local.hi[a] > DETAIL_SIZE_MM)
                        local.hi[a] = DETAIL_SIZE_MM;
                }
                boxes[count] = local;
                if (!detail_paint_build(next[count], local, value))
                    goto failed;
                count++;
            }
    for (int i = 0; i < count; i++) {
        int index = indices[i];
        detail_destroy(w->details[index]);
        node_free(w, &w->chunks[index], CHUNK_SIZE);
        w->chunks[index].value = air();
        w->details[index] = next[i];
        w->pending[index] = false;
        w->modified[index] = true;
        if (index == w->detail_chunk)
            w->detail = next[i];
        detail_dirty_box(w, index, boxes[i]);
    }
    return true;
failed:
    for (int i = 0; i < 27; i++)
        detail_destroy(next[i]);
    return false;
}
bool world_paint_mm(World *w, MmBox box, Voxel value) {
    if (!w->detail || !detail_paint(w->detail, box, value))
        return false;
    w->modified[w->detail_chunk] = true;
    detail_dirty_box(w, w->detail_chunk, box);
    return true;
}
bool world_brush_box(RayHit hit, int edge_mm, bool place, MmBox *out) {
    if (!hit.hit || !hit.fine || edge_mm < 1 || edge_mm > VOXEL_SIZE_MM)
        return false;
    int normal[3] = {hit.nx, hit.ny, hit.nz};
    MmBox b;
    for (int a = 0; a < 3; a++) {
        if (normal[a] > 0) {
            b.hi[a] = hit.mm[a] + 1 + (place ? edge_mm : 0);
            b.lo[a] = b.hi[a] - edge_mm;
        } else if (normal[a] < 0) {
            b.lo[a] = hit.mm[a] - (place ? edge_mm : 0);
            b.hi[a] = b.lo[a] + edge_mm;
        } else {
            b.lo[a] = hit.mm[a] - edge_mm / 2;
            b.hi[a] = b.lo[a] + edge_mm;
        }

        if (b.lo[a] >= b.hi[a])
            return false;
    }
    if (!(hit.nx || hit.ny || hit.nz))
        return false;
    *out = b;
    return true;
}
bool world_create_workshop(World *w) {
    if (w->workshop_chunk >= 0) {
        w->detail_chunk = w->workshop_chunk;
        w->detail = w->details[w->detail_chunk];
        return true;
    }
    int preferred = world_chunk_index(4, 2, 4), chosen = -1;
    if (!world_realize_chunk(w, preferred))
        return false;
    if (!w->chunks[preferred].detail && w->chunks[preferred].value.material == MAT_AIR &&
        (!w->details[preferred] ||
         (w->details[preferred]->nodes == 1 && w->details[preferred]->root->value.material == MAT_AIR)))
        chosen = preferred;
    else
        for (int i = CHUNK_COUNT - 1; i >= 0; i--) {
            if (!world_realize_chunk(w, i))
                return false;
            if (!w->chunks[i].detail && w->chunks[i].value.material == MAT_AIR &&
                (!w->details[i] ||
                 (w->details[i]->nodes == 1 && w->details[i]->root->value.material == MAT_AIR))) {
                chosen = i;
                break;
            }
        }
    if (chosen < 0)
        return false;
    DetailChunk *d = detail_create(w->chunks[chosen].value);
    if (!d)
        return false;
    bool ok = detail_paint(d, (MmBox){{750, 750, 750}, {875, 875, 875}}, (Voxel){MAT_IRON, 0, 293.15f}) &&
              detail_paint(d, (MmBox){{810, 780, 850}, {820, 790, 875}}, (Voxel){MAT_COPPER, 0, 293.15f}) &&
              detail_paint(d, (MmBox){{750, 814, 874}, {875, 815, 875}}, (Voxel){MAT_COPPER, 0, 293.15f}) &&
              detail_paint(d, (MmBox){{750, 812, 874}, {875, 813, 875}}, air());
    if (!ok) {
        detail_destroy(d);
        return false;
    }
    detail_destroy(w->details[chosen]);
    w->details[chosen] = d;
    w->pending[chosen] = false;
    w->modified[chosen] = true;
    w->detail = d;
    w->detail_chunk = w->workshop_chunk = chosen;
    w->chunks[chosen].value = air();
    detail_dirty_neighbors(w, chosen);
    return true;
}
Voxel world_hit_voxel(const World *w, RayHit hit) {
    if (!hit.hit)
        return air();
    return hit.fine ? detail_get(w->details[hit.chunk], hit.mm[0], hit.mm[1], hit.mm[2])
                    : world_get(w, hit.x, hit.y, hit.z);
}
RayHit world_raycast(const World *w, WorldPosition origin, Vec3 direction, double max_distance) {
    RayHit miss = {0};
    DVec3 relative;
    if (!position_relative_cells(origin, w->origin, &relative) || !isfinite(direction.x) ||
        !isfinite(direction.y) || !isfinite(direction.z) || !isfinite(max_distance) || max_distance < 0)
        return miss;
    Vec3 d = vnormalize(direction);
    if (vdot(d, d) < .5f)
        return miss;
    RayHit best = {0};
    double o[3] = {relative.x, relative.y, relative.z}, v[3] = {d.x, d.y, d.z};
    int sizes[3] = {WORLD_X, WORLD_Y, WORLD_Z};
    double enter = 0, leave = max_distance;
    int normal[3] = {0};
    for (int a = 0; a < 3; a++) {
        if (!isfinite(o[a]))
            return miss;
        if (fabs(v[a]) < 1e-8) {
            if (o[a] < 0 || o[a] >= sizes[a])
                return miss;
            continue;
        }
        double t0 = -o[a] / v[a], t1 = ((double)sizes[a] - o[a]) / v[a];
        int sign = -1;
        if (t0 > t1) {
            double t = t0;
            t0 = t1;
            t1 = t;
            sign = 1;
        }
        if (t0 > enter) {
            enter = t0;
            normal[0] = normal[1] = normal[2] = 0;
            normal[a] = sign;
        }
        if (t1 < leave)
            leave = t1;
        if (enter > leave)
            return miss;
    }
    if (leave < 0)
        return miss;
    int cell[3], step[3];
    double next[3], delta[3];
    for (int a = 0; a < 3; a++) {
        double p = o[a] + v[a] * enter;
        if (!isfinite(p) || p < -.0001 || p > sizes[a] + .0001)
            return miss;
        p = fmax(0, fmin(sizes[a], p));
        cell[a] = (int)floor(p);
        if (cell[a] == sizes[a] && v[a] < 0)
            cell[a]--;
        step[a] = v[a] > 0 ? 1 : -1;
        delta[a] = fabs(v[a]) < 1e-8 ? DBL_MAX : fabs(1 / v[a]);
        next[a] = fabs(v[a]) < 1e-8 ? DBL_MAX : ((double)(cell[a] + (step[a] > 0)) - o[a]) / v[a];
    }
    double distance = enter;
    int checked_chunk = -1;
    while (cell[0] >= 0 && cell[0] < WORLD_X && cell[1] >= 0 && cell[1] < WORLD_Y && cell[2] >= 0 &&
           cell[2] < WORLD_Z && distance <= leave) {
        int64_t x = w->origin.x + cell[0], y = w->origin.y + cell[1], z = w->origin.z + cell[2];
        if (best.hit && distance > best.distance)
            return best;
        int chunk = world_chunk_index(cell[0] / CHUNK_SIZE, cell[1] / CHUNK_SIZE, cell[2] / CHUNK_SIZE);
        if (w->details[chunk] && checked_chunk != chunk) {
            checked_chunk = chunk;
            CellCoord chunk_origin = world_chunk_origin(w, chunk);
            DVec3 local;
            if (position_relative_cells(origin, chunk_origin, &local)) {
                DetailHit h = detail_raycast(
                    w->details[chunk],
                    dv3(local.x * VOXEL_SIZE_MM, local.y * VOXEL_SIZE_MM, local.z * VOXEL_SIZE_MM), d,
                    max_distance * VOXEL_SIZE_MM);
                if (h.hit)
                    best = (RayHit){.hit = true,
                                    .x = chunk_origin.x + h.mm[0] / VOXEL_SIZE_MM,
                                    .y = chunk_origin.y + h.mm[1] / VOXEL_SIZE_MM,
                                    .z = chunk_origin.z + h.mm[2] / VOXEL_SIZE_MM,
                                    .nx = h.normal[0],
                                    .ny = h.normal[1],
                                    .nz = h.normal[2],
                                    .distance = h.distance_mm / VOXEL_SIZE_MM,
                                    .fine = true,
                                    .chunk = chunk,
                                    .mm = {h.mm[0], h.mm[1], h.mm[2]}};
            }
        }
        if (!w->details[chunk] && material_get(world_get(w, x, y, z).material)->solid) {
            if (best.hit && best.distance <= distance)
                return best;
            return (RayHit){.hit = true,
                            .x = x,
                            .y = y,
                            .z = z,
                            .nx = normal[0],
                            .ny = normal[1],
                            .nz = normal[2],
                            .distance = distance};
        }
        int a = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
        distance = next[a];
        next[a] += delta[a];
        cell[a] += step[a];
        normal[0] = normal[1] = normal[2] = 0;
        normal[a] = -step[a];
    }
    return best;
}
/* V3: one region tree; V4: multiple chunk-indexed region trees and a workshop marker. */
#define SAVE_HEADER_SIZE 80
#define MAX_TREE_BYTES ((size_t)CHUNK_COUNT * (9 * CHUNK_VOLUME + (CHUNK_VOLUME - 1) / 7))
static void put32(unsigned char *p, uint32_t n) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void put64(unsigned char *p, uint64_t n) {
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(n >> (i * 8));
}
static uint64_t get64(const unsigned char *p) {
    uint64_t n = 0;
    for (int i = 0; i < 8; i++)
        n |= (uint64_t)p[i] << (i * 8);
    return n;
}
static int64_t get_signed64(const unsigned char *p) {
    uint64_t n = get64(p);
    return n <= INT64_MAX ? (int64_t)n : -1 - (int64_t)(UINT64_MAX - n);
}
static uint32_t hash_bytes(uint32_t h, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
static uint32_t save_checksum(const unsigned char *h, const unsigned char *data, size_t n) {
    uint32_t hash = hash_bytes(2166136261u, h, 72);
    hash = hash_bytes(hash, h + 76, 4);
    return hash_bytes(hash, data, n);
}
static void encode_voxel(unsigned char *p, Voxel v) {
    uint32_t bits;
    memcpy(&bits, &v.temperature_k, 4);
    put32(p, (uint32_t)v.material | ((uint32_t)v.flags << 16));
    put32(p + 4, bits);
}
static bool decode_voxel(const unsigned char *p, Voxel *v) {
    uint32_t packed = get32(p), bits = get32(p + 4);
    *v = (Voxel){(uint16_t)packed, (uint16_t)(packed >> 16), 0};
    memcpy(&v->temperature_k, &bits, 4);
    return voxel_valid(*v);
}
static size_t encoded_size(const VoxelNode *n, int size) {
    if (!n->detail)
        return 9;
    if (size == 2)
        return 73;
    size_t bytes = 1;
    for (int i = 0; i < 8; i++)
        bytes += encoded_size(&((VoxelNode *)n->detail)[i], size / 2);
    return bytes;
}
static void encode_node(const VoxelNode *n, int size, unsigned char *data, size_t *at) {
    if (!n->detail) {
        data[(*at)++] = 0;
        encode_voxel(data + *at, n->value);
        *at += 8;
        return;
    }
    data[(*at)++] = 1;
    for (int i = 0; i < 8; i++) {
        if (size == 2) {
            data[(*at)++] = 0;
            encode_voxel(data + *at, ((Voxel *)n->detail)[i]);
            *at += 8;
        } else
            encode_node(&((VoxelNode *)n->detail)[i], size / 2, data, at);
    }
}
static bool decode_node(World *w, VoxelNode *n, int size, const unsigned char *data, size_t bytes,
                        size_t *at) {
    if (*at >= bytes)
        return false;
    unsigned char tag = data[(*at)++];
    if (tag == 0) {
        if (bytes - *at < 8 || !decode_voxel(data + *at, &n->value))
            return false;
        *at += 8;
        return true;
    }
    if (tag != 1 || size == 1 || !node_split(w, n, size))
        return false;
    for (int i = 0; i < 8; i++) {
        if (size == 2) {
            if (bytes - *at < 9 || data[(*at)++] != 0 || !decode_voxel(data + *at, &((Voxel *)n->detail)[i]))
                return false;
            *at += 8;
        } else if (!decode_node(w, &((VoxelNode *)n->detail)[i], size / 2, data, bytes, at))
            return false;
    }
    node_collapse(w, n, size);
    return true;
}
static bool fail(char *e, size_t n, const char *s) {
    if (n)
        snprintf(e, n, "%s", s);
    return false;
}
bool world_save(const World *w, const char *path, char *error, size_t error_size) {
    size_t count = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (w->details[i] && (!w->procedural || w->modified[i]))
            count++;
    int version = w->procedural ? 5 : count > 1 ? 4 : 3;
    bool multi = version >= 4;
    size_t bytes = (multi ? 8 : 4) + (version == 5 ? 68 : 0);
    int single = -1;
    for (int i = 0; i < CHUNK_COUNT; i++)
        if (w->details[i] && (!w->procedural || w->modified[i])) {
            bytes += detail_encoded_size(w->details[i]) + (multi ? 4 : 0);
            single = i;
        }
    for (int i = 0; i < CHUNK_COUNT; i++)
        bytes += encoded_size(&w->chunks[i], CHUNK_SIZE);
    unsigned char *data = malloc(bytes);
    if (!data)
        return fail(error, error_size, "Out of memory");
    size_t at = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        encode_node(&w->chunks[i], CHUNK_SIZE, data, &at);
    if (version == 5) {
        put32(data + at, 1);
        at += 4;
        memset(data + at, 0, 64);
        for (int i = 0; i < CHUNK_COUNT; i++) {
            if (!w->modified[i])
                data[at + i / 8] |= (unsigned char)(1u << (i % 8));
            if (w->modified[i])
                data[at + 32 + i / 8] |= (unsigned char)(1u << (i % 8));
        }
        at += 64;
    }
    if (multi) {
        put32(data + at, (uint32_t)count);
        at += 4;
        put32(data + at, w->workshop_chunk >= 0 ? (uint32_t)w->workshop_chunk : UINT32_MAX);
        at += 4;
        for (int i = 0; i < CHUNK_COUNT; i++)
            if (w->details[i] && (!w->procedural || w->modified[i])) {
                put32(data + at, (uint32_t)i);
                at += 4;
                detail_encode(w->details[i], data, &at);
            }
    } else {
        put32(data + at, single >= 0 ? (uint32_t)single : UINT32_MAX);
        at += 4;
        if (single >= 0)
            detail_encode(w->details[single], data, &at);
    }
    unsigned char h[SAVE_HEADER_SIZE] = {0};
    memcpy(h, version == 5 ? "VXW5" : multi ? "VXW4" : "VXW3", 4);
    put32(h + 4, (uint32_t)version);
    put64(h + 8, WORLD_X);
    put64(h + 16, WORLD_Y);
    put64(h + 24, WORLD_Z);
    put32(h + 32, w->seed);
    put32(h + 36, VOXEL_SIZE_MM);
    put64(h + 40, (uint64_t)w->origin.x);
    put64(h + 48, (uint64_t)w->origin.y);
    put64(h + 56, (uint64_t)w->origin.z);
    put64(h + 64, bytes);
    put32(h + 72, save_checksum(h, data, bytes));
    size_t len = strlen(path) + 5;
    char *tmp = malloc(len);
    if (!tmp) {
        free(data);
        return fail(error, error_size, "Out of memory");
    }
    snprintf(tmp, len, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = false;
    if (f) {
        ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) && fwrite(data, 1, bytes, f) == bytes;
        if (fclose(f) != 0)
            ok = false;
    }
    if (ok)
        ok = rename(tmp, path) == 0;
    if (!ok)
        remove(tmp);
    free(tmp);
    free(data);
    return ok ? true : fail(error, error_size, "Could not write save file");
}
static bool load_v1(World *fresh, FILE *f, unsigned char *h) {
    if (fread(h + 8, 1, 24, f) != 24)
        return false;
    uint32_t sx = get32(h + 8), sy = get32(h + 12), sz = get32(h + 16), mm = get32(h + 28);
    bool native = sx == WORLD_X && sy == WORLD_Y && sz == WORLD_Z && mm == VOXEL_SIZE_MM;
    bool legacy = sx == 64 && sy == 32 && sz == 64 && mm == 250 && WORLD_X == 128 && WORLD_Y == 64 &&
                  WORLD_Z == 128 && VOXEL_SIZE_MM == 125;
    if (!native && !legacy)
        return false;
    size_t bytes = (size_t)sx * sy * sz * 8;
    unsigned char *data = malloc(bytes);
    if (!data)
        return false;
    bool ok = fread(data, 1, bytes, f) == bytes && fgetc(f) == EOF && !ferror(f) &&
              hash_bytes(2166136261u, data, bytes) == get32(h + 24);
    Voxel cells[CHUNK_VOLUME];
    int factor = legacy ? 2 : 1;
    for (int c = 0; ok && c < CHUNK_COUNT; c++) {
        CellCoord o = world_chunk_origin(fresh, c);
        for (int y = 0; y < CHUNK_SIZE && ok; y++)
            for (int z = 0; z < CHUNK_SIZE && ok; z++)
                for (int x = 0; x < CHUNK_SIZE; x++) {
                    size_t gx = (size_t)(o.x + x) / factor, gy = (size_t)(o.y + y) / factor,
                           gz = (size_t)(o.z + z) / factor;
                    size_t index = (gy * sz + gz) * sx + gx;
                    if (!decode_voxel(data + index * 8, &cells[chunk_cell_index(x, y, z)])) {
                        ok = false;
                        break;
                    }
                }
        if (ok)
            ok = node_build(fresh, &fresh->chunks[c], cells, CHUNK_SIZE, 0, 0, 0);
    }
    fresh->seed = get32(h + 20);
    free(data);
    return ok;
}
static bool load_v2(World *fresh, FILE *f, unsigned char *h, int version) {
    if (fread(h + 8, 1, SAVE_HEADER_SIZE - 8, f) != SAVE_HEADER_SIZE - 8)
        return false;
    if (get64(h + 8) != WORLD_X || get64(h + 16) != WORLD_Y || get64(h + 24) != WORLD_Z ||
        get32(h + 36) != VOXEL_SIZE_MM || get32(h + 76) != 0)
        return false;
    CellCoord origin = cell_coord(get_signed64(h + 40), get_signed64(h + 48), get_signed64(h + 56));
    if (!world_rebase(fresh, origin))
        return false;
    uint64_t length = get64(h + 64);
    uint64_t minimum =
        CHUNK_COUNT * 9 + (version >= 3 ? (version >= 4 ? 8 : 4) + (version == 5 ? 68 : 0) : 0);
    uint64_t maximum = MAX_TREE_BYTES + (version >= 3 ? 8 + (version == 5 ? 68 : 0) +
                                                            (uint64_t)(version >= 4 ? CHUNK_COUNT : 1) *
                                                                (4 + (uint64_t)DETAIL_MAX_NODES * 9)
                                                      : 0);
    if (length < minimum || length > maximum)
        return false;
    size_t bytes = (size_t)length;
    unsigned char *data = malloc(bytes);
    if (!data)
        return false;
    bool ok = fread(data, 1, bytes, f) == bytes && fgetc(f) == EOF && !ferror(f) &&
              save_checksum(h, data, bytes) == get32(h + 72);
    size_t at = 0;
    for (int c = 0; ok && c < CHUNK_COUNT; c++)
        ok = decode_node(fresh, &fresh->chunks[c], CHUNK_SIZE, data, bytes, &at);
    if (ok && version == 5) {
        if (bytes - at < 68 || get32(data + at) != 1)
            ok = false;
        else {
            fresh->procedural = true;
            at += 4;
            for (int i = 0; i < CHUNK_COUNT; i++) {
                fresh->pending[i] = (data[at + i / 8] >> (i % 8)) & 1;
                fresh->modified[i] = (data[at + 32 + i / 8] >> (i % 8)) & 1;
                if (fresh->pending[i] == fresh->modified[i] || fresh->chunks[i].detail ||
                    fresh->chunks[i].value.material != MAT_AIR)
                    ok = false;
            }
            at += 64;
        }
    }
    if (ok && version >= 3) {
        uint32_t count = 1, workshop = UINT32_MAX;
        if (version >= 4) {
            if (bytes - at < 8)
                ok = false;
            else {
                count = get32(data + at);
                workshop = get32(data + at + 4);
                at += 8;
                if (count > CHUNK_COUNT || (workshop != UINT32_MAX && workshop >= CHUNK_COUNT))
                    ok = false;
            }
        }
        for (uint32_t i = 0; ok && i < count; i++) {
            if (bytes - at < 4) {
                ok = false;
                break;
            }
            uint32_t chunk = get32(data + at);
            at += 4;
            if (version == 3 && chunk == UINT32_MAX)
                break;
            if (chunk >= CHUNK_COUNT || fresh->pending[chunk] || fresh->details[chunk] ||
                fresh->chunks[chunk].detail || fresh->chunks[chunk].value.material != MAT_AIR) {
                ok = false;
                break;
            }
            fresh->modified[chunk] = true;
            fresh->details[chunk] = detail_decode(data, bytes, &at);
            if (!fresh->details[chunk]) {
                ok = false;
                break;
            }
            if (version == 3)
                workshop = chunk;
        }
        if (ok && workshop != UINT32_MAX) {
            if (!fresh->details[workshop])
                ok = false;
            else {
                fresh->workshop_chunk = fresh->detail_chunk = (int)workshop;
                fresh->detail = fresh->details[workshop];
            }
        }
    }
    if (ok && version == 5)
        for (int i = 0; i < CHUNK_COUNT; i++)
            if (fresh->modified[i] && !fresh->details[i])
                ok = false;
    ok = ok && at == bytes;
    fresh->seed = get32(h + 32);
    free(data);
    return ok;
}
bool world_load(World *w, const char *path, char *error, size_t error_size) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return fail(error, error_size, "Could not open save file");
    unsigned char h[SAVE_HEADER_SIZE] = {0};
    World fresh;
    world_init(&fresh);
    bool ok = fread(h, 1, 8, f) == 8;
    if (ok) {
        if (!memcmp(h, "VXW5", 4) && get32(h + 4) == 5)
            ok = load_v2(&fresh, f, h, 5);
        else if (!memcmp(h, "VXW4", 4) && get32(h + 4) == 4)
            ok = load_v2(&fresh, f, h, 4);
        else if (!memcmp(h, "VXW3", 4) && get32(h + 4) == 3)
            ok = load_v2(&fresh, f, h, 3);
        else if (!memcmp(h, "VXW2", 4) && get32(h + 4) == 2)
            ok = load_v2(&fresh, f, h, 2);
        else if (!memcmp(h, "VXW1", 4) && get32(h + 4) == 1)
            ok = load_v1(&fresh, f, h);
        else
            ok = false;
    }
    fclose(f);
    if (!ok) {
        world_destroy(&fresh);
        return fail(error, error_size, "Corrupt or unsupported save file");
    }
    world_destroy(w);
    *w = fresh;
    world_mark_all_dirty(w);
    return true;
}
