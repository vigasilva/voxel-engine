#ifndef SCENE_H
#define SCENE_H
#include "mesh.h"
#define SCENE_TILE_LIMIT 1024
#define SCENE_SLOTS (CHUNK_COUNT + SCENE_TILE_LIMIT)
#define SCENE_BUILD_BUDGET 6
typedef struct {
    CellCoord origin;
    int size_cells, lod, target_lod;
    int base_chunk;
    bool visible, reset;
    uint64_t last_visible;
} SceneEntry;
typedef struct {
    SceneEntry entries[SCENE_SLOTS];
    uint64_t frame;
    size_t visible, culled, pending, mesh_bytes;
    size_t lod_counts[5];
    uint32_t seed;
    CellCoord world_origin;
    bool initialized;
    float view_m;
} Scene;
void scene_init(Scene *scene);
bool scene_update(Scene *scene, World *world, WorldPosition camera, Vec3 direction, float aspect,
                  float view_m, bool dynamic);
bool scene_box_visible(WorldPosition camera, Vec3 direction, float aspect, float view_m, CellCoord origin,
                       Vec3 size_cells);
int scene_lod(double distance_m, int previous);
bool mesh_build_chunk_lod(Mesh *mesh, const World *world, int chunk, int lod);
bool mesh_build_terrain_tile(Mesh *mesh, const World *world, CellCoord origin, int size_cells);
#endif
