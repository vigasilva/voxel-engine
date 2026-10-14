#include "planet_worker.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
struct PlanetWorker {
    SDL_Thread *thread;
    SDL_mutex *mutex;
    SDL_cond *wake;
    int state;
    bool stop;
    char *data, *save;
    double mass, a, b;
    bool relief;
    PlanetMeshResult result;
};
static char *copy_string(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
static int run(void *arg) {
    PlanetWorker *w = arg;
    Planet p = {0};
    bool initialized = false;
    uint64_t revision = UINT64_MAX, surface_revision = UINT64_MAX;
    SDL_LockMutex(w->mutex);
    for (;;) {
        while (!w->stop && w->state != 1)
            SDL_CondWait(w->wake, w->mutex);
        if (w->stop)
            break;
        PlanetMeshResult job = w->result;
        w->result.mesh = (Mesh){0};
        w->state = 2;
        SDL_UnlockMutex(w->mutex);
        if (!initialized || revision != job.revision) {
            planet_destroy(&p);
            initialized = planet_init(&p, w->data, w->save, w->mass, w->a, w->b);
            p.relief_enabled = w->relief;
            revision = job.revision;
            surface_revision = UINT64_MAX;
        }
        if (initialized && surface_revision != job.surface_revision) {
            planet_detail_refresh(&p);
            surface_revision = job.surface_revision;
        }
        /* This thread owns its SQLite connection, caches, and mapped height metadata.
           The main thread rejects results if any contributing revision changed. */
        job.ok = initialized && (job.patch.edited ? planet_mesh_edits(&job.mesh, &p, &job.patch)
                                                  : planet_mesh_patch(&job.mesh, &p, &job.patch));
        if (!job.ok)
            snprintf(job.error, sizeof(job.error), "%s",
                     p.error[0] ? p.error : "Mesh worker allocation failed");
        SDL_LockMutex(w->mutex);
        w->result = job;
        w->state = 3;
    }
    SDL_UnlockMutex(w->mutex);
    planet_destroy(&p);
    return 0;
}
PlanetWorker *planet_worker_create(const char *data, const char *save, const Planet *p) {
    if (!strcmp(save, ":memory:"))
        return NULL;
    PlanetWorker *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;
    w->data = copy_string(data);
    w->save = copy_string(save);
    w->mass = p->mass_kg;
    w->a = p->a_m;
    w->b = p->b_m;
    w->relief = p->relief_enabled;
    w->mutex = SDL_CreateMutex();
    w->wake = SDL_CreateCond();
    if (w->data && w->save && w->mutex && w->wake)
        w->thread = SDL_CreateThread(run, "terrain-mesher", w);
    if (!w->thread) {
        planet_worker_destroy(w);
        return NULL;
    }
    return w;
}
void planet_worker_destroy(PlanetWorker *w) {
    if (!w)
        return;
    if (w->thread) {
        SDL_LockMutex(w->mutex);
        w->stop = true;
        SDL_CondSignal(w->wake);
        SDL_UnlockMutex(w->mutex);
        SDL_WaitThread(w->thread, NULL);
    }
    mesh_destroy(&w->result.mesh);
    if (w->wake)
        SDL_DestroyCond(w->wake);
    if (w->mutex)
        SDL_DestroyMutex(w->mutex);
    free(w->data);
    free(w->save);
    free(w);
}
bool planet_worker_submit(PlanetWorker *w, int slot, const PlanetPatch *patch, const Planet *p) {
    if (!w)
        return false;
    SDL_LockMutex(w->mutex);
    bool accepted = w->state == 0;
    if (accepted) {
        w->result = (PlanetMeshResult){
            .patch = *patch, .slot = slot, .revision = p->revision, .surface_revision = p->surface_revision};
        w->state = 1;
        SDL_CondSignal(w->wake);
    }
    SDL_UnlockMutex(w->mutex);
    return accepted;
}
bool planet_worker_take(PlanetWorker *w, PlanetMeshResult *out) {
    if (!w)
        return false;
    SDL_LockMutex(w->mutex);
    bool complete = w->state == 3;
    if (complete) {
        *out = w->result;
        w->result.mesh = (Mesh){0};
        w->state = 0;
    }
    SDL_UnlockMutex(w->mutex);
    return complete;
}
