#define _POSIX_C_SOURCE 200809L
#include "planet_worker.h"
#include <SDL.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static PlanetMeshResult take(PlanetWorker *worker) {
    PlanetMeshResult result = {0};
    for (int i = 0; i < 15000; i++) {
        if (planet_worker_take(worker, &result))
            return result;
        SDL_Delay(1);
    }
    CHECK(false);
    return result;
}
int main(void) {
    char path[] = "/tmp/voxel-worker-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    remove(path);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, path, EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    DVec3 pos = planet_from_geographic(&p, 40, -105, planet_visible_elevation(&p, 40, -105) + 60);
    int64_t mm[3] = {(int64_t)floor(pos.x * 1000), (int64_t)floor(pos.y * 1000),
                     (int64_t)floor(pos.z * 1000)};
    PlanetBox box;
    for (int a = 0; a < 3; a++) {
        box.lo[a] = mm[a];
        box.hi[a] = mm[a] + 1;
    }
    CHECK(planet_paint(&p, box, (Voxel){MAT_COPPER, 0, 300}));
    WorldPosition origin;
    CHECK(planet_set_position(pos, &origin));
    PlanetPatch patch = {.edited = true,
                         .origin = origin.cell,
                         .edit_key = {mm[0] / 2000 - (mm[0] % 2000 < 0), mm[1] / 2000 - (mm[1] % 2000 < 0),
                                      mm[2] / 2000 - (mm[2] % 2000 < 0)},
                         .edit_lod_mm = 1,
                         .span_m = 2};
    PlanetWorker *worker = planet_worker_create(TEST_EARTH_DATA, path, &p);
    CHECK(worker);
    CHECK(planet_worker_submit(worker, 3, &patch, &p));
    CHECK(!planet_worker_submit(worker, 4, &patch, &p));
    PlanetMeshResult first = take(worker);
    CHECK(first.ok && first.slot == 3 && first.revision == p.revision && first.mesh.count == 36);
    mesh_destroy(&first.mesh);
    CHECK(planet_paint(&p, box, (Voxel){MAT_AIR, 0, 300}));
    CHECK(planet_worker_submit(worker, 3, &patch, &p));
    PlanetMeshResult second = take(worker);
    CHECK(second.ok && second.revision == p.revision && second.mesh.count == 0);
    mesh_destroy(&second.mesh);
    CHECK(planet_worker_submit(worker, 3, &patch, &p));
    planet_worker_destroy(worker);
    planet_destroy(&p);
    remove(path);
    char side[128];
    snprintf(side, sizeof(side), "%s-wal", path);
    remove(side);
    snprintf(side, sizeof(side), "%s-shm", path);
    remove(side);
    puts("Independent background meshes, revision refresh, queue bounds, and shutdown passed");
    return 0;
}
