#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
/* Assertions remain active in Release builds. */
#define CHECK(c)                                                                                             \
    do {                                                                                                     \
        if (!(c)) {                                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                     \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void clear(World *w) {
    CHECK(world_fill(w, (Voxel){MAT_AIR, 0, 0}));
    memset(w->dirty, 0, sizeof(w->dirty));
}
static WorldPosition at(double x, double y, double z) {
    WorldPosition p;
    CHECK(position_from_local(cell_coord(0, 0, 0), dv3(x, y, z), &p));
    return p;
}
static bool world_cells_equal(const World *a, const World *b) {
    for (int y = 0; y < WORLD_Y; y++)
        for (int z = 0; z < WORLD_Z; z++)
            for (int x = 0; x < WORLD_X; x++)
                if (!voxel_equal(world_get(a, a->origin.x + x, a->origin.y + y, a->origin.z + z),
                                 world_get(b, b->origin.x + x, b->origin.y + y, b->origin.z + z)))
                    return false;
    return true;
}
static void test_mesh_and_dirty(World *w) {
    clear(w);
    CHECK(world_set(w, 15, 15, 15, (Voxel){MAT_STONE, 0, 300}));
    CHECK(w->dirty[world_chunk_index(0, 0, 0)]);
    CHECK(w->dirty[world_chunk_index(1, 0, 0)]);
    CHECK(w->dirty[world_chunk_index(0, 1, 0)]);
    CHECK(w->dirty[world_chunk_index(0, 0, 1)]);
    int dirty = 0;
    for (int i = 0; i < CHUNK_COUNT; i++)
        dirty += w->dirty[i];
    CHECK(dirty == 4);
    Mesh m = {0};
    CHECK(mesh_build_chunk(&m, w, 0));
    CHECK(m.count == 36);
    CHECK(world_set(w, 16, 15, 15, (Voxel){MAT_COPPER, 0, 300}));
    CHECK(mesh_build_chunk(&m, w, 0));
    CHECK(m.count == 30);
    CHECK(mesh_build_chunk(&m, w, 1));
    CHECK(m.count == 30);
    CHECK(!mesh_build_chunk(&m, w, -1));
    CHECK(!mesh_build_chunk(&m, w, CHUNK_COUNT));
    CHECK(!world_set(w, -1, 0, 0, (Voxel){MAT_STONE, 0, 300}));
    CHECK(!world_set(w, 0, WORLD_Y, 0, (Voxel){MAT_STONE, 0, 300}));
    CHECK(!world_set(w, 0, 0, 0, (Voxel){MAT_COUNT, 0, 300}));
    CHECK(!world_set(w, 0, 0, 0, (Voxel){MAT_STONE, 0, NAN}));
    mesh_destroy(&m);
}
static void test_raycast(World *w) {
    clear(w);
    CHECK(world_set(w, 4, 5, 6, (Voxel){MAT_STONE, 0, 300}));
    RayHit h = world_raycast(w, at(4.5f, 5.5f, 10), v3(0, 0, -1), 10);
    CHECK(h.hit && h.x == 4 && h.y == 5 && h.z == 6 && h.nz == 1);
    CHECK(fabs(h.distance - 3) < .0001f);
    h = world_raycast(w, at(-10, 5.5f, 6.5f), v3(1, 0, 0), 30);
    CHECK(h.hit && h.nx == -1 && fabs(h.distance - 14) < .0001f);
    h = world_raycast(w, at(WORLD_X, 5.5f, 6.5f), v3(-1, 0, 0), WORLD_X);
    CHECK(h.hit && h.nx == 1);
    CHECK(!world_raycast(w, at(-1, 5, 6), v3(0, 1, 0), 100).hit);
    CHECK(!world_raycast(w, at(4.5f, 5.5f, 10), v3(0, 0, -1), 2).hit);
    CHECK(!world_raycast(w, at(4, 5, 6), v3(0, 0, 0), 10).hit);
    CHECK(!world_raycast(w, (WorldPosition){{0, 0, 0}, {NAN, 0, 0}}, v3(1, 0, 0), 10).hit);
    h = world_raycast(w, at(4.5f, 5.5f, 6.5f), v3(1, 0, 0), 10);
    CHECK(h.hit && h.distance == 0 && h.nx == 0);
    clear(w);
    CHECK(world_set(w, 0, 0, 0, (Voxel){MAT_STONE, 0, 300}));
    h = world_raycast(w, at(-2, .5f, .5f), v3(1, 0, 0), 10);
    CHECK(h.hit && h.nx == -1 && h.distance == 2);
    h = world_raycast(w, at(1, .5f, .5f), v3(-1, 0, 0), 10);
    CHECK(h.hit && h.nx == 1 && h.distance == 0);
}
static void test_generation_and_save(World *w) {
    World other;
    CHECK(world_init(&other));
    CHECK(world_generate(w, 42));
    CHECK(world_generate(&other, 42));
    CHECK(world_cells_equal(w, &other));
    CHECK(world_generate(&other, 43));
    CHECK(!world_cells_equal(w, &other));
    CHECK(world_set(w, 30, 25, 30, (Voxel){MAT_IRON, 0, 700}));
    char error[128];
    const char *path = "core-test-world.vxw";
    CHECK(world_save(w, path, error, sizeof(error)));
    CHECK(world_load(&other, path, error, sizeof(error)));
    CHECK(other.seed == 42);
    CHECK(world_cells_equal(w, &other));
    for (int i = 0; i < CHUNK_COUNT; i++)
        CHECK(other.dirty[i]);
    FILE *f = fopen(path, "r+b");
    CHECK(f);
    CHECK(fseek(f, 44, SEEK_SET) == 0);
    int byte = fgetc(f);
    CHECK(byte != EOF);
    CHECK(fseek(f, -1, SEEK_CUR) == 0);
    CHECK(fputc(byte ^ 0xff, f) != EOF);
    CHECK(fclose(f) == 0);
    void *before = other.chunks[0].detail;
    CHECK(!world_load(&other, path, error, sizeof(error)));
    CHECK(other.chunks[0].detail == before);
    CHECK(world_cells_equal(w, &other));
    f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite("VXW1", 1, 4, f) == 4);
    CHECK(fclose(f) == 0);
    CHECK(!world_load(&other, path, error, sizeof(error)));
    CHECK(other.chunks[0].detail == before);
    CHECK(remove(path) == 0);
    world_destroy(&other);
}
/* Construct an old 250 mm file independently of world_save, then check spatial/material preservation. */
static void fixture_put32(unsigned char *p, uint32_t value) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}
static void test_v1_import(World *w, bool legacy) {
    uint32_t sx = legacy ? 64 : WORLD_X, sy = legacy ? 32 : WORLD_Y, sz = legacy ? 64 : WORLD_Z;
    int factor = legacy ? 2 : 1;
    const size_t old_count = (size_t)sx * sy * sz;
    unsigned char *payload = calloc(old_count, 8);
    CHECK(payload);
    float temperature = 350;
    uint32_t temperature_bits;
    memcpy(&temperature_bits, &temperature, 4);
    for (size_t i = 0; i < old_count; i++)
        fixture_put32(payload + i * 8 + 4, temperature_bits);
    /* Include an interior cell and the last cell, exercising coordinate order and upper boundaries. */
    size_t cells[] = {((size_t)5 * sz + 7) * sx + 3, old_count - 1};
    for (size_t i = 0; i < 2; i++)
        fixture_put32(payload + cells[i] * 8, MAT_COPPER);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < old_count * 8; i++)
        hash = (hash ^ payload[i]) * 16777619u;
    unsigned char header[32] = {0};
    memcpy(header, "VXW1", 4);
    fixture_put32(header + 4, 1);
    fixture_put32(header + 8, sx);
    fixture_put32(header + 12, sy);
    fixture_put32(header + 16, sz);
    fixture_put32(header + 20, 77);
    fixture_put32(header + 24, hash);
    fixture_put32(header + 28, legacy ? 250 : VOXEL_SIZE_MM);
    const char *path = "core-test-legacy.vxw";
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(header, 1, sizeof(header), f) == sizeof(header));
    CHECK(fwrite(payload, 8, old_count, f) == old_count);
    CHECK(fclose(f) == 0);
    free(payload);
    char error[128];
    CHECK(world_load(w, path, error, sizeof(error)));
    CHECK(w->seed == 77);
    size_t solid_count = 0;
    for (int y = 0; y < WORLD_Y; y++)
        for (int z = 0; z < WORLD_Z; z++)
            for (int x = 0; x < WORLD_X; x++) {
                Voxel v = world_get(w, x, y, z);
                CHECK(v.temperature_k == 350);
                solid_count += v.material == MAT_COPPER;
            }
    CHECK(solid_count == (size_t)2 * factor * factor * factor);
    for (int dy = 0; dy < factor; dy++)
        for (int dz = 0; dz < factor; dz++)
            for (int dx = 0; dx < factor; dx++) {
                CHECK(world_get(w, 3 * factor + dx, 5 * factor + dy, 7 * factor + dz).material == MAT_COPPER);
                CHECK(world_get(w, WORLD_X - factor + dx, WORLD_Y - factor + dy, WORLD_Z - factor + dz)
                          .material == MAT_COPPER);
            }
    CHECK(world_get(w, 4 * factor, 5 * factor, 7 * factor).material == MAT_AIR);
    float refined_mass = factor * factor * factor * material_get(MAT_COPPER)->density_kg_m3 *
                         VOXEL_SIZE_METERS * VOXEL_SIZE_METERS * VOXEL_SIZE_METERS;
    CHECK(fabsf(refined_mass - (legacy ? 140 : 17.5f)) < .0001f);
    for (int i = 0; i < CHUNK_COUNT; i++)
        CHECK(w->dirty[i]);
    /* Unsupported scale is rejected transactionally even with an otherwise valid payload. */
    f = fopen(path, "r+b");
    CHECK(f);
    fixture_put32(header, 200);
    CHECK(fseek(f, 28, SEEK_SET) == 0);
    CHECK(fwrite(header, 1, 4, f) == 4);
    CHECK(fclose(f) == 0);
    void *before = w->chunks[0].detail;
    CHECK(!world_load(w, path, error, sizeof(error)));
    CHECK(w->chunks[0].detail == before && w->seed == 77);
    CHECK(remove(path) == 0);
}
static void test_sparse_storage(World *w) {
    Voxel air = {MAT_AIR, 0, 293.15f};
    CHECK(world_fill(w, air));
    CHECK(w->allocated_bytes == 0 && world_storage_bytes(w) < 8192);
    memset(w->dirty, 0, sizeof(w->dirty));
    CHECK(world_set(w, 1, 2, 3, (Voxel){MAT_AIR, 0, 400}));
    CHECK(w->allocated_bytes > 0 && w->allocated_bytes < 1024);
    for (int i = 0; i < CHUNK_COUNT; i++)
        CHECK(!w->dirty[i]);
    CHECK(world_get(w, 1, 2, 3).temperature_k == 400);
    CHECK(world_get(w, 2, 2, 3).temperature_k == 293.15f);
    CHECK(world_set(w, 1, 2, 3, air));
    CHECK(w->allocated_bytes == 0);
    Voxel reference[CHUNK_VOLUME];
    for (int i = 0; i < CHUNK_VOLUME; i++)
        reference[i] = air;
    uint32_t random = 19;
    for (int i = 0; i < 9000; i++) {
        random = random * 1664525u + 1013904223u;
        int index = (int)((random >> 8) % CHUNK_VOLUME);
        Voxel v = {(uint16_t)(random % MAT_COUNT), 0, (float)(random % 500 + 200)};
        reference[index] = v;
        CHECK(world_set(w, index % 16, index / 256, (index / 16) % 16, v));
    }
    for (int i = 0; i < CHUNK_VOLUME; i++)
        CHECK(voxel_equal(reference[i], world_get(w, i % 16, i / 256, (i / 16) % 16)));
    for (int i = 0; i < CHUNK_VOLUME; i++)
        CHECK(world_set(w, i % 16, i / 256, (i / 16) % 16, air));
    CHECK(w->allocated_bytes == 0 && w->chunks[0].detail == NULL);
}
static const int normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static void verify_mesh_coverage(const World *w, const Mesh *m) {
    unsigned char covered[CHUNK_VOLUME][6] = {{0}};
    size_t area = 0;
    for (size_t at = 0; at < m->count; at += 6) {
        Vertex a = m->vertices[at], b = m->vertices[at + 1], c = m->vertices[at + 2];
        CHECK(vdot(vcross(vsub(b.position, a.position), vsub(c.position, a.position)), a.normal) > 0);
        int axis = a.normal.x != 0 ? 0 : a.normal.y != 0 ? 1 : 2;
        int sign = axis == 0 ? (int)a.normal.x : axis == 1 ? (int)a.normal.y : (int)a.normal.z;
        int face = axis * 2 + (sign < 0), ua = (axis + 1) % 3, va = (axis + 2) % 3;
        int lo[3] = {CHUNK_SIZE, CHUNK_SIZE, CHUNK_SIZE}, hi[3] = {0};
        for (int i = 0; i < 6; i++) {
            Vec3 p = m->vertices[at + i].position;
            float f[3] = {p.x, p.y, p.z};
            for (int j = 0; j < 3; j++) {
                CHECK(f[j] == floorf(f[j]));
                if ((int)f[j] < lo[j])
                    lo[j] = (int)f[j];
                if ((int)f[j] > hi[j])
                    hi[j] = (int)f[j];
            }
        }
        CHECK(lo[axis] == hi[axis]);
        int point[3] = {lo[0], lo[1], lo[2]};
        point[axis] -= sign > 0;
        uint16_t material = world_get(w, point[0], point[1], point[2]).material;
        for (int v = lo[va]; v < hi[va]; v++)
            for (int u = lo[ua]; u < hi[ua]; u++) {
                point[ua] = u;
                point[va] = v;
                CHECK(point[0] >= 0 && point[0] < 16 && point[1] >= 0 && point[1] < 16 && point[2] >= 0 &&
                      point[2] < 16);
                CHECK(world_get(w, point[0], point[1], point[2]).material == material);
                int i = (point[1] * 16 + point[2]) * 16 + point[0];
                CHECK(covered[i][face] == 0);
                covered[i][face]++;
                area++;
            }
    }
    CHECK(area == m->unit_faces);
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 16; x++) {
                bool solid = material_get(world_get(w, x, y, z).material)->solid;
                for (int f = 0; f < 6; f++) {
                    bool neighbor =
                        material_get(
                            world_get(w, x + normals[f][0], y + normals[f][1], z + normals[f][2]).material)
                            ->solid;
                    CHECK(covered[(y * 16 + z) * 16 + x][f] == (solid && !neighbor));
                }
            }
}
static void test_greedy_meshing(World *w) {
    clear(w);
    Mesh m = {0};
    for (int y = 2; y < 7; y++)
        for (int z = 3; z < 7; z++)
            for (int x = 1; x < 9; x++)
                CHECK(world_set(w, x, y, z, (Voxel){MAT_STONE, 0, 300}));
    CHECK(mesh_build_chunk(&m, w, 0));
    CHECK(m.count == 36);
    CHECK(m.unit_faces == 184);
    verify_mesh_coverage(w, &m);
    clear(w);
    CHECK(world_set(w, 1, 1, 1, (Voxel){MAT_STONE, 0, 300}));
    CHECK(world_set(w, 2, 1, 1, (Voxel){MAT_STONE, 0, 500}));
    CHECK(mesh_build_chunk(&m, w, 0));
    CHECK(m.count == 36);
    verify_mesh_coverage(w, &m);
    CHECK(world_set(w, 2, 1, 1, (Voxel){MAT_COPPER, 0, 500}));
    CHECK(mesh_build_chunk(&m, w, 0));
    CHECK(m.count == 60);
    verify_mesh_coverage(w, &m);
    clear(w);
    uint32_t random = 27;
    for (int y = 0; y < 16; y++)
        for (int z = 0; z < 16; z++)
            for (int x = 0; x < 17; x++) {
                random = random * 1664525u + 1013904223u;
                if (random % 3 != 0)
                    CHECK(world_set(w, x, y, z, (Voxel){(uint16_t)(random % 4 + 1), 0, 300}));
            }
    CHECK(mesh_build_chunk(&m, w, 0));
    verify_mesh_coverage(w, &m);
    mesh_destroy(&m);
}
static void test_large_coordinates(World *w) {
    CHECK(world_fill(w, (Voxel){MAT_AIR, 0, 293.15f}));
    CellCoord origin = cell_coord(INT64_C(1) << 60, -(INT64_C(1) << 60), INT64_C(1) << 54);
    CHECK(world_rebase(w, origin));
    CHECK(world_set(w, origin.x + 4, origin.y + 5, origin.z + 6, (Voxel){MAT_COPPER, 0, 300}));
    CHECK(world_inside(w, origin.x + 4, origin.y + 5, origin.z + 6));
    CHECK(!world_inside(w, INT64_MIN, INT64_MAX, 0));
    WorldPosition p;
    CHECK(position_from_local(origin, dv3(4.5, 5.5, 10), &p));
    RayHit h = world_raycast(w, p, v3(0, 0, -1), 10);
    CHECK(h.hit && h.x == origin.x + 4 && h.y == origin.y + 5 && h.z == origin.z + 6 && h.nz == 1 &&
          h.distance == 3);
    CHECK(position_move_mm(&p, dv3(1, 0, 0)));
    DVec3 relative;
    CHECK(position_relative_cells(p, origin, &relative));
    CHECK(fabs(relative.x - 4.508) < 1e-12);
    CHECK(fabs(((float)relative.x - 4.5f) * VOXEL_SIZE_MM - 1) < .0001);
    p = (WorldPosition){origin, {124.5, 0, 0}};
    CHECK(position_move_mm(&p, dv3(1, 0, 0)));
    CHECK(p.cell.x == origin.x + 1 && p.offset_mm.x == .5);
    CHECK(position_move_mm(&p, dv3(-1, 0, 0)));
    CHECK(p.cell.x == origin.x && p.offset_mm.x == 124.5);
    CHECK(position_from_local(origin, dv3(3, 5.5, 6.5), &p));
    p.offset_mm.x = 124.5;
    h = world_raycast(w, p, v3(1, 0, 0), 1);
    CHECK(h.hit && h.nx == -1 && fabs(h.distance * VOXEL_SIZE_MM - .5) < 1e-9);
    CHECK(position_move_mm(&p, dv3(1, 0, 0)));
    h = world_raycast(w, p, v3(1, 0, 0), 1);
    CHECK(h.hit && h.distance == 0 && p.cell.x == origin.x + 4);
    int64_t result;
    CHECK(!coord_add(INT64_MAX, 1, &result));
    CHECK(!coord_sub(INT64_MIN, 1, &result));
    CHECK(!coord_sub(INT64_MAX, INT64_MIN, &result));
    CHECK(!world_rebase(w, cell_coord(INT64_MAX, 0, 0)));
    CHECK(w->origin.x == origin.x);
    WorldPosition before = p;
    CHECK(!position_move_mm(&p, dv3(INFINITY, 0, 0)));
    CHECK(p.cell.x == before.cell.x && p.offset_mm.x == before.offset_mm.x);
    char error[128];
    CHECK(world_save(w, "core-test-large.vxw", error, sizeof(error)));
    World other;
    CHECK(world_init(&other));
    CHECK(world_load(&other, "core-test-large.vxw", error, sizeof(error)));
    CHECK(other.origin.x == origin.x && other.origin.y == origin.y && other.origin.z == origin.z);
    CHECK(world_cells_equal(w, &other));
    world_destroy(&other);
    CHECK(remove("core-test-large.vxw") == 0);
    CHECK(world_rebase(w, cell_coord(0, 0, 0)));
}
static void benchmark(World *w) {
    CHECK(world_generate(w, 42));
    Mesh mesh = {0};
    size_t faces = 0, quads = 0;
    clock_t start = clock();
    for (int i = 0; i < CHUNK_COUNT; i++) {
        CHECK(mesh_build_chunk(&mesh, w, i));
        faces += mesh.unit_faces;
        quads += mesh.count / 6;
    }
    double ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    CHECK(quads < faces);
    CHECK(world_storage_bytes(w) < VOXEL_COUNT * sizeof(Voxel));
    char error[128];
    CHECK(world_save(w, "core-test-benchmark.vxw", error, sizeof(error)));
    FILE *f = fopen("core-test-benchmark.vxw", "rb");
    CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long save_size = ftell(f);
    CHECK(save_size > 0);
    CHECK(fclose(f) == 0);
    CHECK(remove("core-test-benchmark.vxw") == 0);
    printf("Seed 42: %zu exposed cell faces -> %zu greedy quads (%zu vertices); storage %zu bytes; save %ld "
           "bytes; meshing %.2f CPU ms\n",
           faces, quads, quads * 6, world_storage_bytes(w), save_size, ms);
    mesh_destroy(&mesh);
}
static void fixture_write(const char *path, const unsigned char *data, size_t size) {
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(data, 1, size, f) == size);
    CHECK(fclose(f) == 0);
}
static void fixture_v2_checksum(unsigned char *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++)
        if (i < 72 || i >= 76)
            hash = (hash ^ data[i]) * 16777619u;
    fixture_put32(data + 72, hash);
}
static void test_v2_validation(World *w) {
    const char *path = "core-test-invalid-tree.vxw";
    char error[128];
    CHECK(world_fill(w, (Voxel){MAT_SOIL, 0, 300}));
    w->seed = 93;
    CHECK(world_save(w, path, error, sizeof(error)));
    size_t size = 80 + CHUNK_COUNT * 9 + 4;
    unsigned char *original = malloc(size), *bad = malloc(size);
    CHECK(original && bad);
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(fread(original, 1, size, f) == size);
    CHECK(fgetc(f) == EOF);
    CHECK(fclose(f) == 0);
    memcpy(bad, original, size);
    memcpy(bad, "VXW2", 4);
    fixture_put32(bad + 4, 2);
    fixture_put32(bad + 64, (uint32_t)(size - 80 - 4));
    fixture_v2_checksum(bad, size - 4);
    fixture_write(path, bad, size - 4);
    CHECK(world_load(w, path, error, sizeof(error)));
    CHECK(w->seed == 93 && !w->detail);
    for (int variant = 0; variant < 5; variant++) {
        memcpy(bad, original, size);
        if (variant == 0)
            bad[80] = 2; /* Unknown node tag. */
        if (variant == 1)
            bad[80] = 1; /* Branch consumes later roots; decoder eventually hits EOF. */
        if (variant == 2)
            fixture_put32(bad + 81, MAT_COUNT);
        if (variant == 3)
            fixture_put32(bad + 81, (uint32_t)MAT_SOIL | (1u << 16));
        if (variant == 4)
            fixture_put32(bad + 85, 0x7fc00000u); /* NaN, with valid integrity checksum. */
        fixture_v2_checksum(bad, size);
        fixture_write(path, bad, size);
        CHECK(!world_load(w, path, error, sizeof(error)));
        CHECK(w->seed == 93 && w->allocated_bytes == 0 && world_get(w, 0, 0, 0).material == MAT_SOIL);
    }
    fixture_write(path, original, size - 1);
    CHECK(!world_load(w, path, error, sizeof(error)));
    /* A redundant branch containing eight equal children is valid and collapses on import. */
    size_t redundant_size = size + 64;
    unsigned char *redundant = malloc(redundant_size);
    CHECK(redundant);
    memcpy(redundant, original, 80);
    redundant[80] = 1;
    for (int i = 0; i < 8; i++)
        memcpy(redundant + 81 + i * 9, original + 80, 9);
    memcpy(redundant + 153, original + 89, size - 89);
    fixture_put32(redundant + 64, (uint32_t)(redundant_size - 80));
    fixture_v2_checksum(redundant, redundant_size);
    fixture_write(path, redundant, redundant_size);
    CHECK(world_load(w, path, error, sizeof(error)));
    CHECK(w->allocated_bytes == 0 && w->seed == 93);
    CHECK(world_get(w, 15, 15, 15).material == MAT_SOIL);
    free(redundant);
    free(original);
    free(bad);
    CHECK(remove(path) == 0);
}
static void test_math(void) {
    Mat4 p = mat_perspective(1, 1, .1f, 100);
    float near_z = -.1f, far_z = -100;
    CHECK(fabsf((p.m[10] * near_z + p.m[14]) / (-near_z)) < .0001f);
    CHECK(fabsf((p.m[10] * far_z + p.m[14]) / (-far_z) - 1) < .0001f);
    Mat4 reverse = mat_perspective_reverse_z(1, 1, .0008f, 600);
    CHECK(fabsf((reverse.m[10] * (-.0008f) + reverse.m[14]) / .0008f - 1) < .0001f);
    CHECK(fabsf((reverse.m[10] * (-600) + reverse.m[14]) / 600) < .0001f);
    Mat4 view = mat_view(v3(1, 2, 3), v3(0, 0, -1));
    CHECK(view.m[12] == -1 && view.m[13] == -2 && view.m[14] == -3);
}
int main(void) {
    CHECK(materials_validate());
    World w;
    CHECK(world_init(&w));
    test_mesh_and_dirty(&w);
    test_raycast(&w);
    test_generation_and_save(&w);
    test_v1_import(&w, true);
    test_v1_import(&w, false);
    test_sparse_storage(&w);
    test_greedy_meshing(&w);
    test_large_coordinates(&w);
    test_v2_validation(&w);
    test_math();
    benchmark(&w);
    world_destroy(&w);
    puts("PASS: materials, chunk seams, dirty propagation, ray traversal, deterministic generation, save "
         "round-trip/corruption, legacy migration, sparse split/coalesce, greedy coverage/material "
         "boundaries, 64-bit/mm precision, Vulkan projection");
    return 0;
}
