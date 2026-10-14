#define _POSIX_C_SOURCE 200809L
#include "planet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#define CHECK(v)                                                                                             \
    do {                                                                                                     \
        if (!(v)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void put32(unsigned char *p, uint32_t value) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(value >> (8 * i));
}
static void put64(unsigned char *p, double value) {
    uint64_t bits;
    memcpy(&bits, &value, 8);
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(bits >> (8 * i));
}
static void fixture(const char *path, double head) {
    enum { N = 64, BYTES = 64 + N * N * 5 };
    unsigned char data[BYTES];
    memset(data, 0, sizeof(data));
    memcpy(data, "PDEM", 4);
    put32(data + 4, 1);
    for (int i = 8; i <= 20; i += 4)
        put32(data + i, N);
    put64(data + 24, 1);
    put64(data + 32, 1);
    put64(data + 40, .00009);
    put64(data + 48, .00009);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            double r = hypot(x - 32, y - 32);
            double height = head + fmax(0, r - 12) * 3;
            put32(data + 64 + (y * N + x) * 4, (uint32_t)(int32_t)llround(height * 100));
            data[64 + N * N * 4 + y * N + x] = r < 18;
        }
    put32(data + 56, (uint32_t)crc32(0, data + 64, BYTES - 64));
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(data, 1, BYTES, f) == BYTES && !fclose(f));
}
static void check_lake(Planet *p, double head) {
    double lat = 1 + 32 * .00009, lon = lat, level, bank_level;
    CHECK(planet_water_elevation(p, lat, lon, &level) && fabs(level - head) < .01);
    CHECK(planet_water_elevation(p, lat, lon + 17 * .00009, &bank_level));
    CHECK(fabs(bank_level - level) < 1e-8);
    CHECK(fabs(planet_elevation(p, lat, lon) - (head - 2)) < .01);
    double edge = planet_elevation(p, lat, lon + 17 * .00009);
    CHECK(edge > head - 2 && edge <= head);
    CHECK(!planet_water_elevation(p, lat, lon + 20 * .00009, NULL));
    CHECK(planet_visible_material(p, lat, lon + 20 * .00009) != MAT_WATER);
    DVec3 eye = planet_from_geographic(p, lat, lon, level + 1);
    PlanetHit pick = planet_pick(p, eye, pd_scale(planet_up(p, eye), -1), 5);
    CHECK(pick.hit && pick.value.material == MAT_WATER && fabs(pick.distance_m - 1) < .005);
    DVec3 land =
        planet_from_geographic(p, lat, lon + 20 * .00009, planet_elevation(p, lat, lon + 20 * .00009) + 1);
    CHECK(planet_base_voxel(p, land).material == MAT_AIR);
}
int main(void) {
    char root[] = "/tmp/voxel-hydrology-XXXXXX";
    CHECK(mkdtemp(root));
    char directory[256], path[320], cache[340], base[256];
    snprintf(base, sizeof(base), "%s/", root);
    snprintf(directory, sizeof(directory), "%s/earth_detail", root);
    CHECK(!mkdir(directory, 0700));
    snprintf(path, sizeof(path), "%s/fixture.pdem", directory);
    snprintf(cache, sizeof(cache), "%s.hydr", path);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    p.relief_enabled = false;
    for (int negative = 0; negative < 2; negative++) {
        planet_terrain_close(&p);
        fixture(path, negative ? -400 : 100);
        CHECK(planet_terrain_open(&p, base));
        check_lake(&p, negative ? -400 : 100);
        CHECK(!access(cache, R_OK));
        planet_terrain_close(&p);
        CHECK(planet_terrain_open(&p, base));
        check_lake(&p, negative ? -400 : 100); /* Read-only mapped cache reuse. */
    }
    planet_destroy(&p);
    CHECK(!remove(cache) && !remove(path) && !rmdir(directory) && !rmdir(root));
    puts("PASS: native level lakes, gradual beds, below-sea-level dry land/water, picking, cached profiles");
    return 0;
}
