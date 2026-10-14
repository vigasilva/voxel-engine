#include "water.h"
WaterOptics water_default_optics(void) {
    return (WaterOptics){.absorption_per_m = {.18f, .055f, .022f},
                         .scattering_color = {.025f, .13f, .23f},
                         .density_kg_m3 = 1000,
                         .index_of_refraction = 1.333f,
                         .wave_strength = .012f,
                         .turbidity = .3f};
}
Vec3 water_transmittance(WaterOptics o, double depth) {
    depth = fmax(0, depth);
    return v3((float)exp(-o.absorption_per_m.x * depth), (float)exp(-o.absorption_per_m.y * depth),
              (float)exp(-o.absorption_per_m.z * depth));
}
