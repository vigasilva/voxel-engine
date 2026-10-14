#include "surface_texture.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: bake_surface_texture EARTH_DATA OUTPUT\n");
        return 1;
    }
    Planet p;
    if (!planet_init(&p, argv[1], ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS)) {
        fprintf(stderr, "Bake: %s\n", p.error);
        planet_destroy(&p);
        return 1;
    }
    bool ok = planet_surface_texture_bake(&p, argv[2]);
    planet_destroy(&p);
    if (ok)
        puts("Distant terrain atlas baked from engine elevation and materials");
    return ok ? 0 : 1;
}
