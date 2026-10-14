#define _POSIX_C_SOURCE 200809L
#include "surface_texture.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
int main(void) {
    int16_t samples[32];
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 8; x++)
            samples[y * 8 + x] = (int16_t)(x < 4 ? -100 : 100 + y * 10);
    Planet p = {.a_m = EARTH_A_METERS,
                .b_m = EARTH_B_METERS,
                .earth = {.samples = samples,
                          .rows = 4,
                          .columns = 8,
                          .lat0 = -67.5,
                          .lon0 = -157.5,
                          .lat_step = 45,
                          .lon_step = 45}};
    char file[] = "/tmp/voxel-surface-texture-XXXXXX";
    int fd = mkstemp(file);
    CHECK(fd >= 0);
    close(fd);
    CHECK(planet_surface_texture_bake(&p, file));
    unsigned char *pixels = NULL;
    uint32_t width = 0, height = 0;
    CHECK(planet_surface_texture_load(file, &pixels, &width, &height));
    CHECK(width == 8 && height == 4);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned x = 0; x < 8; x++) {
            size_t at = (y * 8 + x) * 4;
            double decoded = ((pixels[at] * 256 + pixels[at + 1]) / 65535.0) * 24000 - 12000;
            CHECK(fabs(decoded - samples[(3 - y) * 8 + x]) < .19);
            CHECK(pixels[at + 2] == planet_surface_material(90 - 180 * (y + .5) / height,
                                                            -180 + 360 * (x + .5) / width, fmax(0, decoded)));
            CHECK(pixels[at + 3] == 0);
        }
    free(pixels);
    FILE *f = fopen(file, "r+b");
    CHECK(f);
    CHECK(fseek(f, 16, SEEK_SET) == 0);
    int byte = fgetc(f);
    CHECK(fseek(f, 16, SEEK_SET) == 0);
    fputc(byte ^ 1, f);
    fclose(f);
    CHECK(!planet_surface_texture_load(file, &pixels, &width, &height) && pixels == NULL);
    remove(file);
    puts("Engine height/material atlas, geographical orientation, and corruption rejection passed");
    return 0;
}
