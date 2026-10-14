#include "surface_texture.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void put32(unsigned char *p, uint32_t n) {
    for (int i = 0; i < 4; i++)
        p[i] = (unsigned char)(n >> (8 * i));
}
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
bool planet_surface_texture_bake(const Planet *p, const char *path) {
    const EarthData *base = p->earth_fine.samples ? &p->earth_fine : &p->earth;
    uint32_t width = base->columns, height = base->rows;
    size_t size = (size_t)width * height * 4;
    if (!width || !height || size > 256u * 1024u * 1024u)
        return false;
    unsigned char *pixels = malloc(size);
    if (!pixels)
        return false;
    for (uint32_t y = 0; y < height; y++) {
        double lat = 90 - 180 * (y + .5) / height;
        for (uint32_t x = 0; x < width; x++) {
            double lon = -180 + 360 * (x + .5) / width;
            /* Subpixel synthetic relief is intentionally omitted from the distant atlas.
               Measured base terrain and engine material rules provide all of its values. */
            double meters = planet_measured_elevation(p, lat, lon) * EARTH_A_METERS / p->a_m;
            unsigned encoded = (unsigned)llround((fmax(-12000, fmin(12000, meters)) + 12000) / 24000 * 65535);
            size_t at = ((size_t)y * width + x) * 4;
            pixels[at] = (unsigned char)(encoded >> 8);
            pixels[at + 1] = (unsigned char)encoded;
            pixels[at + 2] = (unsigned char)planet_surface_material(lat, lon, fmax(0, meters));
            bool water = false;
            planet_detail_sample(p, lat, lon, NULL, &water);
            pixels[at + 3] = water && meters >= 0 ? 255 : 0;
        }
    }
    uLongf compressed_size = compressBound((uLong)size);
    unsigned char *compressed = malloc(compressed_size);
    bool ok =
        compressed && compress2(compressed, &compressed_size, pixels, (uLong)size, Z_BEST_SPEED) == Z_OK;
    unsigned char header[32] = {0};
    memcpy(header, "PTEX", 4);
    put32(header + 4, 1);
    put32(header + 8, width);
    put32(header + 12, height);
    put32(header + 16, (uint32_t)crc32(0, pixels, (uInt)size));
    put32(header + 20, (uint32_t)compressed_size);
    put32(header + 24, p->terrain_checksum);
    FILE *file = ok ? fopen(path, "wb") : NULL;
    if (file) {
        ok = fwrite(header, 1, 32, file) == 32 &&
             fwrite(compressed, 1, compressed_size, file) == compressed_size;
        if (fclose(file) != 0)
            ok = false;
    } else
        ok = false;
    free(compressed);
    free(pixels);
    return ok;
}
bool planet_surface_texture_load(const char *path, unsigned char **pixels, uint32_t *width,
                                 uint32_t *height) {
    *pixels = NULL;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    unsigned char header[32];
    bool ok = fread(header, 1, 32, file) == 32 && !memcmp(header, "PTEX", 4) && get32(header + 4) == 1 &&
              get32(header + 28) == 0;
    uint32_t w = ok ? get32(header + 8) : 0, h = ok ? get32(header + 12) : 0,
             length = ok ? get32(header + 20) : 0;
    ok = ok && w >= 4 && w <= 8192 && h >= 2 && h <= 4096 && length > 0 && length <= 256u * 1024u * 1024u &&
         (uint64_t)w * h * 4 <= 256u * 1024u * 1024u;
    unsigned char *compressed = ok ? malloc(length) : NULL;
    unsigned char *rgba = ok ? malloc((size_t)w * h * 4) : NULL;
    if (ok)
        ok = compressed && rgba && fread(compressed, 1, length, file) == length && fgetc(file) == EOF &&
             !ferror(file);
    if (ok) {
        uLongf bytes = (uLongf)w * h * 4;
        ok = uncompress(rgba, &bytes, compressed, length) == Z_OK && bytes == (uLongf)w * h * 4 &&
             (uint32_t)crc32(0, rgba, (uInt)bytes) == get32(header + 16);
    }
    fclose(file);
    free(compressed);
    if (!ok) {
        free(rgba);
        return false;
    }
    *pixels = rgba;
    *width = w;
    *height = h;
    return true;
}
