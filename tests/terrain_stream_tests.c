#define _POSIX_C_SOURCE 200809L
#include "terrain_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        if (!(e)) {                                                                                          \
            fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #e);                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static bool poll(TerrainStream *s, Planet *p, PlanetGeo g) {
    bool loaded = false;
    for (int i = 0; i < 300 && s->process; i++) {
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
        loaded |= terrain_stream_step(s, p, g, .01);
    }
    CHECK(!s->process);
    return loaded;
}
int main(void) {
    char tmp[] = "/tmp/voxel-stream-XXXXXX";
    CHECK(mkdtemp(tmp));
    char data[512];
    snprintf(data, sizeof(data), "%s/base.edem", tmp);
    FILE *source = fopen(TEST_EARTH_DATA, "rb"), *target = fopen(data, "wb");
    CHECK(source && target);
    unsigned char bytes[4096];
    size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), source)))
        CHECK(fwrite(bytes, 1, n, target) == n);
    fclose(source);
    CHECK(fclose(target) == 0);
    Planet p;
    CHECK(planet_init(&p, data, ":memory:", EARTH_MASS_KG, EARTH_A_METERS, EARTH_B_METERS));
    TerrainStream s = {.enabled = true};
    PlanetGeo location = {30.05, 30.05, 120};
    CHECK(!terrain_stream_step(&s, &p, location, 3) && s.process > 0);
    uint64_t revision = p.surface_revision;
    CHECK(poll(&s, &p, location));
    double height;
    bool water;
    CHECK(planet_detail_count(&p) == 1 && p.surface_revision > revision);
    CHECK(planet_detail_sample(&p, 30.05, 30.05, &height, &water) && water && height == 100);
    CHECK(planet_visible_elevation(&p, 30.05, 30.05) == 100 && planet_elevation(&p, 30.05, 30.05) == 98);
    CHECK(!terrain_stream_step(&s, &p, location, 3) && !s.process);
    location.latitude_deg = 31.05;
    CHECK(!terrain_stream_step(&s, &p, location, 3) && s.process > 0);
    CHECK(!poll(&s, &p, location) && s.cooldown > 0 && planet_detail_count(&p) == 1);
    s.cooldown = 0;
    location.latitude_deg = 32.05;
    CHECK(!terrain_stream_step(&s, &p, location, 3) && s.process > 0);
    terrain_stream_stop(&s);
    CHECK(!s.process);
    s.cooldown = 0;
    location = (PlanetGeo){40.055, -104.045, 1402};
    CHECK(!terrain_stream_step(&s, &p, location, 3) && s.process > 0 && s.requested_fine);
    CHECK(poll(&s, &p, location));
    TerrainSourceInfo source_info;
    CHECK(planet_detail_source(&p, location.latitude_deg, location.longitude_deg, &source_info));
    CHECK(source_info.provider == TERRAIN_PROVIDER_USGS_3DEP && source_info.effective_resolution_m < 2);
    CHECK(!terrain_stream_step(&s, &p, location, 3) && !s.process);
    location.latitude_deg = 40.065;
    CHECK(!terrain_stream_step(&s, &p, location, 3) && s.process > 0 && s.requested_fine);
    CHECK(!poll(&s, &p, location));
    CHECK(!terrain_stream_step(&s, &p, location, 3) && !s.process); /* no source: remember the checked cell */
    planet_destroy(&p);
    char file[512];
    snprintf(file, sizeof(file), "%s/earth_detail/300_300.pdem", tmp);
    unlink(file);
    snprintf(file, sizeof(file), "%s/earth_detail/fine.pdem", tmp);
    unlink(file);
    snprintf(file, sizeof(file), "%s/earth_detail", tmp);
    rmdir(file);
    snprintf(file, sizeof(file), "%s/terrain-fetch.log", tmp);
    unlink(file);
    unlink(data);
    rmdir(tmp);
    puts("PASS: asynchronous terrain loading, surface invalidation, cached coverage, failed-download "
         "fallback and child cleanup");
    return 0;
}
