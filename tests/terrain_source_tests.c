#define _POSIX_C_SOURCE 200809L
#include "planet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #x);                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void put32(unsigned char *p, uint32_t n) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}
static void put64(unsigned char *p, double x) {
    uint64_t bits;
    memcpy(&bits, &x, 8);
    for (int i = 0; i < 8; i++)
        p[i] = (unsigned char)(bits >> (8 * i));
}
static void fixture(const char *path, unsigned version, int size, double origin, double step, int cm,
                    double spacing, TerrainSurfaceKind kind, unsigned date, unsigned cover, bool missing,
                    bool bad_datum) {
    size_t offset = version == 3 ? 256 : 64, bytes = offset + (size_t)size * size * 4 + 4;
    unsigned char *data = calloc(bytes, 1);
    CHECK(data);
    memcpy(data, "PDEM", 4);
    put32(data + 4, version);
    put32(data + 8, size);
    put32(data + 12, size);
    put32(data + 16, 2);
    put32(data + 20, 2);
    put64(data + 24, origin);
    put64(data + 32, origin);
    put64(data + 40, step);
    put64(data + 48, step);
    if (version == 3) {
        put32(data + 60, 256);
        put32(data + 64, TERRAIN_PROVIDER_USGS_3DEP);
        put32(data + 68, kind);
        put32(data + 72, HEIGHT_NAVD88);
        put32(data + 76, bad_datum ? HEIGHT_NAVD88 : HEIGHT_EGM2008);
        put64(data + 80, spacing);
        put64(data + 88, NAN);
        put32(data + 96, date);
        put32(data + 100, TERRAIN_DATUM_CONVERTED);
        memcpy(data + 104, "Survey fixture", 14);
        put64(data + 184, origin);
        put64(data + 192, origin);
        put64(data + 200, origin + (size - 1) * step);
        put64(data + 208, origin + (size - 1) * step);
        put32(data + 220, 399);
        put32(data + 228, 6342);
        put64(data + 232, 1.2);
        put64(data + 240, spacing);
        uint32_t crc = (uint32_t)crc32(0, data, 56);
        crc = (uint32_t)crc32(crc, data + 60, 164);
        crc = (uint32_t)crc32(crc, data + 228, 28);
        put32(data + 224, crc);
    }
    for (int i = 0; i < size * size; i++)
        put32(data + offset + i * 4, (uint32_t)cm);
    if (missing)
        put32(data + offset + (10 * size + 10) * 4, 0x80000000u);
    memset(data + offset + (size_t)size * size * 4, cover, 4);
    put32(data + 56, (uint32_t)crc32(0, data + 64, (uInt)(bytes - 64)));
    char temporary[600];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *f = fopen(temporary, "wb");
    CHECK(f && fwrite(data, 1, bytes, f) == bytes && !fclose(f));
    CHECK(!rename(temporary, path));
    free(data);
}
static void set_core(const char *path, double low, double high) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    CHECK(n > 256);
    rewind(f);
    unsigned char *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    put64(data + 184, low);
    put64(data + 192, low);
    put64(data + 200, high);
    put64(data + 208, high);
    uint32_t crc = (uint32_t)crc32(0, data, 56);
    crc = (uint32_t)crc32(crc, data + 60, 164);
    crc = (uint32_t)crc32(crc, data + 228, 28);
    put32(data + 224, crc);
    put32(data + 56, (uint32_t)crc32(0, data + 64, (uInt)n - 64));
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    CHECK(f && fwrite(data, 1, (size_t)n, f) == (size_t)n && !fclose(f));
    CHECK(!rename(tmp, path));
    free(data);
}
int main(void) {
    char root[] = "/tmp/voxel-sources-XXXXXX";
    CHECK(mkdtemp(root));
    char directory[512], base[512], coarse[512], ground[512], surface[512], upsampled[512];
    snprintf(base, sizeof(base), "%s/", root);
    snprintf(directory, sizeof(directory), "%s/earth_detail", root);
    CHECK(!mkdir(directory, 0700));
    snprintf(coarse, sizeof(coarse), "%s/earth_detail/coarse.pdem", root);
    snprintf(ground, sizeof(ground), "%s/earth_detail/zzz-ground.pdem", root);
    snprintf(surface, sizeof(surface), "%s/earth_detail/aaa-surface.pdem", root);
    snprintf(upsampled, sizeof(upsampled), "%s/earth_detail/fake-fine.pdem", root);
    fixture(coarse, 2, 451, 10, .001, 10000, 0, TERRAIN_SURFACE_MODEL, 0, COVER_TREE, false, false);
    fixture(surface, 3, 31, 10.05, .00001, 25000, 1, TERRAIN_SURFACE_MODEL, 20250101, 0, false, false);
    fixture(ground, 3, 31, 10.05, .00001, 20000, 1, TERRAIN_BARE_GROUND, 20190101, 0, false, false);
    fixture(upsampled, 3, 301, 10.05, .000001, 99000, 1000, TERRAIN_BARE_GROUND, 20260101, 0, false, false);
    Planet p;
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    planet_terrain_close(&p);
    CHECK(planet_terrain_open(&p, base));
    CHECK(planet_detail_count(&p) == 4);
    double height, lat = 10.0501;
    CHECK(planet_detail_sample(&p, 10.001, 10.001, &height, NULL) && height == 100);
    CHECK(planet_detail_sample(&p, 10.449, 10.449, &height, NULL) &&
          height == 100); /* beyond old center-bin lookup */
    CHECK(planet_detail_sample(&p, lat, lat, &height, NULL) && height == 200);
    TerrainSourceInfo info;
    CHECK(planet_detail_source(&p, lat, lat, &info));
    CHECK(info.provider == TERRAIN_PROVIDER_USGS_3DEP && info.kind == TERRAIN_BARE_GROUND && !info.legacy);
    CHECK(info.datum == HEIGHT_EGM2008 && info.source_datum == HEIGHT_NAVD88 &&
          info.flags & TERRAIN_DATUM_CONVERTED);
    CHECK(info.publication_day == 20190101 && info.source_epsg == 6342 && info.transform_accuracy_m == 1.2);
    CHECK(isnan(info.vertical_accuracy_m) && info.effective_resolution_m > 1 &&
          info.effective_resolution_m < 2);
    LandCover cover;
    CHECK(planet_detail_land_cover(&p, lat, lat, &cover) && cover == COVER_TREE);
    bool water = true;
    CHECK(planet_detail_water_mask(&p, lat, lat, &water) && !water); /* unknown fine classes fall back */
    uint64_t revision = p.surface_revision;
    fixture(ground, 3, 31, 10.05, .00001, 20000, 1, TERRAIN_BARE_GROUND, 20190101, 0, true, false);
    CHECK(planet_detail_refresh(&p) == 1 && p.surface_revision > revision);
    CHECK(planet_detail_sample(&p, lat, lat, &height, NULL) &&
          height == 250); /* four-corner nodata fallback */
    fixture(surface, 3, 31, 10.05, .00001, 26000, 1, TERRAIN_SURFACE_MODEL, 20250101, 0, true, false);
    CHECK(planet_detail_refresh(&p) == 1);
    CHECK(planet_detail_sample(&p, lat, lat, &height, NULL) &&
          height == 100); /* don't prefer an upsampled coarse source */
    fixture(ground, 3, 31, 10.05, .00001, 22000, 1, TERRAIN_BARE_GROUND, 20260101, COVER_GRASS, false, false);
    CHECK(planet_detail_refresh(&p) == 1);
    CHECK(planet_detail_sample(&p, lat, lat, &height, NULL) && height == 220);
    CHECK(planet_detail_land_cover(&p, lat, lat, &cover) && cover == COVER_GRASS);
    set_core(ground, 10.05008, 10.05022);
    CHECK(planet_detail_refresh(&p) == 1);
    double halo = 10.05004, blended;
    CHECK(planet_detail_sample(&p, halo, halo, &height, NULL) && height == 220);
    CHECK(planet_detail_elevation(&p, halo, halo, 0, &blended) && fabs(blended - 240) < .0001);
    CHECK(planet_detail_source(&p, halo, halo, &info) && fabs(info.blend_weight - .5) < .00001);
    CHECK(planet_detail_elevation(&p, lat, lat, 0, &blended) && blended == 220);
    revision = p.surface_revision;
    fixture(ground, 3, 31, 10.05, .00001, 50000, 1, TERRAIN_BARE_GROUND, 20260101, COVER_GRASS, false, true);
    CHECK(planet_detail_refresh(&p) == 0 && p.surface_revision == revision);
    CHECK(planet_detail_sample(&p, lat, lat, &height, NULL) &&
          height == 220); /* reject incompatible datum replacement */
    planet_destroy(&p);
    unlink(coarse);
    unlink(ground);
    unlink(surface);
    unlink(upsampled);
    rmdir(directory);
    rmdir(root);
    CHECK(planet_init(&p, TEST_EARTH_DATA, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    CHECK(planet_detail_source(&p, 78.2232, 15.6469, &info));
    CHECK(info.provider == TERRAIN_PROVIDER_ARCTICDEM && info.kind == TERRAIN_SURFACE_MODEL &&
          info.source_datum == HEIGHT_ELLIPSOID && info.datum == HEIGHT_EGM2008 &&
          info.source_epsg == 3413 && info.flags & TERRAIN_DATUM_CONVERTED &&
          info.effective_resolution_m >= 2 && info.effective_resolution_m < 2.2 &&
          isnan(info.vertical_accuracy_m));
    CHECK(planet_detail_sample(&p, 78.2232, 15.6469, &height, NULL) && fabs(height - 7.767) < .01);
    CHECK(planet_detail_source(&p, -77.835, 166.675, &info));
    CHECK(info.provider == TERRAIN_PROVIDER_REMA && info.kind == TERRAIN_SURFACE_MODEL &&
          info.source_datum == HEIGHT_ELLIPSOID && info.datum == HEIGHT_EGM2008 &&
          info.source_epsg == 3031 && info.flags & TERRAIN_DATUM_CONVERTED);
    CHECK(planet_detail_sample(&p, -77.835, 166.675, &height, NULL) && fabs(height - 138) < .01);
    CHECK(!planet_detail_land_cover(&p, -77.835, 166.675, &cover)); /* unknown classification is not dry data */
    planet_destroy(&p);
    puts("PASS: variable coverage bins, finest valid source, ground/model priority, truthful resolution, "
         "provenance, nodata/class fallback and validated refresh");
}
