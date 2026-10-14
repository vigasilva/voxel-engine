#include "planet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static DVec3 cross(DVec3 a, DVec3 b) {
    return dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static double mesh_hit(const Mesh *mesh, const PlanetPatch *patch, DVec3 origin, DVec3 direction) {
    DVec3 offset = dv3(patch->origin.x * .125 - origin.x, patch->origin.y * .125 - origin.y,
                       patch->origin.z * .125 - origin.z);
    double nearest = INFINITY;
    for (size_t i = 0; i < mesh->count; i += 3) {
        DVec3 q[3];
        for (int j = 0; j < 3; j++) {
            Vec3 v = mesh->vertices[i + j].position;
            q[j] = pd_add(offset, dv3(v.x * .125, v.y * .125, v.z * .125));
        }
        DVec3 e1 = pd_sub(q[1], q[0]), e2 = pd_sub(q[2], q[0]), h = cross(direction, e2);
        double det = pd_dot(e1, h);
        if (fabs(det) < 1e-16)
            continue;
        double inv = 1 / det, u = -pd_dot(q[0], h) * inv;
        if (u < -1e-6 || u > 1 + 1e-6)
            continue;
        DVec3 qcross = cross(pd_scale(q[0], -1), e1);
        double v = pd_dot(direction, qcross) * inv;
        if (v < -1e-6 || u + v > 1 + 1e-6)
            continue;
        double distance = pd_dot(e2, qcross) * inv;
        if (distance >= 0 && distance < nearest)
            nearest = distance;
    }
    return nearest;
}
static PlanetPatch terrain_patch(Planet *p, double lat, double lon, int level, int lod) {
    DVec3 base = planet_from_geographic(p, lat, lon, 0), dir = pd_normalize(base);
    int face = 0;
    double best = -1;
    for (int i = 0; i < 6; i++) {
        double dot = pd_dot(dir, planet_cube_direction(i, 0, 0));
        if (dot > best) {
            best = dot;
            face = i;
        }
    }
    DVec3 f = planet_cube_direction(face, 0, 0),
          u = pd_sub(pd_scale(planet_cube_direction(face, 1, 0), sqrt(2)), f),
          v = pd_sub(pd_scale(planet_cube_direction(face, 0, 1), sqrt(2)), f);
    double size = 2.0 / (1u << level);
    PlanetPatch patch = {.face = face,
                         .level = level,
                         .terrain_lod = lod,
                         .river_lod = PLANET_RIVER_DISABLED_LOD,
                         .x = (uint32_t)floor((pd_dot(base, u) / pd_dot(base, f) + 1) / size),
                         .y = (uint32_t)floor((pd_dot(base, v) / pd_dot(base, f) + 1) / size),
                         .span_m = p->a_m * size};
    patch.center_m = planet_surface_point(
        p, planet_cube_direction(face, -1 + (patch.x + .5) * size, -1 + (patch.y + .5) * size), true);
    patch.radius_m = patch.span_m * 1.5;
    WorldPosition origin;
    CHECK(planet_set_position(patch.center_m, &origin));
    patch.origin = origin.cell;
    return patch;
}
typedef struct {
    Vec3 a, b;
    bool surface;
} Edge;
static int compare_position(Vec3 a, Vec3 b) {
    if (a.x != b.x)
        return a.x < b.x ? -1 : 1;
    if (a.y != b.y)
        return a.y < b.y ? -1 : 1;
    return a.z < b.z ? -1 : a.z > b.z;
}
static int compare_edge(const void *aa, const void *bb) {
    const Edge *a = aa, *b = bb;
    int c = compare_position(a->a, b->a);
    return c ? c : compare_position(a->b, b->b);
}
static void check_seams(const Mesh *mesh) {
    Edge *edges = malloc(mesh->count * sizeof(*edges));
    CHECK(edges);
    const Material *stone = material_get(MAT_STONE);
    Vec3 skirt = v3(stone->color[0], stone->color[1], stone->color[2]);
    size_t count = 0;
    for (size_t i = 0; i < mesh->count; i += 3) {
        bool surface = memcmp(&mesh->vertices[i].color, &skirt, sizeof(skirt)) != 0;
        for (int j = 0; j < 3; j++) {
            Vec3 a = mesh->vertices[i + j].position, b = mesh->vertices[i + (j + 1) % 3].position;
            if (compare_position(a, b) > 0) {
                Vec3 swap = a;
                a = b;
                b = swap;
            }
            edges[count++] = (Edge){a, b, surface};
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
    printf("Adaptive seams: %zu paired, %zu unpaired\n", paired, unpaired);
    CHECK(paired > 10 && unpaired == 0);
    free(edges);
}
static void test_sizes(void) {
    const int sizes[] = {1, 1000};
    for (size_t i = 0; i < 2; i++) {
        Planet p;
        CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
        double lat = 40.035, lon = -104.975, ground = planet_elevation(&p, lat, lon);
        DVec3 point = planet_from_geographic(&p, lat, lon, ground), up = planet_up(&p, point);
        PlanetPatch patch = terrain_patch(&p, lat, lon, 16, 12);
        PlanetBox box;
        double xyz[3] = {point.x, point.y, point.z};
        for (int a = 0; a < 3; a++) {
            box.lo[a] = (int64_t)floor(xyz[a] * 1000) - sizes[i] / 2;
            box.hi[a] = box.lo[a] + sizes[i];
        }
        CHECK(planet_paint(&p, box, (Voxel){MAT_AIR, 0, 315}));
        Mesh mesh = {0};
        CHECK(planet_mesh_patch(&mesh, &p, &patch));
        CHECK(!isfinite(mesh_hit(&mesh, &patch, pd_add(point, pd_scale(up, .7)), pd_scale(up, -1))));
        printf("%d mm excavation: %zu base vertices\n", sizes[i], mesh.count);
        CHECK(mesh.count < 25000);
        mesh_destroy(&mesh);
        planet_destroy(&p);
    }
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    double lat = 40.035, lon = -104.975;
    DVec3 ground = planet_from_geographic(&p, lat, lon, planet_elevation(&p, lat, lon)),
          up = planet_up(&p, ground), floating = pd_add(ground, pd_scale(up, .2));
    PlanetPatch patch = terrain_patch(&p, lat, lon, 16, 12);
    double xyz[3] = {floating.x, floating.y, floating.z};
    PlanetBox box;
    for (int a = 0; a < 3; a++) {
        box.lo[a] = (int64_t)floor(xyz[a] * 1000);
        box.hi[a] = box.lo[a] + 1;
    }
    CHECK(planet_paint(&p, box, (Voxel){MAT_COPPER, 0, 300}));
    Mesh mesh = {0};
    CHECK(planet_mesh_patch(&mesh, &p, &patch));
    CHECK(mesh.count < 3500);
    check_seams(&mesh);
    mesh_destroy(&mesh);
    planet_destroy(&p);
}
static void test_edge_cut(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    PlanetPatch patch = terrain_patch(&p, 40.035, -104.975, 16, 12);
    double size = 2.0 / (1u << patch.level);
    DVec3 point = planet_surface_point(
              &p, planet_cube_direction(patch.face, -1 + patch.x * size, -1 + (patch.y + .5) * size), true),
          up = planet_up(&p, point);
    PlanetBox box;
    double xyz[3] = {point.x, point.y, point.z};
    for (int a = 0; a < 3; a++) {
        box.lo[a] = (int64_t)floor(xyz[a] * 1000) - 32;
        box.hi[a] = box.lo[a] + 65;
    }
    CHECK(planet_paint(&p, box, (Voxel){MAT_AIR, 0, 310}));
    Mesh mesh = {0};
    CHECK(planet_mesh_patch(&mesh, &p, &patch));
    DVec3 origin = pd_add(point, pd_scale(up, .2)), direction = pd_scale(up, -1);
    double hit = mesh_hit(&mesh, &patch, origin, direction);
    if (isfinite(hit)) {
        DVec3 q = pd_add(origin, pd_scale(direction, hit));
        double mm[3] = {q.x * 1000, q.y * 1000, q.z * 1000};
        bool inside = true;
        for (int a = 0; a < 3; a++)
            inside &= mm[a] > box.lo[a] + .1 && mm[a] < box.hi[a] - .1;
        CHECK(!inside);
    }
    CHECK(mesh.count < 5000);
    printf("Patch-boundary cut: %zu vertices; no terrain/skirt hit inside removed matter\n", mesh.count);
    mesh_destroy(&mesh);
    planet_destroy(&p);
}
static void test_complete_edited_view(void) {
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    DVec3 eye = planet_from_geographic(&p, 40, -105, planet_elevation(&p, 40, -105) + 1.803),
          up = planet_up(&p, eye), east = pd_normalize(dv3(-up.y, up.x, 0)), north = cross(up, east),
          direction = pd_add(pd_scale(north, cos(.3)), pd_scale(up, -sin(.3)));
    WorldPosition camera;
    CHECK(planet_set_position(eye, &camera));
    PlanetScene *scene = malloc(sizeof(*scene));
    CHECK(scene);
    planet_scene_init(scene);
    Mesh mesh = {0};
    const int sizes[] = {1, 1000};
    for (int pass = 0; pass < 2; pass++) {
        PlanetHit hit = planet_pick(&p, eye, direction, 10);
        CHECK(hit.hit);
        PlanetBox box;
        CHECK(planet_sphere_brush(hit, sizes[pass], true, &box));
        CHECK(planet_paint_sphere(&p, box, (Voxel){MAT_AIR, 0, 310}));
        CHECK(planet_scene_update(scene, &p, camera, v3(direction.x, direction.y, direction.z),
                                  v3(up.x, up.y, up.z), 1.6, 60000));
        size_t vertices = 0, terrain = 0, edits = 0;
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
            if (scene->patches[i].visible) {
                PlanetPatch *patch = &scene->patches[i];
                CHECK(patch->edited ? planet_mesh_edits(&mesh, &p, patch)
                                    : planet_mesh_patch(&mesh, &p, patch));
                vertices += mesh.count;
                if (patch->edited)
                    edits += mesh.count;
                else
                    terrain += mesh.count;
            }
        printf("Complete edited view: %d mm sphere, %zu patches, %zu vertices (%zu terrain / %zu edit)\n",
               sizes[pass], scene->visible, vertices, terrain, edits);
        CHECK(vertices < 300000);
    }
    mesh_destroy(&mesh);
    free(scene);
    planet_destroy(&p);
}
int main(int argc, char **argv) {
    bool focused = argc == 2 && !strcmp(argv[1], "--focused");
    if (argc > 1 && !focused)
        return 2;
    const double lat = 40.035, lon = -104.975;
    for (int pass = 0; pass < 3; pass++) {
        Planet p;
        CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
        double ground = planet_elevation(&p, lat, lon), water;
        CHECK(!planet_water_elevation(&p, lat, lon, &water));
        DVec3 point = planet_from_geographic(&p, lat, lon, ground), up = planet_up(&p, point),
              east = pd_normalize(dv3(-up.y, up.x, 0));
        PlanetPatch patch = terrain_patch(&p, lat, lon, 16 + pass * 2, 12 - pass * 2);
        Mesh mesh = {0};
        CHECK(planet_mesh_patch(&mesh, &p, &patch));
        size_t before = mesh.count;
        PlanetBox hole;
        double coordinates[3] = {point.x, point.y, point.z};
        for (int a = 0; a < 3; a++) {
            hole.lo[a] = (int64_t)floor(coordinates[a] * 1000) - 32;
            hole.hi[a] = hole.lo[a] + 65;
        }
        CHECK(planet_paint(&p, hole, (Voxel){MAT_AIR, 0, 310}));
        CHECK(planet_mesh_patch(&mesh, &p, &patch));
        printf("Local edit: span %.3f m, %zu -> %zu vertices\n", patch.span_m, before, mesh.count);
        CHECK(mesh.count < before * 8 + 6000);
        /* Independent triangle ray tests must see a hole at its true physical
           position, with accurate surviving ground immediately beside it. */
        double hit = mesh_hit(&mesh, &patch, pd_add(point, pd_scale(up, .2)), pd_scale(up, -1));
        CHECK(!isfinite(hit));
        size_t checked = 0;
        for (int side = -1; side <= 1; side += 2) {
            DVec3 nearby = pd_add(point, pd_scale(east, side * .052));
            PlanetGeo g = planet_geographic(&p, nearby);
            DVec3 actual =
                planet_from_geographic(&p, g.latitude_deg, g.longitude_deg,
                                       planet_visible_elevation(&p, g.latitude_deg, g.longitude_deg));
            bool inside = true;
            for (int axis = 0; axis < 3; axis++) {
                double mm = (axis == 0 ? actual.x : axis == 1 ? actual.y : actual.z) * 1000;
                inside &= mm >= hole.lo[axis] && mm < hole.hi[axis];
            }
            if (inside)
                continue;
            DVec3 n = planet_up(&p, actual);
            hit = mesh_hit(&mesh, &patch, pd_add(actual, pd_scale(n, .2)), pd_scale(n, -1));
            printf("  Surviving ground error %.6f mm\n", (hit - .2) * 1000);
            CHECK(isfinite(hit) && fabs(hit - .2) < .00035);
            checked++;
        }
        CHECK(checked > 0);
        /* The edit volume and temperature remain unchanged by rendering. */
        CHECK(planet_voxel(&p, hole.lo[0] + 1, hole.lo[1] + 1, hole.lo[2] + 1).material == MAT_AIR);
        CHECK(planet_voxel(&p, hole.lo[0] + 1, hole.lo[1] + 1, hole.lo[2] + 1).temperature_k == 310);
        mesh_destroy(&mesh);
        planet_destroy(&p);
    }
    test_sizes();
    test_edge_cut();
    if (!focused)
        test_complete_edited_view();
    puts("PASS: local edit geometry cost, true excavation position, adjacent height accuracy and unchanged "
         "matter");
}
