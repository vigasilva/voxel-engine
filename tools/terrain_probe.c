#include "planet.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: terrain_probe EARTH_DATA LAT LON\n");
        return 1;
    }
    char *end;
    double lat = strtod(argv[2], &end);
    if (*end || !isfinite(lat) || fabs(lat) > 90)
        return 1;
    double lon = strtod(argv[3], &end);
    if (*end || !isfinite(lon))
        return 1;
    Planet p;
    if (!planet_init(&p, argv[1], ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS)) {
        fprintf(stderr, "%s\n", p.error);
        planet_destroy(&p);
        return 1;
    }
    TerrainSourceInfo source;
    double measured = planet_measured_elevation(&p, lat, lon);
    printf("Measured height: %.3f m; rendered ground: %.3f m\n", measured, planet_elevation(&p, lat, lon));
    if (planet_detail_source(&p, lat, lon, &source)) {
        printf("Provider: %s / %s\n", terrain_provider_name(source.provider), source.label);
        printf("Tile: %s\nGrid: %.4f x %.4f m; effective resolution %.4f m\n", source.tile_name,
               source.grid_spacing_x_m, source.grid_spacing_y_m, source.effective_resolution_m);
        printf("Vertical datum: %s -> %s%s\n", height_datum_name(source.source_datum),
               height_datum_name(source.datum), source.legacy ? " (legacy assumption)" : "");
        printf("Publication day: %u; source horizontal EPSG %u; physical body %u\n", source.publication_day,
               source.source_epsg, source.body_id);
        if (isfinite(source.vertical_accuracy_m))
            printf("Declared vertical accuracy: %.3f m\n", source.vertical_accuracy_m);
        else
            puts("Vertical accuracy: unknown; sampling resolution is not accuracy");
        if (isfinite(source.transform_accuracy_m))
            printf("Declared transformation accuracy: %.3f m\n", source.transform_accuracy_m);
        else
            puts("Transformation accuracy: unknown");
        printf("Core coverage: %.8f %.8f %.8f %.8f\n", source.coverage[0], source.coverage[1],
               source.coverage[2], source.coverage[3]);
    } else
        puts("Source: global/regional fallback");
    planet_destroy(&p);
    return 0;
}
