#include "hydrology.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v)                                                                                             \
    do {                                                                                                     \
        if (!(v)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
int main(void) {
    enum { W = 100, H = 100 };
    float elevation[W * H];
    uint8_t water[W * H];
    memset(water, 0, sizeof(water));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            double r = hypot(x - 50, y - 50);
            water[y * W + x] = r < 25;
            elevation[y * W + x] = r < 18 ? 100 : (float)(100 + (r - 18) * 3);
        }
    Hydrology lake = {0};
    CHECK(hydrology_build(&lake, elevation, water, W, H, 10, 10));
    CHECK(lake.body_count == 1);
    CHECK(hydrology_level(&lake, 50, 50) == 100);
    CHECK(hydrology_level(&lake, 72, 50) == 100);
    CHECK(lake.shore_m[50 * W + 50] > lake.shore_m[50 * W + 73]);
    CHECK(lake.shore_m[50 * W + 73] > 0 && lake.shore_m[50 * W + 77] < 0);
    CHECK(hydrology_bed(200, 100, 0, 2, 20) == 100);
    CHECK(hydrology_bed(200, 100, 20, 2, 20) == 98);
    double edge = hydrology_bed(200, 100, 2, 2, 20);
    CHECK(edge < 100 && edge > 99.9);
    CHECK(hydrology_bed(120, 100, -10, 2, 20) == 110);
    CHECK(hydrology_bed(120, 100, -20, 2, 20) == 120);
    CHECK(hydrology_save(&lake, "hydrology-test.hydr", 123));
    Hydrology loaded = {0};
    CHECK(!hydrology_load(&loaded, "hydrology-test.hydr", 124));
    CHECK(hydrology_load(&loaded, "hydrology-test.hydr", 123));
    CHECK(hydrology_level(&loaded, 72.1, 50.7) == 100);
    hydrology_destroy(&loaded);
    FILE *f = fopen("hydrology-test.hydr", "r+b");
    CHECK(f && !fseek(f, 70, SEEK_SET));
    fputc(0xff, f);
    fclose(f);
    CHECK(!hydrology_load(&loaded, "hydrology-test.hydr", 123));
    remove("hydrology-test.hydr");
    hydrology_destroy(&lake);
    for (int i = 0; i < W * H; i++)
        elevation[i] -= 500;
    CHECK(hydrology_build(&lake, elevation, water, W, H, 10, 10));
    CHECK(hydrology_level(&lake, 50, 50) == -400); /* A lake below sea level. */
    CHECK(!(lake.bodies[0].flags & 2));
    hydrology_destroy(&lake);
    memset(water, 1, sizeof(water));
    memset(elevation, 0, sizeof(elevation));
    CHECK(hydrology_build(&lake, elevation, water, W, H, 20, 20));
    CHECK(lake.bodies[0].flags & 2);
    CHECK(hydrology_level(&lake, 50, 50) == 0);
    hydrology_destroy(&lake);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            water[y * W + x] = y >= 43 && y <= 57;
            elevation[y * W + x] = (float)(100 + x * .1 + abs(y - 50) * 2 + sin(x * .2));
        }
    Hydrology river = {0};
    CHECK(hydrology_build(&river, elevation, water, W, H, 10, 10));
    CHECK(river.body_count == 1 && river.bodies[0].span_m > 0);
    double previous = hydrology_level(&river, 1, 50);
    for (int x = 2; x < W - 1; x++) {
        double level = hydrology_level(&river, x, 50);
        CHECK(fabs(level - hydrology_level(&river, x, 44)) < 1e-6);
        CHECK(level >= previous && level - previous <= .201);
        previous = level;
    }
    CHECK(hydrology_level(&river, 98, 50) > hydrology_level(&river, 1, 50) + 5);
    CHECK(fabs(hydrology_level(&river, 50.01, 50) - hydrology_level(&river, 50, 50)) < .01);
    hydrology_destroy(&river);
    memset(water, 0, sizeof(water));
    Hydrology dry = {0};
    CHECK(hydrology_build(&dry, elevation, water, W, H, 10, 10));
    CHECK(dry.body_count == 0 && isnan(hydrology_level(&dry, 50, 50)) && dry.shore_m[0] < -10000);
    hydrology_destroy(&dry);
    puts("PASS: level lakes, smooth monotone river heads and flat cross-sections, graded banks/beds, cache "
         "identity/CRC");
    return 0;
}
