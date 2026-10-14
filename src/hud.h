#ifndef HUD_H
#define HUD_H
#include "scene.h"
#include "planet.h"
#include "resources.h"
#include "solar.h"
typedef struct {
    Vertex *vertices;
    size_t count, capacity;
    bool overflow;
} Hud;
bool hud_init(Hud *hud, size_t capacity);
void hud_destroy(Hud *hud);
void hud_build(Hud *hud, const World *world, RayHit hit, uint16_t selected, WorldPosition camera, int width,
               int height, float fps, size_t vertex_count, bool captured, const char *geometry_view,
               bool workshop, int brush_mm, const char *status, const Scene *scene);
void hud_build_planet(Hud *hud, Planet *planet, const PlanetBody *body, PlanetHit hit, uint16_t material,
                      int width, int height, float fps, size_t vertices, const PlanetScene *scene, bool fly,
                      int brush_mm, bool sphere, const char *geometry, const char *status,
                      const char *terrain_status);
void hud_build_solar(Hud *hud, const SolarSystem *system, int focus, int width, int height, float fps,
                     double time_scale, double speed, double clock_rate, double deficit);
void hud_relativity(Hud *hud, int width, int height, double time, double scale, double clock_rate,
                    double deficit);
void hud_resources(Hud *hud, const ResourceStats *stats, const ResourceScan *scan, int width, int height,
                   bool performance, bool materials, uint16_t selected);
#endif
