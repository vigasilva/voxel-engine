#include "planet.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 3)
        return 1;
    Planet p;
    if (!planet_init(&p, argv[1], argv[2], EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS))
        return 1;
    int64_t base = (int64_t)EARTH_A_METERS * 1000;
    PlanetBox front = {{base + 6000, -500, -500}, {base + 6100, 500, 500}};
    PlanetBox behind = {{base + 3000, -250, -250}, {base + 3100, 250, 250}};
    bool ok = planet_paint(&p, front, (Voxel){MAT_COPPER, 0, 300}) &&
              planet_paint(&p, behind, (Voxel){MAT_IRON, 0, 300});
    if (!ok)
        fprintf(stderr, "Seed: %s\n", p.error);
    double spawn = 10 - planet_elevation(&p, 0, 0);
    printf("%.9f\n", spawn);
    planet_destroy(&p);
    return ok ? 0 : 1;
}
