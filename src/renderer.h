#ifndef RENDERER_H
#define RENDERER_H
#include "scene.h"
#include "planet.h"
#include "resources.h"
#include "solar.h"
#include "water.h"
#include <SDL.h>
#define HUD_VERTEX_CAPACITY 131072
typedef struct Renderer Renderer;
typedef enum { GEOMETRY_SOLID, GEOMETRY_OVERLAY, GEOMETRY_WIREFRAME, GEOMETRY_VIEW_COUNT } GeometryView;
typedef struct {
    Mat4 view_projection;
    float selected[4];
    float chunk_offset[4]; /* Chunk origin relative to camera, in cells. */
    float fine_target[4];  /* Target 1 mm cell in world-local millimeters; converted per chunk. */
    float viewport[4];     /* drawable width, height, geometry view, cell size relative to original 250 mm */
} RenderPush;
Renderer *renderer_create(SDL_Window *window, bool validation, bool vsync);
void renderer_destroy(Renderer *r);
bool renderer_start_mesher(Renderer *r, const char *data, const char *save, const Planet *planet);
/* Waits for the previous frame before touching GPU mesh/HUD memory. */
bool renderer_sync_world(Renderer *r, World *world, WorldPosition camera, Vec3 direction, float view_m,
                         bool dynamic);
const Scene *renderer_scene(const Renderer *renderer);
bool renderer_sync_planet(Renderer *renderer, Planet *planet, WorldPosition camera, Vec3 direction, Vec3 up,
                          double view_m, PlanetHit target);
const PlanetScene *renderer_planet_scene(const Renderer *renderer);
bool renderer_draw(Renderer *r, RenderPush *push, WorldPosition camera, const Vertex *hud, size_t hud_count);
void renderer_size(Renderer *r, int *width, int *height);
void renderer_solar_view(Renderer *r, const SolarSystem *system, DVec3 observer_m, DVec3 earth_local_m,
                         bool inspection);
void renderer_water_optics(Renderer *r, WaterOptics optics);
void renderer_occlusion(Renderer *r, bool enabled);
size_t renderer_vertex_count(const Renderer *r);
void renderer_resources(const Renderer *renderer, ResourceStats *stats);
#endif
