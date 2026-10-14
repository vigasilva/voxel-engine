#include "landscape.h"
#include <math.h>
#include <string.h>
#define PI 3.14159265358979323846
static double clamp(double n, double a, double b) {
    return fmax(a, fmin(b, n));
}
static double smooth(double n) {
    n = clamp(n, 0, 1);
    return n * n * (3 - 2 * n);
}
static double fade(double n) {
    return n * n * n * (n * (n * 6 - 15) + 10);
}
static uint32_t hash(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}
static double noise(DVec3 point, double scale, uint32_t seed) {
    double p[3] = {point.x / scale, point.y / scale, point.z / scale}, t[3];
    int64_t cell[3];
    for (int a = 0; a < 3; a++) {
        cell[a] = (int64_t)floor(p[a]);
        t[a] = fade(p[a] - cell[a]);
    }
    double value = 0;
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                uint32_t h = hash((uint32_t)(cell[0] + x) * 73856093u ^ (uint32_t)(cell[1] + y) * 19349663u ^
                                  (uint32_t)(cell[2] + z) * 83492791u ^ seed);
                value += (h / 2147483647.5 - 1) * (x ? t[0] : 1 - t[0]) * (y ? t[1] : 1 - t[1]) *
                         (z ? t[2] : 1 - t[2]);
            }
    return value;
}
static double ridge(DVec3 p, double scale, uint32_t seed) {
    double n = noise(p, scale, seed);
    /* Rounded absolute value retains sharp-looking ridges with finite gradients. */
    return 1 - sqrt(n * n + .0025);
}
LandscapeConfig landscape_default(void) {
    return (LandscapeConfig){.seed = 0x45a91e37u, .roughness_scale = 1, .moisture_scale = 1};
}
bool landscape_config_valid(const LandscapeConfig *c) {
    return c && isfinite(c->roughness_scale) && c->roughness_scale >= 0 && c->roughness_scale <= 4 &&
           isfinite(c->moisture_scale) && c->moisture_scale >= 0 && c->moisture_scale <= 4 &&
           isfinite(c->temperature_offset_k) && fabs(c->temperature_offset_k) <= 80;
}
const char *biome_name(BiomeId id) {
    static const char *names[] = {"ocean",   "freshwater", "coast",    "desert", "grassland", "savanna",
                                  "forest",  "rainforest", "boreal",   "tundra", "alpine",    "ice",
                                  "wetland", "urban",      "cropland", "barren"};
    return id >= 0 && id < BIOME_COUNT ? names[id] : "auto";
}
BiomeId biome_find(const char *name) {
    if (name)
        for (int i = 0; i < BIOME_COUNT; i++)
            if (!strcmp(name, biome_name((BiomeId)i)))
                return (BiomeId)i;
    return BIOME_AUTO;
}
static BiomeId classify(const LandscapeInput *in, double temperature, double rain) {
    if (in->wet || in->cover == COVER_WATER)
        return in->marine ? BIOME_OCEAN : BIOME_FRESHWATER;
    switch (in->cover) {
    case COVER_SNOW:
        return BIOME_ICE;
    case COVER_WETLAND:
    case COVER_MANGROVE:
        return BIOME_WETLAND;
    case COVER_BUILT:
        return BIOME_URBAN;
    case COVER_CROP:
        return BIOME_CROPLAND;
    case COVER_MOSS:
        return BIOME_TUNDRA;
    case COVER_TREE:
        return temperature < 283 ? BIOME_BOREAL : temperature > 294 ? BIOME_RAINFOREST : BIOME_FOREST;
    case COVER_SHRUB:
        return rain < 350 ? BIOME_DESERT : BIOME_SAVANNA;
    case COVER_GRASS:
        return temperature > 295 && rain < 1000 ? BIOME_SAVANNA : BIOME_GRASSLAND;
    case COVER_BARE:
        return in->height_m > 2200 ? BIOME_ALPINE : rain < 350 ? BIOME_DESERT : BIOME_BARREN;
    default:
        break;
    }
    if (in->requested >= 0 && in->requested < BIOME_COUNT)
        return in->requested;
    if (temperature < 255)
        return BIOME_ICE;
    if (temperature < 270)
        return BIOME_TUNDRA;
    if (in->height_m > 2800 && (temperature < 284 || in->slope > .35))
        return BIOME_ALPINE;
    if (in->height_m >= 0 && in->height_m < 8)
        return BIOME_COAST;
    if (rain < 280)
        return BIOME_DESERT;
    if (temperature > 294 && rain > 1450)
        return BIOME_RAINFOREST;
    if (rain > 750)
        return temperature < 283 ? BIOME_BOREAL : BIOME_FOREST;
    return temperature > 293 ? BIOME_SAVANNA : BIOME_GRASSLAND;
}
LandscapeSample landscape_sample(const LandscapeConfig *config, LandscapeInput in, bool detail) {
    LandscapeSample out = {.biome = BIOME_BARREN,
                           .surface_material = MAT_STONE,
                           .subsoil_material = MAT_SOIL,
                           .bedrock_material = MAT_STONE,
                           .appearance_material = MAT_STONE,
                           .temperature_k = 288.15};
    if (!landscape_config_valid(config) || !isfinite(in.point_m.x) || !isfinite(in.point_m.y) ||
        !isfinite(in.point_m.z) || fabs(in.point_m.x) > 1e12 || fabs(in.point_m.y) > 1e12 ||
        fabs(in.point_m.z) > 1e12 || !isfinite(in.latitude_deg) || !isfinite(in.height_m) ||
        !isfinite(in.slope) || !isfinite(in.source_spacing_m) || in.source_spacing_m <= 0)
        return out;
    double latitude = fabs(in.latitude_deg), s = sin(clamp(latitude, 0, 90) * PI / 180), s2 = s * s;
    double continental = noise(in.point_m, 450000, config->seed ^ 0xaea167d3u);
    double rain = 300 + 2200 * exp(-pow(latitude / 20, 2)) - 700 * exp(-pow((latitude - 26) / 9, 2)) +
                  1000 * exp(-pow((latitude - 46) / 15, 2));
    rain = fmax(40, rain) * clamp(.8 + continental * .65, .2, 1.6) * config->moisture_scale;
    double temperature = 303.15 - 35 * s2 - 10 * s2 * s2 * s2 * s2 - .0065 * fmax(0, in.height_m) +
                         continental * 3 + config->temperature_offset_k;
    out.temperature_k = clamp(temperature, 150, 350);
    out.precipitation_mm = rain;
    out.biome = classify(&in, out.temperature_k, rain);
    out.measured_cover = in.cover != COVER_UNKNOWN;
    out.source_spacing_m = in.source_spacing_m;
    double geology = noise(in.point_m, 220000, config->seed ^ 0x612847edu);
    out.bedrock_material = in.height_m < 250 && in.slope < .15 ? MAT_SANDSTONE
                           : geology < -.25                    ? MAT_BASALT
                           : geology > .30                     ? MAT_LIMESTONE
                                                               : MAT_GRANITE;
    out.surface_material = MAT_GRASS;
    out.appearance_material = MAT_GRASS;
    double roughness = .8;
    switch (out.biome) {
    case BIOME_OCEAN:
        out.surface_material = out.appearance_material = MAT_SALT_WATER;
        out.subsoil_material = MAT_SILT;
        roughness = 0;
        break;
    case BIOME_FRESHWATER:
        out.surface_material = out.appearance_material = MAT_WATER;
        out.subsoil_material = MAT_MUD;
        roughness = 0;
        break;
    case BIOME_COAST:
        out.surface_material = out.appearance_material = MAT_SAND;
        out.subsoil_material = MAT_SAND;
        roughness = .4;
        break;
    case BIOME_DESERT:
        out.surface_material = out.appearance_material = MAT_SAND;
        out.subsoil_material = MAT_SAND;
        roughness = .5;
        break;
    case BIOME_GRASSLAND:
        out.vegetation = .65;
        roughness = .65;
        break;
    case BIOME_SAVANNA:
        out.surface_material = out.appearance_material = MAT_DRY_GRASS;
        out.vegetation = .35;
        roughness = .6;
        break;
    case BIOME_FOREST:
    case BIOME_RAINFOREST:
    case BIOME_BOREAL:
        out.surface_material = MAT_FOREST_LITTER;
        out.appearance_material = out.biome == BIOME_BOREAL ? MAT_CONIFER_FOLIAGE : MAT_FOLIAGE;
        out.vegetation = out.biome == BIOME_RAINFOREST ? .95 : .80;
        roughness = .5;
        break;
    case BIOME_TUNDRA:
        out.surface_material = out.appearance_material = MAT_TUNDRA_SOIL;
        out.vegetation = .25;
        roughness = .8;
        break;
    case BIOME_ALPINE:
        out.surface_material = out.appearance_material = out.bedrock_material;
        out.subsoil_material = MAT_GRAVEL;
        roughness = 1.6;
        break;
    case BIOME_ICE:
        out.surface_material = out.appearance_material = MAT_SNOW;
        out.subsoil_material = MAT_ICE;
        out.temperature_k = fmin(268, out.temperature_k);
        roughness = .35;
        break;
    case BIOME_WETLAND:
        out.surface_material = MAT_MUD;
        out.appearance_material = MAT_WETLAND_COVER;
        out.subsoil_material = MAT_CLAY;
        out.vegetation = .8;
        roughness = .25;
        break;
    case BIOME_URBAN:
        out.surface_material = out.appearance_material = MAT_GRAVEL;
        out.subsoil_material = MAT_SOIL;
        roughness = .05;
        break;
    case BIOME_CROPLAND:
        out.surface_material = out.appearance_material = MAT_CROP_SOIL;
        out.vegetation = .6;
        roughness = .2;
        break;
    default:
        out.surface_material = out.appearance_material = out.bedrock_material;
        roughness = 1.2;
        break;
    }
    if (in.slope > .75 && out.biome != BIOME_ICE && out.biome != BIOME_OCEAN &&
        out.biome != BIOME_FRESHWATER && out.biome != BIOME_URBAN) {
        out.surface_material = out.appearance_material = out.bedrock_material;
        out.vegetation *= .2;
    }
    if (!detail || roughness == 0 || config->roughness_scale == 0)
        return out;
    /* Only unresolved bands receive synthetic relief. Fine sources smoothly
       suppress coarse invented ridges; this also avoids quality-boundary pops. */
    double unresolved = clamp(in.source_spacing_m, .001, 20000),
           mountain = smooth(in.slope / .55) * .7 + smooth((in.height_m - 1300) / 2200) * .3;
    double amplitude = fmin(140, unresolved * .025) * roughness * (.12 + mountain);
    double large = clamp(unresolved * .3, 2, 1400), medium = large * .35;
    double ridges = (ridge(in.point_m, large, config->seed) - .55) * .7 +
                    (ridge(in.point_m, medium, config->seed ^ 0x1b05e12u) - .55) * .3;
    double relief = amplitude * ridges;
    if (out.biome == BIOME_DESERT) {
        /* Wind-aligned dunes with slowly wandering crests, not isotropic rocky bumps. */
        double wind = noise(in.point_m, 350, config->seed ^ 0x58310u);
        double phase =
            (in.point_m.x * .78 + in.point_m.y * .35 + in.point_m.z * .51) / fmax(12, large * .25) + wind;
        relief += fmin(3, unresolved * .008) * sin(phase * 2 * PI) * roughness;
    }
    const double scales[] = {4, 1, .25, .0625}, amps[] = {.035, .012, .004, .001};
    for (int i = 0; i < 4; i++) {
        double weight = smooth(unresolved / (scales[i] * 2));
        relief +=
            noise(in.point_m, scales[i], config->seed + (uint32_t)i * 7919u) * amps[i] * roughness * weight;
    }
    /* Suppress coastal coastline shifts; river/lake bands are separately graded. */
    out.detail_m = relief * smooth(fabs(in.height_m) / 20) * config->roughness_scale;
    return out;
}
double landscape_generate_height(const LandscapeConfig *c, BiomeId biome, DVec3 p, double amplitude) {
    if (!landscape_config_valid(c) || biome < 0 || biome >= BIOME_COUNT || !isfinite(amplitude) ||
        amplitude < 0 || amplitude > 10000 || !isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
        fabs(p.x) > 1e12 || fabs(p.y) > 1e12 || fabs(p.z) > 1e12)
        return NAN;
    double warp = noise(p, 1600, c->seed ^ 0x736bd1u) * 240;
    DVec3 q = dv3(p.x + warp, p.y - warp * .3, p.z + warp * .6);
    double broad = noise(q, 2400, c->seed), fine = noise(q, 350, c->seed ^ 0x6aed1u);
    switch (biome) {
    case BIOME_ALPINE:
    case BIOME_BARREN: {
        double mountains = (ridge(q, 1200, c->seed) - .45) * .65 +
                           (ridge(q, 450, c->seed ^ 0x514d3u) - .45) * .25 +
                           (ridge(q, 150, c->seed ^ 0x776d9u) - .45) * .10;
        /* Connected low channels cut between correlated crests; no claim of a
           hydraulic erosion simulation or surveyed mountain morphology. */
        double channel = smooth((.13 - fabs(noise(q, 1900, c->seed ^ 0x715cdu))) / .13);
        return amplitude * (mountains - channel * .10);
    }
    case BIOME_DESERT:
        return amplitude * (broad * .12 + sin(q.x / 95 + fine * 2) * .025 + fine * .035);
    case BIOME_COAST:
        return amplitude * (broad * .08 + fine * .015) - amplitude * .025;
    case BIOME_OCEAN:
        return -fmax(1, amplitude * (.5 + broad * .2));
    case BIOME_FRESHWATER:
    case BIOME_WETLAND:
        return amplitude * (broad * .025 + fine * .005);
    case BIOME_ICE:
        return amplitude * (broad * .30 + fine * .025);
    default:
        return amplitude * (broad * .25 + fine * .035);
    }
}
