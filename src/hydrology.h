#ifndef HYDROLOGY_H
#define HYDROLOGY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef struct {
    double center_x_m, center_y_m, axis_x, axis_y, minimum_m, span_m;
    float heads_m[32];
    uint32_t flags, reserved; /* Bit 0: river; bit 1: estimated marine water. */
} HydroBody;
typedef struct {
    size_t width, height;
    double pixel_x_m, pixel_y_m;
    uint32_t *body_ids;
    float *shore_m;
    HydroBody *bodies;
    size_t body_count, body_capacity;
    void *storage;
    size_t storage_bytes;
    bool mapped;
} Hydrology;
/* Heights and water classification cover the same pixel centers. Output water
   heads are estimates from terrain, not surveyed bathymetry or a fluid solver. */
bool hydrology_build(Hydrology *out, const float *elevation_m, const uint8_t *water, size_t width,
                     size_t height, double pixel_x_m, double pixel_y_m);
void hydrology_destroy(Hydrology *h);
double hydrology_sample(const Hydrology *h, double pixel_x, double pixel_y, double *shore_m, bool *marine);
double hydrology_level(const Hydrology *h, double pixel_x, double pixel_y);
bool hydrology_load(Hydrology *h, const char *path, uint32_t source_checksum);
bool hydrology_save(const Hydrology *h, const char *path, uint32_t source_checksum);
double hydrology_bed(double ground_m, double level_m, double shore_m, double depth_m, double bank_width_m);
#endif
