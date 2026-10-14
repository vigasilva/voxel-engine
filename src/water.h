#ifndef WATER_H
#define WATER_H
#include "math3d.h"
/* Optical parameters are independent of the editable water voxel volume.
   A future volume solver can reuse these coefficients in ray marching. */
typedef struct {
    Vec3 absorption_per_m, scattering_color;
    float density_kg_m3, index_of_refraction, wave_strength, turbidity;
} WaterOptics;
WaterOptics water_default_optics(void);
Vec3 water_transmittance(WaterOptics optics, double depth_m);
#endif
