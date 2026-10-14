#ifndef MESH_H
#define MESH_H
#include "world.h"
typedef struct {
    Vec3 position, normal, color;
} Vertex;
/* RGBA: engine-generated appearance RGB, exact ground material ID in alpha.
   Mips are packed largest first; alpha is read only from level zero. */
#define TERRAIN_FIELD_SIZE 256
#define TERRAIN_FIELD_LEVELS 9
#define TERRAIN_FIELD_BYTES 349524u
typedef struct {
    Vertex *vertices;
    size_t count, capacity;
    uint64_t surface_area_mm2;
    size_t unit_faces; /* Exposed cell faces before merging, for coverage checks and statistics. */
    unsigned char *terrain_colors;
    bool terrain_field;
    float terrain_mapping[4]; /* Cube-face u0, v0, inverse span, face + 8. */
} Mesh;
bool mesh_build_chunk(Mesh *mesh, const World *world, int chunk_index);
void mesh_destroy(Mesh *mesh);
#endif
