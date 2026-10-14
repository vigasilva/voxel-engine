#include "landscape.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(v)                                                                                             \
    do {                                                                                                     \
        if (!(v)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
int main(void) {
    LandscapeConfig c = landscape_default();
    LandscapeInput in = {.point_m = {4051200, -3500112, 2215100},
                         .latitude_deg = 10,
                         .height_m = 100,
                         .source_spacing_m = 30,
                         .slope = .1,
                         .cover = COVER_TREE,
                         .requested = BIOME_AUTO};
    LandscapeSample forest = landscape_sample(&c, in, true);
    CHECK(forest.measured_cover && forest.biome == BIOME_RAINFOREST);
    CHECK(forest.surface_material == MAT_FOREST_LITTER && forest.appearance_material == MAT_FOLIAGE);
    CHECK(material_get(forest.surface_material)->solid && forest.temperature_k > 294);
    in.latitude_deg = 60;
    LandscapeSample boreal = landscape_sample(&c, in, true);
    CHECK(boreal.biome == BIOME_BOREAL && boreal.appearance_material == MAT_CONIFER_FOLIAGE);
    CHECK(boreal.temperature_k < forest.temperature_k - 20);
    in.cover = COVER_SNOW;
    LandscapeSample ice = landscape_sample(&c, in, true);
    CHECK(ice.biome == BIOME_ICE && ice.surface_material == MAT_SNOW && ice.subsoil_material == MAT_ICE &&
          ice.temperature_k <= 268);
    in.cover = COVER_WETLAND;
    LandscapeSample wetland = landscape_sample(&c, in, true);
    CHECK(wetland.biome == BIOME_WETLAND && wetland.surface_material == MAT_MUD &&
          wetland.appearance_material == MAT_WETLAND_COVER);
    in.cover = COVER_CROP;
    CHECK(landscape_sample(&c, in, true).surface_material == MAT_CROP_SOIL);
    in.cover = COVER_BUILT;
    CHECK(landscape_sample(&c, in, true).biome == BIOME_URBAN);
    in.cover = COVER_GRASS;
    in.requested = BIOME_DESERT;
    CHECK(landscape_sample(&c, in, true).biome == BIOME_GRASSLAND); /* Measured cover takes precedence. */
    in.height_m = 3000;
    LandscapeSample high = landscape_sample(&c, in, false);
    in.height_m = 100;
    CHECK(landscape_sample(&c, in, false).temperature_k > high.temperature_k + 10);
    in.cover = COVER_UNKNOWN;
    in.requested = BIOME_ALPINE;
    in.slope = .9;
    LandscapeSample rock = landscape_sample(&c, in, true);
    CHECK(rock.surface_material == rock.bedrock_material &&
          material_get(rock.bedrock_material)->family == FAMILY_ROCK);
    in.wet = true;
    in.marine = false;
    CHECK(landscape_sample(&c, in, true).surface_material == MAT_WATER &&
          landscape_sample(&c, in, true).detail_m == 0);
    in.marine = true;
    CHECK(landscape_sample(&c, in, true).surface_material == MAT_SALT_WATER);
    in.wet = in.marine = false;
    in.height_m = 0;
    CHECK(landscape_sample(&c, in, true).detail_m == 0); /* Coastline datum is preserved. */
    in.height_m = 3000;
    in.source_spacing_m = 9000;
    LandscapeSample once = landscape_sample(&c, in, true), again = landscape_sample(&c, in, true);
    CHECK(once.detail_m == again.detail_m && once.bedrock_material == again.bedrock_material);
    c.roughness_scale = 2;
    CHECK(fabs(landscape_sample(&c, in, true).detail_m - 2 * once.detail_m) < 1e-12);
    c.roughness_scale = 0;
    CHECK(landscape_sample(&c, in, true).detail_m == 0);
    c = landscape_default();
    double coarse_energy = 0, fine_energy = 0, coarse_min = HUGE_VAL, coarse_max = -HUGE_VAL;
    for (int i = 0; i < 512; i++) {
        in.point_m.x += 11;
        in.point_m.z += 7;
        in.source_spacing_m = 9000;
        double coarse = landscape_sample(&c, in, true).detail_m;
        in.source_spacing_m = 1;
        double fine = landscape_sample(&c, in, true).detail_m;
        coarse_energy += coarse * coarse;
        fine_energy += fine * fine;
        coarse_min = fmin(coarse_min, coarse);
        coarse_max = fmax(coarse_max, coarse);
        in.source_spacing_m = .001;
        CHECK(fabs(landscape_sample(&c, in, true).detail_m) < .00025);
    }
    CHECK(coarse_energy > fine_energy * 100 && coarse_max - coarse_min > 5);
    DVec3 p = {400, 0, 600};
    double a = landscape_generate_height(&c, BIOME_ALPINE, p, 1200);
    CHECK(a == landscape_generate_height(&c, BIOME_ALPINE, p, 1200));
    c.seed++;
    CHECK(a != landscape_generate_height(&c, BIOME_ALPINE, p, 1200));
    c.roughness_scale = -1;
    CHECK(!landscape_config_valid(&c) && isnan(landscape_generate_height(&c, BIOME_ALPINE, p, 1200)));
    c = landscape_default();
    CHECK(isnan(landscape_generate_height(&c, BIOME_AUTO, p, 1200)));
    double peak_min = HUGE_VAL, peak_max = -HUGE_VAL, plain_min = HUGE_VAL, plain_max = -HUGE_VAL;
    for (int z = 0; z < 80; z++)
        for (int x = 0; x < 80; x++) {
            p = dv3(x * 50, 0, z * 50);
            double peak = landscape_generate_height(&c, BIOME_ALPINE, p, 1200),
                   plain = landscape_generate_height(&c, BIOME_GRASSLAND, p, 1200);
            peak_min = fmin(peak_min, peak);
            peak_max = fmax(peak_max, peak);
            plain_min = fmin(plain_min, plain);
            plain_max = fmax(plain_max, plain);
        }
    CHECK(peak_max - peak_min > (plain_max - plain_min) * 1.5);
    CHECK(biome_find("alpine") == BIOME_ALPINE && biome_find("invalid") == BIOME_AUTO);
    printf("PASS: measured biome precedence, canopy/ground matter, lapse/rock/ice states, source-resolution "
           "relief, deterministic landforms (%.1f m alpine / %.1f m grassland)\n",
           peak_max - peak_min, plain_max - plain_min);
    return 0;
}
