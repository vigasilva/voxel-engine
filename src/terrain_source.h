#ifndef TERRAIN_SOURCE_H
#define TERRAIN_SOURCE_H
#include <stdbool.h>
#include <stdint.h>
#define TERRAIN_SOURCE_HEADER_BYTES 256
#define TERRAIN_SOURCE_LABEL_BYTES 48
typedef enum {
    TERRAIN_PROVIDER_UNKNOWN,
    TERRAIN_PROVIDER_COPERNICUS,
    TERRAIN_PROVIDER_USGS_3DEP,
    TERRAIN_PROVIDER_ARCTICDEM,
    TERRAIN_PROVIDER_REMA,
    TERRAIN_PROVIDER_LOCAL,
    TERRAIN_PROVIDER_NASADEM
} TerrainProvider;
typedef enum {
    TERRAIN_SURFACE_UNKNOWN,
    TERRAIN_BARE_GROUND,
    TERRAIN_SURFACE_MODEL,
    TERRAIN_TOPOBATHY
} TerrainSurfaceKind;
typedef enum {
    HEIGHT_DATUM_UNKNOWN,
    HEIGHT_EGM2008,
    HEIGHT_NAVD88,
    HEIGHT_ELLIPSOID,
    HEIGHT_EGM1996
} HeightDatum;
enum { TERRAIN_DATUM_CONVERTED = 1, TERRAIN_ACCURACY_ESTIMATED = 2 };
typedef struct {
    TerrainProvider provider;
    TerrainSurfaceKind kind;
    HeightDatum source_datum, datum;
    double source_spacing_m, finest_spacing_m, vertical_accuracy_m, transform_accuracy_m;
    double grid_spacing_x_m, grid_spacing_y_m, effective_resolution_m, blend_weight;
    double coverage[4]; /* West, south, east, north: core coverage, excluding halo. */
    uint32_t publication_day, flags, source_epsg, body_id;
    char label[TERRAIN_SOURCE_LABEL_BYTES], tile_name[256];
    bool legacy;
} TerrainSourceInfo;
const char *terrain_provider_name(TerrainProvider provider);
const char *height_datum_name(HeightDatum datum);
#endif
