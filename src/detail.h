#ifndef DETAIL_H
#define DETAIL_H
#include "world.h"
#define DETAIL_SIZE_MM (CHUNK_SIZE * VOXEL_SIZE_MM)
#define DETAIL_MAX_NODES 65535
#define DETAIL_MAX_DEPTH 128
typedef struct DetailNode {
    Voxel value;
    int axis, cut;
    struct DetailNode *child[2];
} DetailNode;
typedef struct DetailChunk {
    DetailNode *root;
    size_t nodes;
    bool unlimited; /* Planet edits grow until allocation fails; laboratory retains its limit. */
} DetailChunk;
typedef struct {
    int lo[2], hi[2];
} MmRect;
typedef struct {
    bool hit;
    int mm[3], normal[3];
    double distance_mm;
    Voxel voxel;
} DetailHit;
typedef bool (*DetailLeafFn)(MmBox bounds, Voxel value, void *context);
typedef bool (*DetailRectFn)(MmRect rect, void *context);
/* Callback returns a uniform value (axis=-1) or an integer split. */
typedef bool (*DetailBuildFn)(MmBox bounds, Voxel *value, int *axis, int *cut, void *context);
DetailChunk *detail_build(DetailBuildFn generator, void *context);
DetailChunk *detail_build_unlimited(DetailBuildFn generator, void *context);
DetailChunk *detail_clone(const DetailChunk *detail);
/* For unpublished builders/clones only: discard the tree if this fails. */
bool detail_paint_build(DetailChunk *detail, MmBox box, Voxel value);
DetailChunk *detail_create(Voxel value);
void detail_destroy(DetailChunk *detail);
Voxel detail_get(const DetailChunk *detail, int x, int y, int z);
bool detail_paint(DetailChunk *detail, MmBox box, Voxel value);
bool detail_visit(const DetailChunk *detail, DetailLeafFn visitor, void *context);
bool detail_empty_plane(const DetailChunk *detail, int axis, int slice, MmRect rect, DetailRectFn visitor,
                        void *context);
DetailHit detail_raycast(const DetailChunk *detail, DVec3 origin_mm, Vec3 direction, double max_mm);
uint64_t detail_material_volume(const DetailChunk *detail, uint16_t material);
size_t detail_storage_bytes(const DetailChunk *detail);
size_t detail_encoded_size(const DetailChunk *detail);
void detail_encode(const DetailChunk *detail, unsigned char *data, size_t *offset);
DetailChunk *detail_decode(const unsigned char *data, size_t length, size_t *offset);
DetailChunk *detail_decode_unlimited(const unsigned char *data, size_t length, size_t *offset);
#endif
