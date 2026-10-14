#include "scene.h"
#include "terrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        if (!(e)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #e);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static uint32_t random_state = 91;
static unsigned random_number(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}
static void test_lazy_data(void) {
    World w, loaded;
    world_init(&w);
    world_init(&loaded);
    CHECK(world_generate_lazy(&w, 42));
    CHECK(w.procedural && world_detail_count(&w) == 0 && world_storage_bytes(&w) < 8192);
    char error[128];
    CHECK(world_save(&w, "stream-test.vxw", error, sizeof(error)));
    FILE *f = fopen("stream-test.vxw", "rb");
    CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    CHECK(ftell(f) == 2460);
    CHECK(fclose(f) == 0);
    CHECK(world_load(&loaded, "stream-test.vxw", error, sizeof(error)) && loaded.procedural &&
          world_detail_count(&loaded) == 0);
    for (int c = 0; c < CHUNK_COUNT; c++) {
        CHECK(world_realize_chunk(&w, c));
        for (int i = 0; i < 120; i++) {
            int x = random_number() % 2000, y = random_number() % 2000, z = random_number() % 2000;
            CHECK(voxel_equal(world_get_chunk_mm(&loaded, c, x, y, z), detail_get(w.details[c], x, y, z)));
        }
        world_release_chunk(&w, c);
        CHECK(w.pending[c] && !w.details[c]);
    }
    /* Procedural plane queries must expose exactly the same mm area as stored BSP queries. */
    int c = world_chunk_index(2, 1, 2);
    CHECK(world_realize_chunk(&w, c));
    Mesh before = {0}, after = {0};
    CHECK(mesh_build_chunk(&before, &w, c));
    const int neighbors[] = {world_chunk_index(1, 1, 2), world_chunk_index(3, 1, 2),
                             world_chunk_index(2, 0, 2), world_chunk_index(2, 2, 2),
                             world_chunk_index(2, 1, 1), world_chunk_index(2, 1, 3)};
    for (int i = 0; i < 6; i++)
        CHECK(world_realize_chunk(&w, neighbors[i]));
    CHECK(mesh_build_chunk(&after, &w, c));
    CHECK(before.surface_area_mm2 == after.surface_area_mm2);
    mesh_destroy(&before);
    mesh_destroy(&after);
    CHECK(world_paint_chunk_mm(&w, c, (MmBox){{100, 1500, 100}, {101, 1501, 101}},
                               (Voxel){MAT_COPPER, 0, 480}));
    DetailChunk *edited = w.details[c];
    world_release_chunk(&w, c);
    CHECK(w.details[c] == edited && w.modified[c]);
    CHECK(world_save(&w, "stream-test.vxw", error, sizeof(error)));
    CHECK(world_load(&loaded, "stream-test.vxw", error, sizeof(error)));
    CHECK(world_detail_count(&loaded) == 1 && loaded.modified[c] && loaded.details[c]);
    CHECK(detail_get(loaded.details[c], 100, 1500, 100).temperature_k == 480);
    CHECK(world_realize_chunk(&loaded, 0));
    CHECK(world_save(&loaded, "stream-copy.vxw", error, sizeof(error)));
    /* Cached unedited trees do not change the saved bytes. */
    FILE *a = fopen("stream-test.vxw", "rb"), *b = fopen("stream-copy.vxw", "rb");
    CHECK(a && b);
    int aa, bb;
    do {
        aa = fgetc(a);
        bb = fgetc(b);
        CHECK(aa == bb);
    } while (aa != EOF);
    CHECK(!ferror(a) && !ferror(b));
    CHECK(fclose(a) == 0 && fclose(b) == 0);
    /* Reject inconsistent procedural metadata, even with a repaired checksum. */
    f = fopen("stream-test.vxw", "rb");
    CHECK(f);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    long length = ftell(f);
    CHECK(length > 0);
    rewind(f);
    unsigned char *data = malloc((size_t)length);
    CHECK(data);
    CHECK(fread(data, 1, (size_t)length, f) == (size_t)length && fclose(f) == 0);
    size_t metadata = 80 + CHUNK_COUNT * 9;
    data[metadata + 4] &= (unsigned char)~1u; /* Chunk 0 is neither pending nor modified. */
    uint32_t checksum = 2166136261u;
    for (size_t i = 0; i < (size_t)length; i++)
        if (i < 72 || i >= 76)
            checksum = (checksum ^ data[i]) * 16777619u;
    for (int i = 0; i < 4; i++)
        data[72 + i] = (unsigned char)(checksum >> (i * 8));
    f = fopen("stream-test.vxw", "wb");
    CHECK(f);
    CHECK(fwrite(data, 1, (size_t)length, f) == (size_t)length && fclose(f) == 0);
    free(data);
    edited = loaded.details[c];
    CHECK(!world_load(&loaded, "stream-test.vxw", error, sizeof(error)) && loaded.details[c] == edited);
    CHECK(remove("stream-test.vxw") == 0 && remove("stream-copy.vxw") == 0);
    world_destroy(&w);
    world_destroy(&loaded);
}
static bool covers(SceneEntry e, WorldPosition p) {
    DVec3 r;
    return position_relative_cells(p, e.origin, &r) && r.x >= 0 && r.z >= 0 && r.x < e.size_cells &&
           r.z < e.size_cells;
}
static void test_scene_lods(void) {
    CHECK(scene_lod(1, -1) == 0 && scene_lod(3, -1) == 1 && scene_lod(20, -1) == 2 &&
          scene_lod(40, -1) == 3 && scene_lod(120, -1) == 4);
    CHECK(scene_lod(2.1, 0) == 0 && scene_lod(1.9, 1) == 1 && scene_lod(1.5, 1) == 0);
    World w;
    world_init(&w);
    CHECK(world_generate_lazy(&w, 42));
    WorldPosition camera;
    CHECK(position_from_local(w.origin, dv3(64, 44, 98), &camera));
    Vec3 direction = v3(0, -.605, -.796);
    CHECK(!scene_box_visible(camera, v3(0, 0, -1), 1.6, 512, cell_coord(64, 44, 140), v3(16, 16, 16)));
    CHECK(scene_box_visible(camera, v3(0, 0, -1), 1.6, 512, cell_coord(64, 44, 40), v3(16, 16, 16)));
    Scene *s = malloc(sizeof(*s));
    CHECK(s);
    scene_init(s);
    CHECK(scene_update(s, &w, camera, direction, 1.6, 512, true));
    CHECK(s->visible > 20 && s->visible < SCENE_SLOTS && world_detail_count(&w) == 0);
    /* Desired terrain tiles never overlap the editable footprint or each other. */
    size_t fine_requests = 0, tiles = 0;
    int minsize = 1024, maxsize = 0;
    for (int i = 0; i < SCENE_SLOTS; i++) {
        SceneEntry e = s->entries[i];
        if (!e.visible)
            continue;
        if (i < CHUNK_COUNT) {
            fine_requests += e.target_lod == 0;
            continue;
        }
        tiles++;
        if (e.size_cells < minsize)
            minsize = e.size_cells;
        if (e.size_cells > maxsize)
            maxsize = e.size_cells;
        CHECK(e.origin.x >= WORLD_X || e.origin.z >= WORLD_Z || e.origin.x + e.size_cells <= 0 ||
              e.origin.z + e.size_cells <= 0);
        for (int j = i + 1; j < SCENE_SLOTS; j++)
            if (s->entries[j].visible) {
                SceneEntry n = s->entries[j];
                CHECK(!(e.origin.x < n.origin.x + n.size_cells && n.origin.x < e.origin.x + e.size_cells &&
                        e.origin.z < n.origin.z + n.size_cells && n.origin.z < e.origin.z + e.size_cells));
            }
    }
    CHECK(fine_requests < CHUNK_COUNT / 4 && tiles > 20 && minsize < maxsize);
    /* A far visible point gets exactly one tile; no chunk-sized world allocation. */
    WorldPosition far;
    CHECK(position_from_local(w.origin, dv3(64, 20, -2000), &far));
    int cover = 0;
    for (int i = CHUNK_COUNT; i < SCENE_SLOTS; i++)
        if (s->entries[i].visible && covers(s->entries[i], far))
            cover++;
    CHECK(cover == 1);
    CHECK(world_prepare_ray(&w, camera, direction, 80));
    RayHit hit = world_raycast(&w, camera, direction, 80);
    CHECK(hit.hit && hit.fine);
    CHECK(scene_update(s, &w, camera, direction, 1.6, 512, true));
    CHECK(s->entries[hit.chunk].target_lod == 0);
    size_t resident = world_detail_count(&w);
    CHECK(resident < 16);
    Mesh m = {0};
    size_t vertices[5];
    for (int lod = 0; lod <= 4; lod++) {
        CHECK(mesh_build_chunk_lod(&m, &w, hit.chunk, lod));
        vertices[lod] = m.count;
    }
    CHECK(vertices[0] > vertices[4] * 4);
    CellCoord tile = cell_coord(256, 0, -256);
    CHECK(mesh_build_terrain_tile(&m, &w, tile, 1024) && m.count == 1920);
    Vec3 p = m.vertices[0].position;
    CHECK(fabsf(p.y * 125 - terrain_height_mm(&w, tile, 0, 0)) < .001f);
    /* Large moves change tile origins, then eventually release old cache entries. */
    CHECK(position_move_mm(&camera, dv3(1000000, 0, 0)));
    CHECK(scene_update(s, &w, camera, direction, 1.6, 512, true));
    CHECK(world_detail_count(&w) < resident);
    for (int frame = 0; frame < 125; frame++)
        CHECK(scene_update(s, &w, camera, v3(1, 0, 0), 1.6, 512, true));
    CHECK(s->visible > 0 && s->visible < SCENE_SLOTS);
    /* int64 anchors retain the same local terrain and do not overflow target offsets. */
    CellCoord big = cell_coord(INT64_C(1) << 60, -(INT64_C(1) << 60), INT64_C(1) << 54);
    CHECK(world_rebase(&w, big));
    CHECK(position_from_local(w.origin, dv3(64, 44, 98), &camera));
    CHECK(scene_update(s, &w, camera, direction, 1.6, 512, true));
    CHECK(terrain_height_mm(&w, big, 3, 4) ==
          terrain_height_mm(&w, cell_coord(big.x + 3, big.y, big.z + 4), 0, 0));
    CHECK(world_prepare_ray(&w, camera, direction, 80));
    hit = world_raycast(&w, camera, direction, 80);
    CHECK(hit.hit && hit.fine);
    printf("Streaming: %zu initial fine chunks along pick ray, LOD mesh vertices %zu/%zu/%zu/%zu/%zu, %zu "
           "visible scene entries\n",
           resident, vertices[0], vertices[1], vertices[2], vertices[3], vertices[4], s->visible);
    /* Endpoint anchors and coarse neighbor queries must stay within signed range. */
    const CellCoord edges[] = {{INT64_MIN + 1, INT64_MIN + 1, INT64_MIN + 1},
                               {INT64_MAX - WORLD_X - 1, INT64_MAX - WORLD_Y - 1, INT64_MAX - WORLD_Z - 1}};
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        CHECK(world_rebase(&w, edges[i]));
        CHECK(position_from_local(w.origin, dv3(64, 44, 98), &camera));
        CHECK(scene_update(s, &w, camera, direction, 1.6, 1024, true));
        CHECK(mesh_build_chunk_lod(&m, &w, 0, 4));
        CHECK(world_get_chunk_mm(&w, -1, 0, 0, 0).material == MAT_AIR);
        CHECK(!world_prepare_ray(&w, camera, v3(0, 0, 0), 80));
        CHECK(!world_prepare_ray(&w, camera, direction, -1));
    }
    /* Direction/aspect sweeps exercise cache turnover at the maximum view range. */
    CHECK(world_rebase(&w, cell_coord(0, 0, 0)));
    CHECK(position_from_local(w.origin, dv3(64, 44, 98), &camera));
    for (int turn = 0; turn < 32; turn++) {
        double angle = turn * 6.283185307179586 / 32;
        CHECK(scene_update(s, &w, camera, v3((float)sin(angle), -.1f, (float)cos(angle)), 2.5, 1024, true));
        CHECK(s->visible < SCENE_SLOTS);
    }
    mesh_destroy(&m);
    free(s);
    world_destroy(&w);
}
int main(void) {
    test_lazy_data();
    test_scene_lods();
    puts("PASS: lazy generation, exact queries/seams, cache-independent V5 saves, edit retention, frustum "
         "culling, LOD hysteresis, disjoint streamed tiles, camera movement, large-address picking");
    return 0;
}
