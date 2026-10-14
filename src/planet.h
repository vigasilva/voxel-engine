#ifndef PLANET_H
#define PLANET_H
#include "detail.h"
#include "mesh.h"
#define EARTH_MASS_KG 5.9722e24
#define EARTH_A_METERS 6378137.0
#define EARTH_B_METERS 6356752.314245
#define GRAVITATIONAL_CONSTANT 6.67430e-11
#define PLANET_CACHE_LIMIT 64
#define PLANET_EDIT_BUDGET (64u * 1024u * 1024u)
#define PLANET_PATCH_LIMIT 768
#define PLANET_SURFACE_GRID 16
#define PLANET_REGION_LIMIT 3
#define PLANET_RIVER_DISABLED_LOD 30
#define PLANET_EDIT_RANGE_M 128.0
#define PLANET_EDIT_SURFACE_SPAN_M 320.0
typedef struct {
    int64_t lo[3], hi[3];
} PlanetBox;
typedef struct {
    PlanetBox box;
    Voxel value;
} PlanetEdit;
typedef struct {
    PlanetEdit *items;
    size_t count, capacity;
} PlanetEdits;
typedef struct {
    int16_t *samples;
    uint32_t rows, columns, checksum;
    double lat0, lon0, lat_step, lon_step, pole_south, pole_north;
} EarthData;
typedef struct PlanetStore PlanetStore;
typedef struct PlanetTerrainData PlanetTerrainData;
typedef struct {
    double mass_kg, a_m, b_m;
    EarthData earth, earth_fine;
    PlanetTerrainData *terrain;
    EarthData regions[PLANET_REGION_LIMIT];
    size_t region_count;
    uint32_t terrain_checksum;
    bool relief_enabled;
    PlanetStore *store;
    uint64_t revision, surface_revision;
    char error[192];
} Planet;
typedef struct {
    double latitude_deg, longitude_deg, altitude_m;
} PlanetGeo;
typedef struct {
    bool hit;
    double distance_m;
    DVec3 point_m;
    Vec3 normal, brush_normal;
    int64_t mm[3];
    Voxel value;
} PlanetHit;
typedef struct {
    WorldPosition position;
    DVec3 velocity_mps;
    bool grounded, jump_pending;
    double accumulator;
} PlanetBody;
typedef struct {
    int face, level;
    uint32_t x, y;
    bool edited, visible, reset;
    CellCoord edit_key, origin;
    DVec3 center_m;
    double radius_m, span_m;
    uint64_t last_visible, built_revision;
    uint64_t built_surface_revision;
    bool ready;
    int edit_lod_mm, built_edit_lod_mm;
    int river_lod, built_river_lod;
    int terrain_lod, built_terrain_lod;
} PlanetPatch;
typedef struct {
    PlanetPatch patches[PLANET_PATCH_LIMIT];
    uint64_t frame;
    size_t visible, pending;
    int viewport_height;
    double river_clearance_m, river_pixel_factor;
    bool river_disabled;
} PlanetScene;
DVec3 planet_cube_direction(int face, double u, double v);
DVec3 planet_surface_point(const Planet *p, DVec3 direction, bool ocean);
DVec3 pd_add(DVec3 a, DVec3 b);
DVec3 pd_sub(DVec3 a, DVec3 b);
DVec3 pd_scale(DVec3 a, double s);
double pd_dot(DVec3 a, DVec3 b);
double pd_length(DVec3 a);
DVec3 pd_normalize(DVec3 a);
DVec3 planet_position_m(WorldPosition p);
bool planet_set_position(DVec3 meters, WorldPosition *out);
bool planet_init(Planet *planet, const char *earth_file, const char *save_file, double mass_kg, double a_m,
                 double b_m);
void planet_destroy(Planet *planet);
PlanetGeo planet_geographic(const Planet *planet, DVec3 position_m);
DVec3 planet_from_geographic(const Planet *planet, double latitude_deg, double longitude_deg,
                             double altitude_m);
double planet_elevation(const Planet *planet, double latitude_deg, double longitude_deg);
double planet_measured_elevation(const Planet *planet, double latitude_deg, double longitude_deg);
DVec3 planet_surface_normal(const Planet *planet, DVec3 position_m);
uint16_t planet_surface_material(double latitude_deg, double longitude_deg, double height_m);
size_t planet_elevation_bytes(const Planet *planet);
DVec3 planet_up(const Planet *planet, DVec3 position_m);
DVec3 planet_gravity(const Planet *planet, DVec3 position_m);
DVec3 planet_sun_direction(void);
Voxel planet_base_voxel(const Planet *planet, DVec3 position_m);
Voxel planet_voxel(Planet *planet, int64_t x_mm, int64_t y_mm, int64_t z_mm);
bool planet_paint(Planet *planet, PlanetBox box, Voxel value);
/* Voxelized sphere inscribed in bounds; adaptive boundary on large strokes. */
bool planet_paint_sphere(Planet *planet, PlanetBox bounds, Voxel value);
bool planet_collect(Planet *planet, PlanetBox bounds, PlanetEdits *out);
size_t planet_edit_keys(Planet *planet, PlanetBox bounds, DVec3 camera_m, CellCoord *keys, size_t capacity);
void planet_edits_destroy(PlanetEdits *edits);
size_t planet_storage_bytes(const Planet *planet);
size_t planet_saved_chunks(const Planet *planet);
size_t planet_resident_chunks(const Planet *planet);
bool planet_checkpoint(Planet *planet);
PlanetHit planet_raycast(Planet *planet, DVec3 origin_m, DVec3 direction, double max_m);
PlanetHit planet_pick(Planet *planet, DVec3 origin_m, DVec3 direction, double max_m);
bool planet_brush(PlanetHit hit, int edge_mm, bool place, PlanetBox *out);
bool planet_sphere_brush(PlanetHit hit, int diameter_mm, bool place, PlanetBox *out);
double planet_view_distance(const Planet *planet, WorldPosition camera, double requested_m);
double planet_near_distance(const Planet *planet, WorldPosition camera);
void planet_body_step(Planet *planet, PlanetBody *body, DVec3 tangent_velocity, bool jump, double elapsed_s);
void planet_scene_init(PlanetScene *scene);
bool planet_scene_update(PlanetScene *scene, Planet *planet, WorldPosition camera, Vec3 direction, Vec3 up,
                         float aspect, double view_m);
bool planet_mesh_patch(Mesh *mesh, Planet *planet, const PlanetPatch *patch);
bool planet_mesh_edits(Mesh *mesh, Planet *planet, const PlanetPatch *patch);
bool planet_mesh_greedy_edits(Mesh *mesh, CellCoord origin, int lod_mm);
bool planet_mesh_smooth_edits(Mesh *mesh);
typedef struct {
    double lat[2], lon[2], half_width_m;
} PlanetRiverSegment;
bool planet_river_candidates(const Planet *planet, double lat, double lon, double radius_m, uint32_t **ids,
                             size_t *count);
bool planet_river_segment(const Planet *planet, uint32_t id, PlanetRiverSegment *out);
bool planet_terrain_open(Planet *planet, const char *base_directory);
void planet_terrain_close(Planet *planet);
size_t planet_detail_refresh(Planet *planet);
size_t planet_detail_count(const Planet *planet);
size_t planet_river_count(const Planet *planet);
double planet_river_max_width(const Planet *planet);
size_t planet_terrain_bytes(const Planet *planet);
const char *planet_detail_directory(const Planet *planet);
bool planet_detail_sample(const Planet *planet, double lat, double lon, double *height, bool *water);
bool planet_river_sample(const Planet *planet, double lat, double lon, double *center_lat, double *center_lon,
                         double *depth);
bool planet_water_elevation(const Planet *planet, double lat, double lon, double *height);
double planet_visible_elevation(const Planet *planet, double lat, double lon);
uint16_t planet_visible_material(const Planet *planet, double lat, double lon);
int planet_app_main(int argc, char **argv);
#endif
