#ifndef LANDSCAPE_H
#define LANDSCAPE_H
#include "material.h"
#include "position.h"
#include "landcover.h"
typedef enum {
    BIOME_AUTO = -1,
    BIOME_OCEAN,
    BIOME_FRESHWATER,
    BIOME_COAST,
    BIOME_DESERT,
    BIOME_GRASSLAND,
    BIOME_SAVANNA,
    BIOME_FOREST,
    BIOME_RAINFOREST,
    BIOME_BOREAL,
    BIOME_TUNDRA,
    BIOME_ALPINE,
    BIOME_ICE,
    BIOME_WETLAND,
    BIOME_URBAN,
    BIOME_CROPLAND,
    BIOME_BARREN,
    BIOME_COUNT
} BiomeId;
typedef struct {
    uint32_t seed;
    double roughness_scale, moisture_scale, temperature_offset_k;
} LandscapeConfig;
typedef struct {
    DVec3 point_m;
    double latitude_deg, height_m, slope, source_spacing_m;
    LandCover cover;
    bool wet, marine;
    BiomeId requested;
} LandscapeInput;
typedef struct {
    BiomeId biome;
    uint16_t surface_material, subsoil_material, bedrock_material, appearance_material;
    double temperature_k, precipitation_mm, vegetation, detail_m, source_spacing_m;
    bool measured_cover;
} LandscapeSample;
LandscapeConfig landscape_default(void);
bool landscape_config_valid(const LandscapeConfig *config);
const char *biome_name(BiomeId biome);
BiomeId biome_find(const char *name);
/* Stateless sampling in body-local meters; no allocated voxel array. Surface
   appearance may include canopy coverage while ground matter remains soil. */
LandscapeSample landscape_sample(const LandscapeConfig *config, LandscapeInput input, bool detail);
/* Entirely synthetic base landforms for independent worlds/tools, never survey data. */
double landscape_generate_height(const LandscapeConfig *config, BiomeId biome, DVec3 point_m,
                                 double amplitude_m);
#endif
