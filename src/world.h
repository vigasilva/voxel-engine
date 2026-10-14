#ifndef WORLD_H
#define WORLD_H
#include "material.h"
#include "position.h"
#include <stddef.h>
#define CHUNK_SIZE 16
#define CHUNK_VOLUME (CHUNK_SIZE * CHUNK_SIZE * CHUNK_SIZE)
#define WORLD_CHUNKS_X 8
#define WORLD_CHUNKS_Y 4
#define WORLD_CHUNKS_Z 8
#define WORLD_X (WORLD_CHUNKS_X * CHUNK_SIZE)
#define WORLD_Y (WORLD_CHUNKS_Y * CHUNK_SIZE)
#define WORLD_Z (WORLD_CHUNKS_Z * CHUNK_SIZE)
#define CHUNK_COUNT (WORLD_CHUNKS_X * WORLD_CHUNKS_Y * WORLD_CHUNKS_Z)
#define VOXEL_COUNT ((size_t)WORLD_X * WORLD_Y * WORLD_Z)
typedef struct {
    uint16_t material, flags;
    float temperature_k;
} Voxel;
/* Uniform cube, otherwise eight subcubes; size-2 cubes store eight individual voxel values. */
typedef struct VoxelNode {
    Voxel value;
    void *detail;
} VoxelNode;
struct DetailChunk;
typedef struct {
    int lo[3], hi[3];
} MmBox;
typedef struct {
    VoxelNode chunks[CHUNK_COUNT];
    bool dirty[CHUNK_COUNT];
    uint32_t seed;
    CellCoord origin;
    size_t allocated_bytes;
    bool procedural;
    bool pending[CHUNK_COUNT], modified[CHUNK_COUNT];
    struct DetailChunk *details[CHUNK_COUNT];
    /* Active workshop/legacy editing view; ownership lives in details[]. */
    struct DetailChunk *detail;
    int detail_chunk;
    int workshop_chunk;
} World;
typedef struct {
    bool hit;
    int64_t x, y, z;
    int nx, ny, nz;
    double distance;
    bool fine;
    int chunk;
    int mm[3]; /* Integer millimeter cell within the detailed chunk. */
} RayHit;
bool voxel_valid(Voxel value);
bool voxel_equal(Voxel a, Voxel b);
bool world_init(World *w);
void world_destroy(World *w);
bool world_rebase(World *w, CellCoord origin);
bool world_inside(const World *w, int64_t x, int64_t y, int64_t z);
Voxel world_get(const World *w, int64_t x, int64_t y, int64_t z);
bool world_set(World *w, int64_t x, int64_t y, int64_t z, Voxel voxel);
bool world_fill(World *w, Voxel voxel);
bool world_generate(World *w, uint32_t seed);
bool world_generate_detailed(World *w, uint32_t seed);
bool world_generate_lazy(World *w, uint32_t seed);
bool world_realize_chunk(World *w, int chunk);
void world_release_chunk(World *w, int chunk);
bool world_prepare_ray(World *w, WorldPosition origin, Vec3 direction, double max_cells);
Voxel world_get_chunk_mm(const World *w, int chunk, int x, int y, int z);
size_t world_detail_count(const World *w);
size_t world_detail_nodes(const World *w);
int world_chunk_index(int cx, int cy, int cz);
CellCoord world_chunk_origin(const World *w, int chunk);
void world_mark_all_dirty(World *w);
size_t world_storage_bytes(const World *w);
RayHit world_raycast(const World *w, WorldPosition origin, Vec3 direction, double max_distance);
bool world_enable_detail(World *w, int chunk);
bool world_paint_mm(World *w, MmBox box, Voxel value);
/* Box is relative to chunk; may cross into adjacent chunks by at most 125 mm. */
bool world_paint_chunk_mm(World *w, int chunk, MmBox box, Voxel value);
bool world_brush_box(RayHit hit, int edge_mm, bool place, MmBox *out);
bool world_create_workshop(World *w);
Voxel world_hit_voxel(const World *w, RayHit hit);
bool world_save(const World *w, const char *path, char *error, size_t error_size);
bool world_load(World *w, const char *path, char *error, size_t error_size);
#endif
