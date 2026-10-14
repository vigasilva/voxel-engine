#ifndef PLANET_WORKER_H
#define PLANET_WORKER_H
#include "planet.h"
typedef struct PlanetWorker PlanetWorker;
typedef struct {
    PlanetPatch patch;
    Mesh mesh;
    int slot;
    uint64_t revision, surface_revision;
    bool ok;
    char error[192];
} PlanetMeshResult;
PlanetWorker *planet_worker_create(const char *data, const char *save, const Planet *planet);
void planet_worker_destroy(PlanetWorker *worker);
bool planet_worker_submit(PlanetWorker *worker, int slot, const PlanetPatch *patch, const Planet *planet);
bool planet_worker_take(PlanetWorker *worker, PlanetMeshResult *result);
#endif
