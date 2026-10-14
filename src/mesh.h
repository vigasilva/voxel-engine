#ifndef MESH_H
#define MESH_H
#include "world.h"
typedef struct {
    Vec3 position, normal, color;
} Vertex;
typedef struct {
    Vertex *vertices;
    size_t count, capacity;
    uint64_t surface_area_mm2;
    size_t unit_faces; /* Exposed cell faces before merging, for coverage checks and statistics. */
} Mesh;
bool mesh_build_chunk(Mesh *mesh, const World *world, int chunk_index);
void mesh_destroy(Mesh *mesh);
#endif
