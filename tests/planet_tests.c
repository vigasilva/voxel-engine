#include "planet.h"
#include "resources.h"
#include <sqlite3.h>
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
static void remove_save(const char *file) {
    remove(file);
    char name[256];
    snprintf(name, sizeof(name), "%s-wal", file);
    remove(name);
    snprintf(name, sizeof(name), "%s-shm", file);
    remove(name);
}
static void test_smooth_edit_surface(void) {
    const Material *stone = material_get(MAT_STONE), *copper = material_get(MAT_COPPER);
    Vec3 color = v3(stone->color[0], stone->color[1], stone->color[2]);
    Vertex vertices[6] = {{{0, 0, 0}, {1, 0, 0}, color}, {{0, 1, 0}, {1, 0, 0}, color},
                          {{0, 0, 1}, {1, 0, 0}, color}, {{0, 0, 0}, {0, 1, 0}, color},
                          {{1, 0, 0}, {0, 1, 0}, color}, {{0, 0, 1}, {0, 1, 0}, color}};
    Mesh mesh = {.vertices = vertices, .count = 6};
    Vec3 positions[6];
    for (int i = 0; i < 6; i++)
        positions[i] = vertices[i].position;
    CHECK(planet_mesh_smooth_edits(&mesh));
    CHECK(fabs(vertices[0].normal.x - sqrt(.5)) < 1e-6 && fabs(vertices[0].normal.y - sqrt(.5)) < 1e-6);
    CHECK(!memcmp(&vertices[0].normal, &vertices[3].normal, sizeof(Vec3)));
    for (int i = 0; i < 6; i++)
        CHECK(!memcmp(&positions[i], &vertices[i].position, sizeof(Vec3)));
    for (int i = 0; i < 6; i++) {
        vertices[i].color = v3(copper->color[0], copper->color[1], copper->color[2]);
        vertices[i].normal = i < 3 ? v3(1, 0, 0) : v3(0, 1, 0);
    }
    CHECK(planet_mesh_smooth_edits(&mesh));
    CHECK(vertices[0].normal.x == 1 && vertices[3].normal.y == 1);
}
static void test_scale_and_geography(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(p.earth.rows == 1080 && p.earth.columns == 2160 &&
          planet_storage_bytes(&p) < planet_elevation_bytes(&p) + 1024 * 1024);
    double locations[][3] = {{0, 0, 0},         {40, -105, 2000}, {-33.8, 151.2, 100},
                             {89.999, 170, 10}, {-90, 0, 0},      {0, 179.999, 1e7}};
    for (size_t i = 0; i < sizeof(locations) / sizeof(locations[0]); i++) {
        DVec3 v = planet_from_geographic(&p, locations[i][0], locations[i][1], locations[i][2]);
        PlanetGeo g = planet_geographic(&p, v);
        CHECK(fabs(g.latitude_deg - locations[i][0]) < 1e-6 && fabs(g.altitude_m - locations[i][2]) < .001);
        CHECK(fabs(g.longitude_deg - locations[i][1]) < 1e-6 || fabs(locations[i][0]) == 90);
    }
    CHECK(planet_elevation(&p, 40, -105) > 1000 && planet_elevation(&p, 0, -140) < -1000);
    CHECK(fabs(planet_elevation(&p, 20, -180) - planet_elevation(&p, 20, 180)) < 1e-6);
    CHECK(fabs(planet_elevation(&p, 90, 0) - planet_elevation(&p, 90, 120)) < 1e-6);
    double expected = GRAVITATIONAL_CONSTANT * EARTH_MASS_KG / (EARTH_A_METERS * EARTH_A_METERS);
    CHECK(fabs(pd_length(planet_gravity(&p, dv3(p.a_m, 0, 0))) - expected) < 1e-10);
    CHECK(fabs(pd_length(planet_gravity(&p, dv3(2 * p.a_m, 0, 0))) - expected * .25) < 1e-10);
    CHECK(fabs(pd_length(planet_gravity(&p, dv3(p.a_m * .5, 0, 0))) - expected * .5) < 1e-10);
    CHECK(pd_length(planet_gravity(&p, dv3(0, 0, 0))) == 0);
    Planet bigger = p;
    bigger.mass_kg *= 2;
    CHECK(fabs(pd_length(planet_gravity(&bigger, dv3(p.a_m, 0, 0))) - 2 * expected) < 1e-10);
    WorldPosition pos;
    DVec3 point = planet_from_geographic(&p, 40, -105, 2000);
    CHECK(planet_set_position(point, &pos));
    CHECK(position_move_mm(&pos, dv3(1, 0, 0)));
    DVec3 after = planet_position_m(pos);
    CHECK(fabs(after.x - point.x - .001) < 1e-7);
    CHECK(!planet_raycast(&p, dv3(1e30, 0, 0), dv3(1, 0, 0), 10).hit);
    CHECK(!planet_raycast(&p, point, dv3(NAN, 0, 0), 10).hit);
    PlanetEdits invalid = {0};
    CHECK(!planet_collect(&p, (PlanetBox){{INT64_MIN, 0, 0}, {INT64_MIN, 1, 1}}, &invalid));
    printf("Earth gravity %.6f m/s2 at equator, equatorial diameter %.3f km, DEM %zu bytes\n", expected,
           2 * p.a_m / 1000, (size_t)p.earth.rows * p.earth.columns * 2);
    planet_destroy(&p);
}
static double mesh_area(const Mesh *m) {
    double sum = 0;
    for (size_t i = 0; i < m->count; i += 3) {
        Vec3 a = vsub(m->vertices[i + 1].position, m->vertices[i].position),
             b = vsub(m->vertices[i + 2].position, m->vertices[i].position);
        sum += sqrtf(vdot(vcross(a, b), vcross(a, b))) * .5 * 125 * 125;
    }
    return sum;
}
static void test_water_edits(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    DVec3 eye = planet_from_geographic(&p, 0, -140, 1), up = planet_up(&p, eye);
    PlanetHit water = planet_pick(&p, eye, pd_scale(up, -1), 4);
    CHECK(water.hit && water.value.material == MAT_WATER && fabs(water.distance_m - 1) < .002);
    CHECK(!planet_raycast(&p, eye, pd_scale(up, -1), 4).hit);
    PlanetBox hole;
    CHECK(planet_brush(water, 1, false, &hole));
    CHECK(planet_paint(&p, hole, (Voxel){MAT_AIR, 0, 290}));
    CHECK(planet_voxel(&p, hole.lo[0], hole.lo[1], hole.lo[2]).material == MAT_AIR);
    int64_t key = (int64_t)((p.a_m + 1000) / 2);
    PlanetBox grain = {{key * 2000 + 1000, 1000, 1000}, {key * 2000 + 1001, 1001, 1001}};
    CHECK(planet_paint(&p, grain, (Voxel){MAT_WATER, 0, 310}));
    PlanetPatch patch = {.edited = true, .edit_key = {key, 0, 0}, .origin = {key * 16, 0, 0}};
    Mesh mesh = {0};
    CHECK(planet_mesh_edits(&mesh, &p, &patch));
    CHECK(fabs(mesh_area(&mesh) - 6) < .01);
    DVec3 start = dv3((grain.lo[0] - 3) * .001, 1.0005, 1.0005);
    CHECK(!planet_raycast(&p, start, dv3(1, 0, 0), .01).hit);
    CHECK(planet_pick(&p, start, dv3(1, 0, 0), .01).value.material == MAT_WATER);
    mesh_destroy(&mesh);
    planet_destroy(&p);
    puts("Water: editable ocean picking and visible 1 mm water placement remain non-solid for physics");
}
static void test_spherical_edits(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    int64_t edge = ((int64_t)p.a_m / 2 + 1000) * 2000;
    PlanetBox small = {{edge - 2, -2, -2}, {edge + 3, 3, 3}};
    PlanetBox existing = {{edge - 2, -2, -2}, {edge - 1, -1, -1}};
    CHECK(planet_paint(&p, existing, (Voxel){MAT_IRON, 0, 450}));
    CHECK(planet_paint_sphere(&p, small, (Voxel){MAT_COPPER, 0, 500}));
    int count = 0;
    for (int z = 0; z < 5; z++)
        for (int y = 0; y < 5; y++)
            for (int x = 0; x < 5; x++) {
                bool inside = (x - 2) * (x - 2) + (y - 2) * (y - 2) + (z - 2) * (z - 2) <= 6;
                Voxel v = planet_voxel(&p, small.lo[0] + x, small.lo[1] + y, small.lo[2] + z);
                CHECK((v.material == MAT_COPPER) == inside);
                if (inside) {
                    CHECK(v.temperature_k == 500);
                    count++;
                }
            }
    CHECK(count == 81 && planet_saved_chunks(&p) == 8);
    CHECK(planet_voxel(&p, existing.lo[0], -2, -2).temperature_k == 450);
    const int directions[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    int exposed = 0;
    for (int z = -2; z < 3; z++)
        for (int y = -2; y < 3; y++)
            for (int x = -2; x < 3; x++) {
                if (planet_voxel(&p, edge + x, y, z).material == MAT_AIR)
                    continue;
                for (int d = 0; d < 6; d++)
                    exposed += planet_voxel(&p, edge + x + directions[d][0], y + directions[d][1],
                                            z + directions[d][2])
                                   .material == MAT_AIR;
            }
    Mesh small_mesh = {0};
    double exposed_area = 0;
    for (int z = -1; z <= 0; z++)
        for (int y = -1; y <= 0; y++)
            for (int x = -1; x <= 0; x++) {
                int64_t key = edge / 2000 + x;
                PlanetPatch small_patch = {
                    .edited = true, .edit_key = {key, y, z}, .origin = {key * 16, y * 16, z * 16}};
                CHECK(planet_mesh_edits(&small_mesh, &p, &small_patch));
                exposed_area += mesh_area(&small_mesh);
            }
    CHECK(fabs(exposed_area - exposed) < .1);
    mesh_destroy(&small_mesh);
    PlanetBox large = {{edge + 4000, -500, -500}, {edge + 5000, 500, 500}};
    CHECK(planet_paint_sphere(&p, large, (Voxel){MAT_STONE, 0, 320}));
    PlanetEdits edits = {0};
    CHECK(planet_collect(&p, large, &edits));
    CHECK(edits.count > 10);
    double volume = 0;
    for (size_t i = 0; i < edits.count; i++) {
        PlanetBox box = edits.items[i].box;
        volume += (double)(box.hi[0] - box.lo[0]) * (box.hi[1] - box.lo[1]) * (box.hi[2] - box.lo[2]);
    }
    double expected = 3.14159265358979323846 * 1e9 / 6;
    CHECK(fabs(volume / expected - 1) < .05);
    CHECK(planet_voxel(&p, edge + 4500, 0, 0).material == MAT_STONE);
    CHECK(planet_voxel(&p, edge + 4000, -500, -500).material == MAT_AIR);
    Mesh mesh = {0};
    PlanetPatch patch = {
        .edited = true, .edit_key = {edge / 2000 + 2, 0, 0}, .origin = {(edge / 2000 + 2) * 16, 0, 0}};
    CHECK(planet_mesh_edits(&mesh, &p, &patch));
    CHECK(mesh.count > 100 && mesh.count < 500000);
    printf("Spherical brushes: 5 mm sphere = %d cells across eight chunks, %.3f / %d mm2 exposed area; "
           "1 m sphere = %zu regions, %.2f%% volume error\n",
           count, exposed_area, exposed, edits.count, 100 * fabs(volume / expected - 1));
    planet_edits_destroy(&edits);
    mesh_destroy(&mesh);
    planet_destroy(&p);
}
static void test_distant_planet(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    PlanetScene *scene = malloc(sizeof(*scene));
    CHECK(scene);
    planet_scene_init(scene);
    const double altitudes[] = {60000, 43000000, 1e9};
    for (size_t i = 0; i < 3; i++) {
        WorldPosition camera;
        CHECK(planet_set_position(planet_from_geographic(&p, 40, -105, altitudes[i]), &camera));
        DVec3 position = planet_position_m(camera), up = planet_up(&p, position),
              direction = pd_normalize(pd_scale(position, -1));
        double far = planet_view_distance(&p, camera, 100), near = planet_near_distance(&p, camera);
        CHECK(far > pd_length(position) + p.a_m && near > .0001 && near < altitudes[i]);
        Vec3 d = v3((float)direction.x, (float)direction.y, (float)direction.z),
             u = v3((float)up.x, (float)up.y, (float)up.z);
        CHECK(planet_scene_update(scene, &p, camera, d, u, 1.6, far));
        CHECK(scene->visible > 5 && scene->visible < PLANET_PATCH_LIMIT);
        Mat4 projection = mat_mul(mat_perspective_reverse_z(1.22173f, 1.6f, (float)near * 8, (float)far * 8),
                                  mat_view_up(v3(0, 0, 0), d, u));
        DVec3 r = pd_scale(pd_sub(planet_surface_point(&p, pd_normalize(position), true), position), 8);
        float z = projection.m[2] * r.x + projection.m[6] * r.y + projection.m[10] * r.z + projection.m[14],
              w = projection.m[3] * r.x + projection.m[7] * r.y + projection.m[11] * r.z + projection.m[15];
        CHECK(w > 0 && z / w > 1.0 / 16777215 && z / w < 1);
        if (i == 1)
            printf("43 million m visibility: %zu surface patches, near %.1f m, far %.1f km, depth %.6g\n",
                   scene->visible, near, far / 1000, z / w);
    }
    free(scene);
    planet_destroy(&p);
}
static void test_surface_spheres(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    DVec3 eye = planet_from_geographic(&p, 40, -105, planet_elevation(&p, 40, -105) + 1.803),
          up = planet_up(&p, eye), east = pd_normalize(dv3(-up.y, up.x, 0)),
          north = dv3(up.y * east.z - up.z * east.y, up.z * east.x - up.x * east.z,
                      up.x * east.y - up.y * east.x),
          forward = pd_add(pd_scale(north, cos(.3)), pd_scale(up, -sin(.3)));
    WorldPosition camera;
    CHECK(planet_set_position(eye, &camera));
    PlanetScene *scene = malloc(sizeof(*scene));
    CHECK(scene);
    planet_scene_init(scene);
    Mesh mesh = {0};
    const int diameters[] = {1, 5, 125, 250, 1000};
    for (size_t step = 0; step < 5; step++) {
        PlanetHit hit = planet_pick(&p, eye, forward, 10);
        PlanetBox bounds;
        CHECK(planet_sphere_brush(hit, diameters[step], true, &bounds));
        CHECK(planet_paint_sphere(&p, bounds, (Voxel){MAT_COPPER, 0, 400}));
        bool planned =
            planet_scene_update(scene, &p, camera, v3((float)forward.x, (float)forward.y, (float)forward.z),
                                v3((float)up.x, (float)up.y, (float)up.z), 1.6, 60000);
        if (!planned)
            fprintf(stderr, "Sphere planning failure: diameter %d: %s\n", diameters[step], p.error);
        CHECK(planned);
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
            PlanetPatch *patch = &scene->patches[i];
            if (!patch->visible || patch->span_m >= 40)
                continue;
            bool ok =
                patch->edited ? planet_mesh_edits(&mesh, &p, patch) : planet_mesh_patch(&mesh, &p, patch);
            if (!ok)
                fprintf(stderr, "Sphere mesh failure: diameter %d, edited %d, span %.3f m, %zu vertices\n",
                        diameters[step], patch->edited, patch->span_m, mesh.count);
            CHECK(ok);
        }
    }
    mesh_destroy(&mesh);
    free(scene);
    planet_destroy(&p);
    puts("Surface spheres: sequential 1/5/125/250/1000 mm strokes mesh against oblique Earth terrain");
}
static void test_projected_river_lod(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    uint32_t *ids = NULL;
    size_t count = 0;
    CHECK(planet_river_candidates(&p, -3, -60, 15000, &ids, &count) && count);
    PlanetRiverSegment river;
    CHECK(planet_river_segment(&p, ids[0], &river));
    free(ids);
    double lat = (river.lat[0] + river.lat[1]) * .5, lon = (river.lon[0] + river.lon[1]) * .5;
    CHECK(!planet_detail_sample(&p, lat, lon, NULL, NULL));
    DVec3 direction = pd_normalize(planet_from_geographic(&p, lat, lon, 0));
    int face = 0;
    double best = -1;
    for (int i = 0; i < 6; i++) {
        double dot = pd_dot(direction, planet_cube_direction(i, 0, 0));
        if (dot > best) {
            best = dot;
            face = i;
        }
    }
    DVec3 f = planet_cube_direction(face, 0, 0),
          u = pd_sub(pd_scale(planet_cube_direction(face, 1, 0), sqrt(2)), f),
          v = pd_sub(pd_scale(planet_cube_direction(face, 0, 1), sqrt(2)), f);
    int level = 10;
    double size = 2.0 / (1u << level);
    PlanetPatch patch = {.face = face,
                         .level = level,
                         .x = (uint32_t)floor((pd_dot(direction, u) / best + 1) / size),
                         .y = (uint32_t)floor((pd_dot(direction, v) / best + 1) / size),
                         .span_m = p.a_m * size};
    patch.center_m = planet_surface_point(
        &p, planet_cube_direction(face, -1 + (patch.x + .5) * size, -1 + (patch.y + .5) * size), true);
    patch.radius_m = patch.span_m * 1.5;
    WorldPosition origin;
    CHECK(planet_set_position(patch.center_m, &origin));
    patch.origin = origin.cell;
    Mesh near = {0}, far = {0}, disabled = {0};
    CHECK(planet_mesh_patch(&near, &p, &patch));
    patch.river_lod = 12;
    CHECK(planet_mesh_patch(&far, &p, &patch));
    patch.river_lod = PLANET_RIVER_DISABLED_LOD;
    CHECK(planet_mesh_patch(&disabled, &p, &patch));
    CHECK(near.count > far.count && far.count == disabled.count);
    PlanetScene scene;
    planet_scene_init(&scene);
    WorldPosition camera;
    CHECK(planet_set_position(planet_from_geographic(&p, 40, -105, 43000000), &camera));
    DVec3 aim = pd_normalize(pd_scale(planet_position_m(camera), -1));
    CHECK(planet_scene_update(&scene, &p, camera, v3((float)aim.x, (float)aim.y, (float)aim.z), v3(0, 0, 1),
                              1.6, planet_view_distance(&p, camera, 0)));
    CHECK(scene.river_disabled && scene.visible > 0);
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
        if (scene.patches[i].visible && !scene.patches[i].edited)
            CHECK(scene.patches[i].river_lod == PLANET_RIVER_DISABLED_LOD);
    printf("Projected rivers: Amazon patch %zu -> %zu vertices; planetary view disables every river ribbon\n",
           near.count, far.count);
    mesh_destroy(&near);
    mesh_destroy(&far);
    mesh_destroy(&disabled);
    planet_destroy(&p);
}
typedef struct {
    Vec3 a, b;
    bool surface;
} TerrainEdge;
static int compare_position(Vec3 a, Vec3 b) {
    if (a.x != b.x)
        return a.x < b.x ? -1 : 1;
    if (a.y != b.y)
        return a.y < b.y ? -1 : 1;
    return a.z < b.z ? -1 : a.z > b.z;
}
static int compare_edge(const void *aa, const void *bb) {
    const TerrainEdge *a = aa, *b = bb;
    int c = compare_position(a->a, b->a);
    return c ? c : compare_position(a->b, b->b);
}
static void test_terrain_seams(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    /* Real gently curved terrain produces differently sized greedy rectangles.
       Every interior edge must have an identical partner, without T-junctions. */
    DVec3 direction = pd_normalize(planet_from_geographic(&p, 40, -105, 0));
    int face = 3, level = 21;
    DVec3 f = planet_cube_direction(face, 0, 0),
          u = pd_sub(pd_scale(planet_cube_direction(face, 1, 0), sqrt(2)), f),
          v = pd_sub(pd_scale(planet_cube_direction(face, 0, 1), sqrt(2)), f);
    double size = 2.0 / (1u << level), denominator = pd_dot(direction, f);
    PlanetPatch patch = {.face = face,
                         .level = level,
                         .x = (uint32_t)floor((pd_dot(direction, u) / denominator + 1) / size),
                         .y = (uint32_t)floor((pd_dot(direction, v) / denominator + 1) / size),
                         .span_m = p.a_m * size,
                         .river_lod = PLANET_RIVER_DISABLED_LOD};
    double u0 = -1 + patch.x * size, v0 = -1 + patch.y * size;
    patch.center_m =
        planet_surface_point(&p, planet_cube_direction(face, u0 + size * .5, v0 + size * .5), true);
    patch.radius_m = patch.span_m * 1.5;
    WorldPosition origin;
    CHECK(planet_set_position(patch.center_m, &origin));
    patch.origin = origin.cell;
    Mesh mesh = {0};
    CHECK(planet_mesh_patch(&mesh, &p, &patch));
    TerrainEdge *edges = malloc(mesh.count * sizeof(*edges));
    CHECK(edges);
    size_t count = 0;
    const Material *stone = material_get(MAT_STONE);
    Vec3 skirt = v3(stone->color[0], stone->color[1], stone->color[2]);
    for (size_t i = 0; i < mesh.count; i += 3) {
        bool surface = memcmp(&mesh.vertices[i].color, &skirt, sizeof(skirt)) != 0;
        for (int j = 0; j < 3; j++) {
            Vec3 a = mesh.vertices[i + j].position, b = mesh.vertices[i + (j + 1) % 3].position;
            if (compare_position(a, b) > 0) {
                Vec3 swap = a;
                a = b;
                b = swap;
            }
            edges[count++] = (TerrainEdge){a, b, surface};
        }
    }
    qsort(edges, count, sizeof(*edges), compare_edge);
    size_t unpaired = 0, paired = 0;
    for (size_t i = 0; i < count;) {
        size_t end = i + 1;
        bool surface = edges[i].surface;
        while (end < count && !compare_edge(&edges[i], &edges[end])) {
            surface |= edges[end].surface;
            end++;
        }
        if (surface) {
            paired += end - i == 2;
            unpaired += end - i != 2;
        }
        i = end;
    }
    printf("Terrain seams: %zu paired surface/skirt edges, %zu unpaired, %zu vertices\n", paired, unpaired,
           mesh.count);
    CHECK(paired > 100 && unpaired == 0);
    CHECK(mesh.count < mesh.unit_faces * 6);
    free(edges);
    mesh_destroy(&mesh);
    planet_destroy(&p);
}
static void test_height_and_greedy(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(p.region_count == 3 && p.earth_fine.rows == 2160);
    CHECK(planet_measured_elevation(&p, 40, -105.8) > 2500);
    CHECK(planet_measured_elevation(&p, 27.99, 86.93) > 7000);
    DVec3 point = planet_from_geographic(&p, 40, -105, 0), up = planet_up(&p, point),
          east = pd_normalize(dv3(-up.y, up.x, 0));
    double minimum = HUGE_VAL, maximum = -HUGE_VAL;
    for (int i = -32; i <= 32; i++) {
        PlanetGeo g = planet_geographic(&p, pd_add(point, pd_scale(east, i)));
        double h = planet_elevation(&p, g.latitude_deg, g.longitude_deg);
        minimum = fmin(minimum, h);
        maximum = fmax(maximum, h);
    }
    CHECK(maximum - minimum > .5);
    DVec3 normal =
        planet_surface_normal(&p, planet_from_geographic(&p, 40, -105, planet_elevation(&p, 40, -105)));
    CHECK(pd_dot(normal, up) > .5 && pd_dot(normal, up) < 1 - 1e-10);
    /* A flat patch should merge its 16384 surface cells and edge skirts. */
    size_t regions = p.region_count;
    p.region_count = 0;
    p.relief_enabled = false;
    memset(p.earth.samples, 0, (size_t)p.earth.rows * p.earth.columns * sizeof(int16_t));
    p.earth.pole_north = p.earth.pole_south = 0;
    PlanetPatch patch = {
        .face = 0, .level = 21, .x = 1u << 20, .y = 1u << 20, .span_m = 2 * p.a_m / (1u << 21)};
    DVec3 direction = planet_cube_direction(patch.face, 0, 0);
    patch.center_m = planet_surface_point(&p, direction, true);
    patch.radius_m = patch.span_m * 1.5;
    WorldPosition origin;
    CHECK(planet_set_position(patch.center_m, &origin));
    patch.origin = origin.cell;
    Mesh mesh = {0};
    CHECK(planet_mesh_patch(&mesh, &p, &patch));
    CHECK(mesh.count == 30 && mesh.unit_faces == 16896);
    printf("Terrain heights: %.3f m local range across 64 m, native Himalaya %.1f m; flat terrain %zu -> %zu "
           "vertices\n",
           maximum - minimum, 8586.0, mesh.unit_faces * 6, mesh.count);
    mesh_destroy(&mesh);
    p.region_count = regions;
    planet_destroy(&p);
}
static void test_edit_mesh_lod(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    int64_t key = (int64_t)(p.a_m / 2) + 1000, x = key * 2000 + 960;
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int dx = 0; dx < 4; dx++) {
                PlanetBox b = {{x + dx, 960 + y, 960 + z}, {x + dx + 1, 961 + y, 961 + z}};
                CHECK(planet_paint(&p, b, (Voxel){MAT_COPPER, 0, 300 + dx + y * 4 + z * 16}));
            }
    PlanetPatch patch = {
        .edited = true, .edit_key = {key, 0, 0}, .origin = {key * 16, 0, 0}, .edit_lod_mm = 1};
    Mesh mesh = {0};
    CHECK(planet_mesh_edits(&mesh, &p, &patch));
    CHECK(mesh.count == 36 && fabs(mesh_area(&mesh) - 96) < .01);
    CHECK(planet_voxel(&p, x + 3, 963, 963).temperature_k == 363);
    size_t saved = planet_saved_chunks(&p);
    patch.edit_lod_mm = 32;
    CHECK(planet_mesh_edits(&mesh, &p, &patch));
    CHECK(mesh.count == 0);
    CHECK(planet_voxel(&p, x + 3, 963, 963).temperature_k == 363 && planet_saved_chunks(&p) == saved);
    patch.edit_lod_mm = 1;
    CHECK(planet_mesh_edits(&mesh, &p, &patch) && mesh.count == 36);
    printf("Greedy voxel skin: 96 exposed cell faces -> 6 quads; far LOD hides 4 mm feature while all 64 "
           "temperatures remain\n");
    mesh_destroy(&mesh);
    planet_destroy(&p);
}
static void test_global_edits(void) {
    const char *file = "planet-test.vxp";
    remove_save(file);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    double h = planet_elevation(&p, 40, -105);
    DVec3 eye = planet_from_geographic(&p, 40, -105, h + 3), up = planet_up(&p, eye);
    PlanetHit hit = planet_raycast(&p, eye, pd_scale(up, -1), 10);
    CHECK(hit.hit && hit.distance_m > 2.9 && hit.distance_m < 3.1);
    CHECK(material_get(hit.value.material)->solid);
    PlanetBox grain;
    CHECK(planet_brush(hit, 1, true, &grain));
    for (int a = 0; a < 3; a++)
        CHECK(grain.hi[a] - grain.lo[a] == 1);
    CHECK(planet_paint(&p, grain, (Voxel){MAT_COPPER, 0, 490}));
    CHECK(planet_voxel(&p, grain.lo[0], grain.lo[1], grain.lo[2]).temperature_k == 490);
    PlanetHit copper = planet_raycast(&p, eye, pd_scale(up, -1), 10);
    CHECK(copper.hit && copper.value.material == MAT_COPPER);
    CHECK(planet_paint(&p, grain, (Voxel){MAT_AIR, 0, 288.15f}));
    CHECK(planet_voxel(&p, grain.lo[0], grain.lo[1], grain.lo[2]).material == MAT_AIR);
    CHECK(planet_paint(&p, grain, (Voxel){MAT_COPPER, 0, 490}));
    /* Earth-wide keys: paint on another continent and inside the core. */
    DVec3 far = planet_from_geographic(&p, -33.8, 151.2, planet_elevation(&p, -33.8, 151.2) + 2);
    PlanetBox foreign = {
        {(int64_t)floor(far.x * 1000), (int64_t)floor(far.y * 1000), (int64_t)floor(far.z * 1000)}, {0}};
    for (int a = 0; a < 3; a++)
        foreign.hi[a] = foreign.lo[a] + 1;
    CHECK(planet_paint(&p, foreign, (Voxel){MAT_IRON, 0, 480}));
    CHECK(pd_length(pd_sub(far, eye)) > 1e6);
    PlanetBox core = {{0, 0, 0}, {1, 1, 1}};
    CHECK(planet_base_voxel(&p, dv3(0, 0, 0)).material == MAT_IRON);
    CHECK(planet_paint(&p, core, (Voxel){MAT_AIR, 0, 310}));
    CHECK(planet_voxel(&p, 0, 0, 0).temperature_k == 310);
    int64_t x = ((int64_t)ceil(p.a_m * 1000 / 2000) + 500) * 2000 + 1999;
    PlanetBox seam = {{x, 1999, 1999}, {x + 2, 2001, 2001}};
    CHECK(planet_paint(&p, seam, (Voxel){MAT_STONE, 0, 300}));
    Mesh mesh = {0};
    double area = 0;
    int64_t cx = x / 2000;
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 2; y++)
            for (int dx = 0; dx < 2; dx++) {
                PlanetPatch patch = {
                    .edited = true, .edit_key = {cx + dx, y, z}, .origin = {(cx + dx) * 16, y * 16, z * 16}};
                CHECK(planet_mesh_edits(&mesh, &p, &patch));
                area += mesh_area(&mesh);
            }
    CHECK(fabs(area - 24) < .01);
    size_t saved = planet_saved_chunks(&p);
    CHECK(saved >= 11);
    for (int i = 0; i < 100; i++) {
        PlanetBox b = {{x + (i + 2) * 4000, 0, 0}, {x + (i + 2) * 4000 + 1, 1, 1}};
        CHECK(planet_paint(&p, b, (Voxel){MAT_IRON, 0, 350}));
    }
    CHECK(planet_resident_chunks(&p) <= PLANET_CACHE_LIMIT &&
          planet_storage_bytes(&p) < planet_elevation_bytes(&p) + 1024 * 1024);
    CHECK(planet_voxel(&p, grain.lo[0], grain.lo[1], grain.lo[2]).temperature_k == 490);
    CHECK(planet_checkpoint(&p));
    size_t total = planet_saved_chunks(&p);
    planet_destroy(&p);
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(planet_saved_chunks(&p) == total &&
          planet_voxel(&p, foreign.lo[0], foreign.lo[1], foreign.lo[2]).temperature_k == 480);
    CHECK(planet_voxel(&p, grain.lo[0], grain.lo[1], grain.lo[2]).temperature_k == 490);
    PlanetBox bad = {{INT64_MIN, 0, 0}, {0, 1, 1}};
    CHECK(!planet_paint(&p, bad, (Voxel){MAT_SOIL, 0, 300}));
    CHECK(planet_saved_chunks(&p) == total);
    printf("Global editing: %zu saved chunks, 1 mm temperatures retained after eviction/reopen, eight-chunk "
           "seam area %.3f mm2\n",
           total, area);
    mesh_destroy(&mesh);
    planet_destroy(&p);
    remove_save(file);
}
static void test_physics_and_lod(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    double ground = planet_elevation(&p, 40, -105);
    PlanetBody b = {0};
    CHECK(planet_set_position(planet_from_geographic(&p, 40, -105, ground + 20), &b.position));
    for (int i = 0; i < 120; i++)
        planet_body_step(&p, &b, dv3(0, 0, 0), false, 1.0 / 120);
    double fallen = planet_geographic(&p, planet_position_m(b.position)).altitude_m - ground;
    CHECK(fallen > 14.8 && fallen < 15.3);
    for (int i = 0; i < 360; i++)
        planet_body_step(&p, &b, dv3(0, 0, 0), false, 1.0 / 120);
    CHECK(b.grounded);
    double eye_height = planet_geographic(&p, planet_position_m(b.position)).altitude_m - ground;
    CHECK(eye_height > 1.7 && eye_height < 1.95);
    double max = 0;
    for (int i = 0; i < 240; i++) {
        planet_body_step(&p, &b, dv3(0, 0, 0), i == 0, 1.0 / 120);
        double current =
            planet_geographic(&p, planet_position_m(b.position)).altitude_m - ground - eye_height;
        if (current > max)
            max = current;
    }
    CHECK(max > 1 && max < 1.5 && b.grounded);
    PlanetBody orbit = {0};
    double r = p.a_m + 400000, mu = GRAVITATIONAL_CONSTANT * p.mass_kg;
    CHECK(planet_set_position(dv3(r, 0, 0), &orbit.position));
    orbit.velocity_mps = dv3(0, sqrt(mu / r), 0);
    double energy0 = pd_dot(orbit.velocity_mps, orbit.velocity_mps) * .5 - mu / r;
    for (int i = 0; i < 2000; i++)
        planet_body_step(&p, &orbit, dv3(0, 0, 0), false, 1.0 / 120);
    double energy = pd_dot(orbit.velocity_mps, orbit.velocity_mps) * .5 -
                    mu / pd_length(planet_position_m(orbit.position));
    CHECK(fabs((energy - energy0) / energy0) < 1e-7);
    PlanetScene *scene = malloc(sizeof(*scene));
    CHECK(scene);
    planet_scene_init(scene);
    DVec3 eye = planet_position_m(b.position), up = planet_up(&p, eye),
          east = pd_normalize(dv3(-up.y, up.x, 0));
    DVec3 north =
        dv3(up.y * east.z - up.z * east.y, up.z * east.x - up.x * east.z, up.x * east.y - up.y * east.x);
    DVec3 f = pd_add(pd_scale(north, cos(.3)), pd_scale(up, -sin(.3)));
    CHECK(planet_scene_update(scene, &p, b.position, v3((float)f.x, (float)f.y, (float)f.z),
                              v3((float)up.x, (float)up.y, (float)up.z), 1.6, 60000));
    CHECK(scene->visible > 10 && scene->visible < PLANET_PATCH_LIMIT);
    Mesh m = {0};
    size_t vertices = 0;
    clock_t start = clock();
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
        if (scene->patches[i].visible) {
            CHECK(planet_mesh_patch(&m, &p, &scene->patches[i]));
            vertices += m.count;
            for (size_t j = 0; j < m.count; j++)
                CHECK(isfinite(m.vertices[j].position.x) && isfinite(m.vertices[j].position.y) &&
                      isfinite(m.vertices[j].position.z));
        }
    size_t ground_patches = scene->visible;
    double mesh_ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    WorldPosition space;
    CHECK(planet_set_position(planet_from_geographic(&p, 40, -105, p.a_m * 2), &space));
    up = planet_up(&p, planet_position_m(space));
    DVec3 dir = pd_scale(up, -1);
    Vec3 space_up = v3(0, 0, 1);
    CHECK(planet_scene_update(scene, &p, space, v3((float)dir.x, (float)dir.y, (float)dir.z), space_up, 1.6,
                              p.a_m * 8));
    CHECK(scene->visible > 5 && scene->visible < PLANET_PATCH_LIMIT);
    printf("Physics: freefall eye %.3f m after 1 s, grounded %.3f m, jump %.3f m, orbit relative energy "
           "drift %.3g\n",
           fallen, eye_height, max, fabs((energy - energy0) / energy0));
    printf("Planet LOD: %zu ground patches, %zu ground vertices in %.1f CPU ms, %zu overview patches\n",
           ground_patches, vertices, mesh_ms, scene->visible);
    mesh_destroy(&m);
    /* Geographic seams, polar views, altitude changes, and unusually wide windows
       must fit the same cache instead of terminating the application. */
    const double latitudes[] = {-90, -45, 0, 40, 89.999};
    const double altitudes[] = {2, 1000, 400000, EARTH_A_METERS * 2};
    for (size_t lat = 0; lat < 5; lat++)
        for (size_t alt = 0; alt < 4; alt++) {
            double longitude = lat & 1 ? 179.999 : -105;
            double height = fmax(0, planet_elevation(&p, latitudes[lat], longitude)) + altitudes[alt];
            CHECK(planet_set_position(planet_from_geographic(&p, latitudes[lat], longitude, height), &space));
            up = planet_up(&p, planet_position_m(space));
            double lon = longitude * 3.14159265358979323846 / 180;
            east = dv3(-sin(lon), cos(lon), 0);
            north = dv3(up.y * east.z - up.z * east.y, up.z * east.x - up.x * east.z,
                        up.x * east.y - up.y * east.x);
            for (int look = 0; look < 2; look++) {
                f = pd_add(pd_scale(north, look ? cos(.3) : .1), pd_scale(up, look ? -sin(.3) : -.995));
                CHECK(planet_scene_update(scene, &p, space, v3((float)f.x, (float)f.y, (float)f.z),
                                          v3((float)up.x, (float)up.y, (float)up.z), 4, 2e8));
                CHECK(scene->visible <= PLANET_PATCH_LIMIT);
            }
        }
    free(scene);
    planet_destroy(&p);
}
static void test_atomic_edits(void) {
    const char *file = "planet-transaction-test.vxp";
    remove_save(file);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    /* Negative coordinates, eight chunk boundaries, failure after the first SQL write. */
    int64_t edge = -(int64_t)(p.a_m * 1000 + 1000000);
    edge -= edge % 2000;
    PlanetBox b = {{edge - 1, -1, -1}, {edge + 1, 1, 1}};
    sqlite3 *db;
    CHECK(sqlite3_open(file, &db) == SQLITE_OK);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "CREATE TRIGGER fail_brush BEFORE INSERT ON chunks WHEN NEW.x=%lld "
             "AND NEW.y=-1 AND NEW.z=-1 BEGIN SELECT RAISE(ABORT,'injected failure'); END",
             (long long)(edge / 2000));
    CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    CHECK(!planet_paint_sphere(&p, b, (Voxel){MAT_COPPER, 0, 400}));
    CHECK(planet_saved_chunks(&p) == 0 && p.revision == 0);
    CHECK(planet_voxel(&p, edge - 1, -1, -1).material == MAT_AIR);
    sqlite3_stmt *s;
    CHECK(sqlite3_prepare_v2(db, "SELECT count(*) FROM chunks", -1, &s, NULL) == SQLITE_OK);
    CHECK(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);
    CHECK(sqlite3_exec(db, "DROP TRIGGER fail_brush", NULL, NULL, NULL) == SQLITE_OK);
    CHECK(planet_paint(&p, b, (Voxel){MAT_COPPER, 0, 400}));
    CHECK(planet_saved_chunks(&p) == 8 && p.revision == 1);
    CHECK(planet_voxel(&p, edge - 1, -1, -1).temperature_k == 400);
    sqlite3_close(db);
    planet_destroy(&p);
    CHECK(sqlite3_open(file, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db, "UPDATE terrain_config SET version=2,checksum=0", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(planet_saved_chunks(&p) == 8);
    CHECK(planet_voxel(&p, edge, 0, 0).temperature_k == 400);
    planet_destroy(&p);
    CHECK(!planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG * 2, EARTH_A_METERS, EARTH_B_METERS));
    planet_destroy(&p);
    CHECK(sqlite3_open(file, &db) == SQLITE_OK);
    CHECK(sqlite3_exec(db,
                       "UPDATE chunks SET data=zeroblob(length(data)) WHERE id=(SELECT min(id) FROM chunks)",
                       NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(!planet_paint(&p, b, (Voxel){MAT_IRON, 0, 350}));
    CHECK(strstr(p.error, "checksum") && planet_saved_chunks(&p) == 8 && p.revision == 0);
    planet_destroy(&p);
    remove_save(file);
    puts("Atomic edits: database failure rollback, negative-coordinate retry/reopen, configuration mismatch "
         "and corrupted chunk rejection passed");
}
/* An independent, high-complexity fixture exercises old collection/tree/mesh
   limits without thousands of UI brush transactions. */
static bool checker(MmBox b, Voxel *v, int *axis, int *cut, void *context) {
    (void)context;
    *axis = -1;
    *v = (Voxel){MAT_AIR, 0, 300};
    for (int a = 0; a < 3; a++) {
        if (b.lo[a] >= 32)
            return true;
        if (b.hi[a] > 32) {
            *axis = a;
            *cut = 32;
            return true;
        }
    }
    for (int a = 0; a < 3; a++)
        if (b.hi[a] - b.lo[a] > 1) {
            *axis = a;
            *cut = (b.lo[a] + b.hi[a]) / 2;
            return true;
        }
    if ((b.lo[0] + b.lo[1] + b.lo[2]) % 2 == 0)
        v->material = MAT_COPPER;
    return true;
}
static void test_unlimited_and_resources(void) {
    const char *file = "planet-unlimited-test.vxp";
    remove_save(file);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    int64_t key = (int64_t)p.a_m / 2 + 30000;
    planet_destroy(&p);
    CHECK(!detail_build(checker, NULL));
    DetailChunk *values = detail_build_unlimited(checker, NULL);
    CHECK(values && values->nodes > DETAIL_MAX_NODES);
    DetailChunk *mask = detail_create((Voxel){MAT_AIR, 0, 293.15f});
    CHECK(mask && detail_paint_build(mask, (MmBox){{0, 0, 0}, {32, 32, 32}}, (Voxel){MAT_IRON, 0, 293.15f}));
    size_t a = detail_encoded_size(mask), b = detail_encoded_size(values), n = a + b + 20;
    unsigned char *bytes = calloc(n, 1);
    CHECK(bytes);
    memcpy(bytes, "PVED", 4);
    uint32_t fields[4] = {1, (uint32_t)a, (uint32_t)b, 0};
    size_t at = 20;
    detail_encode(mask, bytes, &at);
    detail_encode(values, bytes, &at);
    CHECK(at == n);
    fields[3] = 2166136261u;
    for (size_t i = 20; i < n; i++)
        fields[3] = (fields[3] ^ bytes[i]) * 16777619u;
    for (int f = 0; f < 4; f++)
        for (int i = 0; i < 4; i++)
            bytes[4 + f * 4 + i] = (unsigned char)(fields[f] >> (8 * i));
    sqlite3 *db;
    sqlite3_stmt *statement;
    CHECK(sqlite3_open(file, &db) == SQLITE_OK);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO chunks(id,x,y,z,data) VALUES(1,?,0,0,?)", -1, &statement,
                             NULL) == SQLITE_OK);
    sqlite3_bind_int64(statement, 1, key);
    sqlite3_bind_blob(statement, 2, bytes, (int)n, SQLITE_TRANSIENT);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    CHECK(sqlite3_prepare_v2(db, "INSERT INTO chunk_index VALUES(1,?,?,0,0,0,0)", -1, &statement, NULL) ==
          SQLITE_OK);
    sqlite3_bind_int64(statement, 1, key);
    sqlite3_bind_int64(statement, 2, key);
    CHECK(sqlite3_step(statement) == SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(db);
    free(bytes);
    detail_destroy(values);
    detail_destroy(mask);
    CHECK(planet_init(&p, TEST_EARTH_DATA, file, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    p.relief_enabled = false;
    PlanetBox bounds = {{key * 2000, 0, 0}, {key * 2000 + 32, 32, 32}};
    PlanetEdits edits = {0};
    CHECK(planet_collect(&p, bounds, &edits) && edits.count == 32768);
    PlanetPatch patch = {
        .edited = true, .edit_key = {key, 0, 0}, .origin = {key * 16, 0, 0}, .edit_lod_mm = 1};
    Mesh mesh = {0};
    CHECK(planet_mesh_edits(&mesh, &p, &patch));
    CHECK(mesh.count == 589824 && fabs(mesh_area(&mesh) - 98304) < .1);
    ResourceScan scan = {0};
    CHECK(
        resources_scan(&scan, &p, dv3(key * 2.0 + .0005, .0005, .0005), dv3(0, 1, 0), dv3(0, 0, 1), 0, .024));
    CHECK(scan.materials[RESOURCE_SCAN_GRID * RESOURCE_SCAN_GRID / 2] == MAT_COPPER);
    size_t samples = 0;
    for (int id = 0; id < MAT_COUNT; id++)
        samples += scan.counts[id];
    CHECK(samples == RESOURCE_SCAN_GRID * RESOURCE_SCAN_GRID);
    CHECK(resources_scan(&scan, &p, dv3(key * 2.0 + .0005, .0005, .0005), dv3(0, 1, 0), dv3(0, 0, 1), .01,
                         .024));
    CHECK(scan.counts[MAT_AIR] == samples);
    CHECK(!resources_scan(&scan, &p, dv3(0, 0, 0), dv3(0, 1, 0), dv3(0, 0, 1), NAN, 1));
    printf(
        "Unlimited mesh: %zu regions, %zu vertices, exact %.0f mm2 surface; material depth slices verified\n",
        edits.count, mesh.count, mesh_area(&mesh));
    mesh_destroy(&mesh);
    planet_edits_destroy(&edits);
    planet_destroy(&p);
    remove_save(file);
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    for (int i = 0; i < 300; i++) {
        PlanetBox cell = {{key * 2000 + i * 2000, 100, 100}, {key * 2000 + i * 2000 + 1, 101, 101}};
        CHECK(planet_paint(&p, cell, (Voxel){MAT_IRON, 0, 300}));
    }
    bounds = (PlanetBox){{key * 2000, 0, 0}, {key * 2000 + 600000, 2000, 2000}};
    CHECK(planet_collect(&p, bounds, &edits) && edits.count == 300);
    CHECK(planet_resident_chunks(&p) <= PLANET_CACHE_LIMIT);
    planet_edits_destroy(&edits);
    planet_destroy(&p);
    ResourceStats stats = {0};
    for (int i = 0; i < 150; i++)
        resources_sample(&stats, .01, 2);
    CHECK(stats.history_count == RESOURCE_HISTORY && stats.history_next == 30 && stats.update_ms == 2);
#ifdef __linux__
    CHECK(stats.process_bytes > 0 && stats.system_bytes > 0 && stats.available_bytes > 0);
#endif
    puts("Unbounded collection: 300 saved chunks streamed through 64-entry cache; resource history and RAM "
         "verified");
}
static void test_inland_water_and_detail(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(p.earth_fine.rows == 2160 && p.earth_fine.columns == 4320);
    CHECK(planet_detail_count(&p) >= 3 && planet_river_count(&p) > 200000);
    double lat = 40.010509259259294, lon = -104.9581944444449, native, level;
    bool water;
    CHECK(planet_detail_sample(&p, lat, lon, &native, &water) && water && native > 1500);
    CHECK(planet_water_elevation(&p, lat, lon, &level) && fabs(level - native) < .001);
    double inland_level = level;
    CHECK(fabs(planet_elevation(&p, lat, lon) - (level - 2)) < .001);
    CHECK(planet_visible_material(&p, lat, lon) == MAT_WATER);
    DVec3 eye = planet_from_geographic(&p, lat, lon, level + 1), up = planet_up(&p, eye);
    PlanetHit pick = planet_pick(&p, eye, pd_scale(up, -1), 5),
              solid = planet_raycast(&p, eye, pd_scale(up, -1), 5);
    CHECK(pick.hit && pick.value.material == MAT_WATER && fabs(pick.distance_m - 1) < .005);
    CHECK(solid.hit && material_get(solid.value.material)->solid && fabs(solid.distance_m - 3) < .005);
    PlanetBox hole;
    CHECK(planet_brush(pick, 1, false, &hole) && planet_paint(&p, hole, (Voxel){MAT_AIR, 0, 290}));
    CHECK(planet_voxel(&p, hole.lo[0], hole.lo[1], hole.lo[2]).material == MAT_AIR);
    uint32_t *ids = NULL;
    size_t count = 0;
    CHECK(planet_river_candidates(&p, -3, -60, 100000, &ids, &count) && count > 100);
    bool found = false;
    double river_lat = 0, river_lon = 0;
    for (size_t i = 0; i < count; i++) {
        PlanetRiverSegment r;
        CHECK(planet_river_segment(&p, ids[i], &r));
        river_lat = (r.lat[0] + r.lat[1]) * .5;
        river_lon = (r.lon[0] + r.lon[1]) * .5;
        if (planet_measured_elevation(&p, river_lat, river_lon) > 5) {
            found = true;
            break;
        }
    }
    CHECK(found && planet_water_elevation(&p, river_lat, river_lon, &level));
    CHECK(planet_visible_material(&p, river_lat, river_lon) == MAT_WATER);
    free(ids);
    lat = -1.4847685185185209;
    lon = -48.45347222222226;
    CHECK(planet_detail_sample(&p, lat, lon, &native, &water) && water);
    CHECK(planet_visible_material(&p, lat, lon) == MAT_WATER);
    CHECK(planet_detail_refresh(&p) == 0);
    printf("Hydrology: inland water at %.2f m, exact picking/editing, Amazon network and Guama satellite "
           "mask verified\n",
           inland_level);
    planet_destroy(&p);
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--terrain-seams")) {
        test_terrain_seams();
        return 0;
    }
    test_terrain_seams();
    test_smooth_edit_surface();
    test_projected_river_lod();
    test_inland_water_and_detail();
    test_unlimited_and_resources();
    test_scale_and_geography();
    test_water_edits();
    test_spherical_edits();
    test_distant_planet();
    test_surface_spheres();
    test_height_and_greedy();
    test_edit_mesh_lod();
    test_global_edits();
    test_physics_and_lod();
    test_atomic_edits();
    puts("PASS: Earth scale/geography, variable gravity, global 1 mm persistence, bounded cache, exact "
         "cross-chunk edits, falling/collision/jumping, orbit integration, planetary mesh LOD");
    return 0;
}
