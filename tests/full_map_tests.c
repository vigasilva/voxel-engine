#include "detail.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        if (!(e)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static Voxel air(void) {
    return (Voxel){MAT_AIR, 0, 293.15f};
}
static Voxel stone(void) {
    return (Voxel){MAT_STONE, 0, 300};
}
static unsigned char *file_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    CHECK(length >= 0);
    *size = (size_t)length;
    rewind(f);
    unsigned char *data = malloc(*size);
    CHECK(data);
    CHECK(fread(data, 1, *size, f) == *size);
    CHECK(fclose(f) == 0);
    return data;
}
static void put32(unsigned char *p, uint32_t v) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(v >> (8 * i));
}
static void test_seams(void) {
    World w, other;
    world_init(&w);
    world_init(&other);
    int c = world_chunk_index(1, 1, 1), n = world_chunk_index(2, 1, 1);
    CHECK(world_enable_detail(&w, c));
    CHECK(world_enable_detail(&w, n));
    CHECK(world_enable_detail(&w, c) && w.detail == w.details[c]);
    memset(w.dirty, 0, sizeof(w.dirty));
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{50, 50, 50}, {51, 51, 51}}, stone()));
    for (int i = 0; i < CHUNK_COUNT; i++)
        CHECK(w.dirty[i] == (i == c));
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{50, 50, 50}, {51, 51, 51}}, air()));
    memset(w.dirty, 0, sizeof(w.dirty));
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{1998, 100, 100}, {2002, 104, 104}}, stone()));
    CHECK(detail_material_volume(w.details[c], MAT_STONE) == 32);
    CHECK(detail_material_volume(w.details[n], MAT_STONE) == 32);
    Mesh m = {0};
    uint64_t area = 0;
    CHECK(mesh_build_chunk(&m, &w, c));
    area += m.surface_area_mm2;
    CHECK(mesh_build_chunk(&m, &w, n));
    area += m.surface_area_mm2;
    CHECK(area == 96); /* 4x4x4 cube, shared faces are completely hidden. */
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{1999, 101, 101}, {2000, 102, 102}}, air()));
    CHECK(mesh_build_chunk(&m, &w, c));
    area = m.surface_area_mm2;
    CHECK(mesh_build_chunk(&m, &w, n));
    area += m.surface_area_mm2;
    CHECK(area == 102); /* Interior 1 mm cavity spanning a fine/fine interface. */
    WorldPosition p;
    CellCoord o = world_chunk_origin(&w, c);
    CHECK(position_from_local(o, dv3(1980.0 / 125, 101.5 / 125, 101.5 / 125), &p));
    RayHit h = world_raycast(&w, p, v3(1, 0, 0), 1);
    CHECK(h.hit && h.fine && h.chunk == c && h.mm[0] == 1998);
    h.mm[0] = 1999;
    h.nx = 1;
    MmBox brush;
    CHECK(world_brush_box(h, 1, true, &brush));
    CHECK(brush.lo[0] == 2000 && brush.hi[0] == 2001);
    CHECK(world_paint_chunk_mm(&w, c, brush, (Voxel){MAT_COPPER, 0, 490}));
    CHECK(detail_get(w.details[n], 0, 101, 101).material == MAT_COPPER);
    CHECK(w.dirty[c] && w.dirty[n]);
    char error[128];
    CHECK(world_save(&w, "full-map-seam.vxw", error, sizeof(error)));
    CHECK(world_load(&other, "full-map-seam.vxw", error, sizeof(error)));
    CHECK(world_detail_count(&other) == 2 && other.details[c] && other.details[n]);
    CHECK(detail_get(other.details[n], 0, 101, 101).temperature_k == 490);
    size_t length;
    unsigned char *data = file_read("full-map-seam.vxw", &length);
    CHECK(!memcmp(data, "VXW4", 4));
    /* Duplicate chunk index with a valid integrity checksum must still be rejected. */
    size_t second_index = 80 + CHUNK_COUNT * 9 + 8 + 4 + detail_encoded_size(w.details[c]);
    put32(data + second_index, (uint32_t)c);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; i++)
        if (i < 72 || i >= 76)
            hash = (hash ^ data[i]) * 16777619u;
    put32(data + 72, hash);
    FILE *f = fopen("full-map-seam.vxw", "wb");
    CHECK(f);
    CHECK(fwrite(data, 1, length, f) == length);
    CHECK(fclose(f) == 0);
    DetailChunk *before = other.details[n];
    CHECK(!world_load(&other, "full-map-seam.vxw", error, sizeof(error)) && other.details[n] == before);
    free(data);
    CHECK(remove("full-map-seam.vxw") == 0);
    /* A brush over a three-axis intersection updates all eight chunks atomically. */
    CHECK(world_fill(&w, air()));
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{1999, 1999, 1999}, {2001, 2001, 2001}}, stone()));
    CHECK(world_detail_count(&w) == 8);
    uint64_t volume = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        volume += detail_material_volume(w.details[i], MAT_STONE);
    CHECK(volume == 8);
    before = w.details[c];
    CHECK(!world_paint_chunk_mm(&w, 0, (MmBox){{-1, 0, 0}, {1, 1, 1}}, stone()));
    CHECK(w.details[c] == before);
    mesh_destroy(&m);
    world_destroy(&w);
    world_destroy(&other);
}
typedef struct {
    uint64_t volume;
    size_t grains;
} RegionStats;
static bool sum_leaf(MmBox b, Voxel v, void *ctx) {
    CHECK(voxel_valid(v));
    RegionStats *stats = ctx;
    if (material_get(v.material)->solid && b.hi[0] - b.lo[0] == 1 && b.hi[1] - b.lo[1] == 1 &&
        b.hi[2] - b.lo[2] == 1)
        stats->grains++;
    stats->volume += (uint64_t)(b.hi[0] - b.lo[0]) * (b.hi[1] - b.lo[1]) * (b.hi[2] - b.lo[2]);
    return true;
}
static void test_generated_map(void) {
    World w, other;
    world_init(&w);
    world_init(&other);
    CHECK(world_rebase(&w, cell_coord(INT64_C(1) << 60, -(INT64_C(1) << 60), INT64_C(1) << 54)));
    clock_t start = clock();
    CHECK(world_generate_detailed(&w, 42));
    double generation_ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    CHECK(world_detail_count(&w) == CHUNK_COUNT && !w.detail);
    CHECK(world_storage_bytes(&w) < 64 * 1024 * 1024);
    size_t empty = 0;
    RegionStats stats = {0};
    for (int c = 0; c < CHUNK_COUNT; c++) {
        CHECK(w.details[c] && w.details[c]->nodes <= DETAIL_MAX_NODES);
        CHECK(w.chunks[c].detail == NULL && w.chunks[c].value.material == MAT_AIR);
        CHECK(detail_visit(w.details[c], sum_leaf, &stats));
        empty += w.details[c]->nodes == 1 && w.details[c]->root->value.material == MAT_AIR;
    }
    CHECK(stats.volume == UINT64_C(2048000000000) && stats.grains > 100 && empty > 64);
    /* Independent mm sampling checks downward picking in every horizontal map chunk. */
    for (int z = 0; z < WORLD_CHUNKS_Z; z++)
        for (int x = 0; x < WORLD_CHUNKS_X; x++) {
            int top = -1;
            for (int y = WORLD_Y * VOXEL_SIZE_MM - 1; y >= 0; y--) {
                int c = world_chunk_index(x, y / DETAIL_SIZE_MM, z);
                if (material_get(detail_get(w.details[c], 345, y % DETAIL_SIZE_MM, 567).material)->solid) {
                    top = y;
                    break;
                }
            }
            CHECK(top >= 0);
            WorldPosition p;
            CHECK(position_from_local(
                w.origin, dv3(x * CHUNK_SIZE + 345.5 / 125, WORLD_Y + 1, z * CHUNK_SIZE + 567.5 / 125), &p));
            RayHit h = world_raycast(&w, p, v3(0, -1, 0), WORLD_Y + 2);
            CHECK(h.hit && h.fine && h.ny == 1);
            CHECK(h.chunk == world_chunk_index(x, top / DETAIL_SIZE_MM, z) &&
                  h.mm[1] == top % DETAIL_SIZE_MM);
            CHECK(fabs(h.distance - (WORLD_Y + 1 - (top + 1) / 125.0)) < 1e-6);
        }
    Mesh m = {0};
    size_t vertices = 0;
    uint64_t area = 0;
    start = clock();
    for (int c = 0; c < CHUNK_COUNT; c++) {
        CHECK(mesh_build_chunk(&m, &w, c));
        vertices += m.count;
        area += m.surface_area_mm2;
    }
    double mesh_ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    CHECK(vertices && vertices < 2000000 && area > 512000000);
    char error[128];
    CHECK(world_save(&w, "full-map.vxw", error, sizeof(error)));
    size_t bytes;
    unsigned char *original = file_read("full-map.vxw", &bytes);
    CHECK(world_load(&other, "full-map.vxw", error, sizeof(error)));
    CHECK(world_detail_count(&other) == CHUNK_COUNT && other.origin.x == w.origin.x);
    CHECK(world_detail_nodes(&other) == world_detail_nodes(&w));
    CHECK(world_save(&other, "full-map-copy.vxw", error, sizeof(error)));
    size_t copy_bytes;
    unsigned char *copy = file_read("full-map-copy.vxw", &copy_bytes);
    CHECK(bytes == copy_bytes && !memcmp(original, copy, bytes));
    free(copy);
    CHECK(world_generate_detailed(&other, 42));
    CHECK(world_save(&other, "full-map-copy.vxw", error, sizeof(error)));
    copy = file_read("full-map-copy.vxw", &copy_bytes);
    CHECK(bytes == copy_bytes && !memcmp(original, copy, bytes));
    free(copy);
    CHECK(world_generate_detailed(&other, 43));
    CHECK(world_save(&other, "full-map-copy.vxw", error, sizeof(error)));
    copy = file_read("full-map-copy.vxw", &copy_bytes);
    CHECK(bytes != copy_bytes || memcmp(original, copy, bytes));
    free(copy);
    free(original);
    CHECK(world_create_workshop(&w));
    int workshop = w.workshop_chunk;
    CHECK(workshop >= 0 && world_detail_count(&w) == CHUNK_COUNT);
    CHECK(world_save(&w, "full-map.vxw", error, sizeof(error)));
    CHECK(world_load(&other, "full-map.vxw", error, sizeof(error)));
    CHECK(other.workshop_chunk == workshop && other.detail == other.details[workshop]);
    CHECK(world_create_workshop(&other));
    printf("Full map seed 42: %zu region nodes, %zu requested storage bytes, %zu save bytes, %zu vertices, "
           "%.1f CPU ms generation, %.1f CPU ms meshing\n",
           world_detail_nodes(&w) - w.detail->nodes + 1,
           world_storage_bytes(&w) - detail_storage_bytes(w.detail) + 48, bytes, vertices, generation_ms,
           mesh_ms);
    CHECK(remove("full-map.vxw") == 0 && remove("full-map-copy.vxw") == 0);
    mesh_destroy(&m);
    world_destroy(&w);
    world_destroy(&other);
}
int main(void) {
    test_seams();
    test_generated_map();
    puts(
        "PASS: all 256 refined chunks, generated topology/determinism, exact fine/fine seams, cross-boundary "
        "brushes, eight-chunk edits, huge-address picking, V4 round trips and duplicate-record rejection");
    return 0;
}
