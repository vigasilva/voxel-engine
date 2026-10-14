#include "detail.h"
#include "mesh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(condition)                                                                                     \
    do {                                                                                                     \
        if (!(condition)) {                                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                             \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static Voxel iron(void) {
    return (Voxel){MAT_IRON, 0, 350};
}
static Voxel empty(void) {
    return (Voxel){MAT_AIR, 0, 293.15f};
}
static const int normals[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
static void verify_local_surface(const World *w, const Mesh *mesh) {
    enum { BASE = 96, SIDE = 16 };
    unsigned char covered[SIDE * SIDE * SIDE][6] = {{0}};
    uint64_t total = 0;
    for (size_t at = 0; at < mesh->count; at += 6) {
        Vertex a = mesh->vertices[at], b = mesh->vertices[at + 1], c = mesh->vertices[at + 2];
        CHECK(vdot(vcross(vsub(b.position, a.position), vsub(c.position, a.position)), a.normal) > 0);
        int axis = a.normal.x != 0 ? 0 : a.normal.y != 0 ? 1 : 2;
        int sign = axis == 0 ? (int)a.normal.x : axis == 1 ? (int)a.normal.y : (int)a.normal.z;
        int face = axis * 2 + (sign < 0), ua = (axis + 1) % 3, va = (axis + 2) % 3;
        int lo[3] = {DETAIL_SIZE_MM, DETAIL_SIZE_MM, DETAIL_SIZE_MM}, hi[3] = {0};
        for (int i = 0; i < 6; i++) {
            Vec3 v = mesh->vertices[at + i].position;
            float f[3] = {v.x, v.y, v.z};
            for (int j = 0; j < 3; j++) {
                double mm = (double)f[j] * VOXEL_SIZE_MM;
                int n = (int)llround(mm);
                CHECK(fabs(mm - n) < .0002);
                if (n < lo[j])
                    lo[j] = n;
                if (n > hi[j])
                    hi[j] = n;
            }
        }
        CHECK(lo[axis] == hi[axis]);
        int p[3] = {lo[0], lo[1], lo[2]};
        p[axis] -= sign > 0;
        uint16_t material = detail_get(w->detail, p[0], p[1], p[2]).material;
        for (int v = lo[va]; v < hi[va]; v++)
            for (int u = lo[ua]; u < hi[ua]; u++) {
                p[ua] = u;
                p[va] = v;
                int x = p[0] - BASE, y = p[1] - BASE, z = p[2] - BASE;
                CHECK(x >= 0 && x < SIDE && y >= 0 && y < SIDE && z >= 0 && z < SIDE);
                CHECK(detail_get(w->detail, p[0], p[1], p[2]).material == material);
                int i = (y * SIDE + z) * SIDE + x;
                CHECK(!covered[i][face]);
                covered[i][face]++;
                total++;
            }
    }
    CHECK(total == mesh->surface_area_mm2);
    for (int y = 0; y < SIDE; y++)
        for (int z = 0; z < SIDE; z++)
            for (int x = 0; x < SIDE; x++) {
                bool solid =
                    material_get(detail_get(w->detail, BASE + x, BASE + y, BASE + z).material)->solid;
                for (int f = 0; f < 6; f++) {
                    bool adjacent =
                        material_get(detail_get(w->detail, BASE + x + normals[f][0], BASE + y + normals[f][1],
                                                BASE + z + normals[f][2])
                                         .material)
                            ->solid;
                    CHECK(covered[(y * SIDE + z) * SIDE + x][f] == (solid && !adjacent));
                }
            }
}
static WorldPosition local_position(const World *w, double x, double y, double z) {
    WorldPosition p;
    CHECK(position_from_local(world_chunk_origin(w, w->detail_chunk),
                              dv3(x / VOXEL_SIZE_MM, y / VOXEL_SIZE_MM, z / VOXEL_SIZE_MM), &p));
    return p;
}
static void test_carving_and_surface(void) {
    World w;
    CHECK(world_init(&w));
    int chunk = world_chunk_index(2, 1, 2);
    CHECK(world_enable_detail(&w, chunk));
    CHECK(w.detail->nodes == 1 && detail_storage_bytes(w.detail) < 128);
    CHECK(world_paint_mm(&w, (MmBox){{100, 100, 100}, {108, 108, 108}}, iron()));
    size_t nodes = w.detail->nodes;
    CHECK(nodes < 100);
    CHECK(detail_material_volume(w.detail, MAT_IRON) == 512);
    Mesh mesh = {0};
    CHECK(mesh_build_chunk(&mesh, &w, chunk));
    CHECK(mesh.count == 36 && mesh.surface_area_mm2 == 384);
    verify_local_surface(&w, &mesh);
    WorldPosition p = local_position(&w, 104.5, 104.5, 120);
    RayHit h = world_raycast(&w, p, v3(0, 0, -1), 1);
    CHECK(h.hit && h.fine && h.nz == 1 && h.mm[0] == 104 && h.mm[1] == 104 && h.mm[2] == 107);
    CHECK(fabs(h.distance * VOXEL_SIZE_MM - 12) < 1e-8);
    MmBox brush;
    CHECK(world_brush_box(h, 1, false, &brush));
    CHECK(brush.lo[2] == 107 && brush.hi[2] == 108);
    CHECK(world_paint_mm(&w, brush, empty()));
    CHECK(detail_material_volume(w.detail, MAT_IRON) == 511);
    CHECK(mesh_build_chunk(&mesh, &w, chunk));
    CHECK(mesh.surface_area_mm2 == 388);
    verify_local_surface(&w, &mesh);
    h = world_raycast(&w, p, v3(0, 0, -1), 1);
    CHECK(h.hit && h.mm[2] == 106 && fabs(h.distance * VOXEL_SIZE_MM - 13) < 1e-8);
    CHECK(world_brush_box(h, 1, true, &brush));
    CHECK(world_paint_mm(&w, brush, iron()));
    CHECK(w.detail->nodes == nodes);
    CHECK(world_paint_mm(&w, (MmBox){{104, 104, 104}, {105, 105, 105}}, (Voxel){MAT_IRON, 0, 500}));
    CHECK(detail_get(w.detail, 104, 104, 104).temperature_k == 500);
    CHECK(detail_get(w.detail, 105, 104, 104).temperature_k == 350);
    CHECK(detail_material_volume(w.detail, MAT_IRON) == 512);
    CHECK(mesh_build_chunk(&mesh, &w, chunk));
    CHECK(mesh.surface_area_mm2 == 384);
    verify_local_surface(&w, &mesh);
    CHECK(world_paint_mm(&w, (MmBox){{104, 104, 104}, {105, 105, 105}}, iron()));
    CHECK(w.detail->nodes == nodes);
    size_t before = w.detail->nodes;
    CHECK(!world_paint_mm(&w, (MmBox){{-1, 0, 0}, {1, 1, 1}}, iron()));
    CHECK(w.detail->nodes == before);
    CHECK(world_paint_mm(&w, (MmBox){{0, 0, 0}, {2000, 2000, 2000}}, empty()));
    CHECK(w.detail->nodes == 1);
    mesh_destroy(&mesh);
    world_destroy(&w);
}
static void test_random_regions(void) {
    World w;
    CHECK(world_init(&w));
    int chunk = world_chunk_index(2, 1, 2);
    CHECK(world_enable_detail(&w, chunk));
    CHECK(world_paint_mm(&w, (MmBox){{100, 100, 100}, {108, 108, 108}}, iron()));
    Voxel reference[512];
    for (int i = 0; i < 512; i++)
        reference[i] = iron();
    uint32_t random = 71;
    for (int edit = 0; edit < 180; edit++) {
        random = random * 1664525u + 1013904223u;
        int x = (int)(random % 7), y = (int)((random >> 8) % 7), z = (int)((random >> 16) % 7);
        Voxel v = {(uint16_t)(random % 3 == 0   ? MAT_AIR
                              : random % 3 == 1 ? MAT_IRON
                                                : MAT_COPPER),
                   0, (float)(random % 2 ? 350 : 500)};
        CHECK(world_paint_mm(&w, (MmBox){{100 + x, 100 + y, 100 + z}, {102 + x, 102 + y, 102 + z}}, v));
        for (int dy = 0; dy < 2; dy++)
            for (int dz = 0; dz < 2; dz++)
                for (int dx = 0; dx < 2; dx++)
                    reference[((y + dy) * 8 + z + dz) * 8 + x + dx] = v;
    }
    uint64_t iron_count = 0, copper_count = 0;
    for (int y = 0; y < 8; y++)
        for (int z = 0; z < 8; z++)
            for (int x = 0; x < 8; x++) {
                Voxel v = reference[(y * 8 + z) * 8 + x];
                CHECK(voxel_equal(v, detail_get(w.detail, 100 + x, 100 + y, 100 + z)));
                iron_count += v.material == MAT_IRON;
                copper_count += v.material == MAT_COPPER;
            }
    CHECK(detail_material_volume(w.detail, MAT_IRON) == iron_count);
    CHECK(detail_material_volume(w.detail, MAT_COPPER) == copper_count);
    Mesh mesh = {0};
    CHECK(mesh_build_chunk(&mesh, &w, chunk));
    verify_local_surface(&w, &mesh);
    mesh_destroy(&mesh);
    CHECK(world_paint_mm(&w, (MmBox){{0, 0, 0}, {2000, 2000, 2000}}, empty()));
    CHECK(w.detail->nodes == 1);
    world_destroy(&w);
}
static void test_mixed_resolution_seam(void) {
    World w;
    CHECK(world_init(&w));
    CHECK(world_fill(&w, (Voxel){MAT_STONE, 0, 300}));
    int chunk = world_chunk_index(2, 1, 2), left = world_chunk_index(1, 1, 2);
    CHECK(world_enable_detail(&w, chunk));
    memset(w.dirty, 0, sizeof(w.dirty));
    CHECK(world_paint_mm(&w, (MmBox){{0, 62, 62}, {1, 63, 63}}, empty()));
    CHECK(w.dirty[chunk] && w.dirty[left]);
    CHECK(detail_material_volume(w.detail, MAT_STONE) == UINT64_C(8000000000) - 1);
    Mesh a = {0}, b = {0};
    CHECK(mesh_build_chunk(&a, &w, chunk));
    CHECK(mesh_build_chunk(&b, &w, left));
    CHECK(a.surface_area_mm2 == 5 && b.surface_area_mm2 == 1 && b.count == 6);
    CHECK(world_paint_mm(&w, (MmBox){{0, 62, 62}, {1, 63, 63}}, (Voxel){MAT_STONE, 0, 300}));
    CHECK(w.detail->nodes == 1);
    CHECK(mesh_build_chunk(&a, &w, chunk));
    CHECK(mesh_build_chunk(&b, &w, left));
    CHECK(a.count == 0 && b.count == 0);
    mesh_destroy(&a);
    mesh_destroy(&b);
    world_destroy(&w);
}
static void test_promote_and_save(void) {
    World w;
    CHECK(world_init(&w));
    CHECK(world_set(&w, 1, 2, 3, (Voxel){MAT_COPPER, 0, 490}));
    CHECK(world_enable_detail(&w, 0));
    CHECK(detail_get(w.detail, 125, 250, 375).material == MAT_COPPER);
    CHECK(detail_get(w.detail, 249, 374, 499).temperature_k == 490);
    CHECK(detail_get(w.detail, 250, 374, 499).material == MAT_AIR);
    CHECK(detail_material_volume(w.detail, MAT_COPPER) == UINT64_C(1953125));
    CHECK(world_paint_mm(&w, (MmBox){{125, 250, 375}, {126, 251, 376}}, empty()));
    CHECK(detail_material_volume(w.detail, MAT_COPPER) == UINT64_C(1953125) - 1);
    CellCoord big = cell_coord(INT64_C(1) << 60, -(INT64_C(1) << 60), INT64_C(1) << 54);
    CHECK(world_rebase(&w, big));
    WorldPosition p = local_position(&w, 125.5, 250.5, 370);
    RayHit h = world_raycast(&w, p, v3(0, 0, 1), 1);
    CHECK(h.hit && h.fine && h.mm[2] == 376 && fabs(h.distance * VOXEL_SIZE_MM - 6) < 1e-8);
    char error[128];
    CHECK(world_save(&w, "detail-test.vxw", error, sizeof(error)));
    World other;
    CHECK(world_init(&other));
    CHECK(world_load(&other, "detail-test.vxw", error, sizeof(error)));
    CHECK(other.detail && other.detail_chunk == 0 && other.origin.x == big.x && other.origin.y == big.y &&
          other.origin.z == big.z);
    CHECK(detail_material_volume(other.detail, MAT_COPPER) == UINT64_C(1953125) - 1);
    for (int y = 249; y < 253; y++)
        for (int z = 374; z < 379; z++)
            for (int x = 124; x < 128; x++)
                CHECK(voxel_equal(detail_get(w.detail, x, y, z), detail_get(other.detail, x, y, z)));
    h = world_raycast(&other, p, v3(0, 0, 1), 1);
    CHECK(h.hit && h.fine && h.x == big.x + 1 && h.mm[2] == 376);
    FILE *f = fopen("detail-test.vxw", "rb");
    CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long bytes = ftell(f);
    CHECK(fclose(f) == 0);
    CHECK(bytes < 10000);
    CHECK(remove("detail-test.vxw") == 0);
    size_t encoded = detail_encoded_size(w.detail);
    unsigned char *data = malloc(encoded);
    CHECK(data);
    size_t at = 0;
    detail_encode(w.detail, data, &at);
    CHECK(at == encoded);
    data[0] = 4;
    at = 0;
    CHECK(!detail_decode(data, encoded, &at));
    data[0] = 1;
    data[1] = data[2] = 0;
    at = 0;
    CHECK(!detail_decode(data, encoded, &at));
    free(data);
    world_destroy(&other);
    world_destroy(&w);
}
static void benchmark_uniform_chunk(void) {
    World w;
    CHECK(world_init(&w));
    int chunk = world_chunk_index(2, 1, 2);
    CHECK(world_enable_detail(&w, chunk));
    CHECK(world_paint_mm(&w, (MmBox){{0, 0, 0}, {2000, 2000, 2000}}, iron()));
    CHECK(detail_material_volume(w.detail, MAT_IRON) == UINT64_C(8000000000));
    Mesh m = {0};
    clock_t start = clock();
    CHECK(mesh_build_chunk(&m, &w, chunk));
    CHECK(m.count == 36 && m.surface_area_mm2 == UINT64_C(24000000));
    printf("1 mm prototype: 2 m solid cube, %zu nodes, %zu detail bytes, %zu vertices, %.3f CPU ms meshing\n",
           w.detail->nodes, detail_storage_bytes(w.detail), m.count,
           1000.0 * (clock() - start) / CLOCKS_PER_SEC);
    mesh_destroy(&m);
    world_destroy(&w);
}
int main(void) {
    test_carving_and_surface();
    test_random_regions();
    test_mixed_resolution_seam();
    test_promote_and_save();
    benchmark_uniform_chunk();
    puts("PASS: 1 mm carving/filling, exact volume and face coverage, independent temperatures, coalescing, "
         "coarse/fine seams, large-address ray picking, V3 saves, malformed refinement trees");
    return 0;
}
