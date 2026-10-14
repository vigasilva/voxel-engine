#include "solar.h"
#include "water.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
#define PI 3.14159265358979323846
static DVec3 cross(DVec3 a, DVec3 b) {
    return dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static double eccentric_angle(const SolarSystem *s) {
    DVec3 r = pd_sub(s->bodies[1].position_m, s->bodies[0].position_m),
          v = pd_sub(s->bodies[1].velocity_mps, s->bodies[0].velocity_mps);
    DVec3 e = pd_sub(pd_scale(cross(v, cross(r, v)), 1 / SOLAR_GM), pd_normalize(r));
    return atan2(e.y, e.x);
}
static void mercury_precession(void) {
    SolarSystem gr, classic;
    solar_init(&gr);
    double a = .38709927 * ASTRONOMICAL_UNIT_M, e = .20563593, r = a * (1 - e);
    for (int i = 0; i < 9; i++) {
        gr.bodies[i].position_m = dv3(i * 1e16, 0, 0);
        gr.bodies[i].velocity_mps = dv3(0, 0, 0);
        if (i) {
            gr.bodies[i].gm = 0;
            gr.bodies[i].mass_kg = 0;
        }
    }
    gr.bodies[1].position_m = dv3(r, 0, 0);
    gr.bodies[1].velocity_mps = dv3(0, sqrt(SOLAR_GM * (1 + e) / r), 0);
    classic = gr;
    classic.relativistic = false;
    double period = 2 * PI * sqrt(a * a * a / SOLAR_GM);
    CHECK(solar_step(&gr, period));
    CHECK(solar_step(&classic, period));
    double advance = remainder(eccentric_angle(&gr) - eccentric_angle(&classic), 2 * PI);
    double analytic = 6 * PI * SOLAR_GM / (a * (1 - e * e) * LIGHT_SPEED_MPS * LIGHT_SPEED_MPS);
    CHECK(advance > 0 && fabs(advance / analytic - 1) < .002);
    double century = advance * 206264.80624709636 * 36525 * 86400 / period;
    CHECK(century > 42.8 && century < 43.2);
    printf("Integrated Mercury relativistic precession: %.5f arcseconds/century\n", century);
}
static void orbital_stability(void) {
    SolarSystem s;
    solar_init(&s);
    DVec3 initial = s.bodies[3].position_m;
    double r0 = pd_length(pd_sub(s.bodies[3].position_m, s.bodies[0].position_m));
    CHECK(solar_step(&s, 86400 * 365.25));
    CHECK(pd_length(pd_sub(s.bodies[3].position_m, initial)) < ASTRONOMICAL_UNIT_M * .025);
    double r1 = pd_length(pd_sub(s.bodies[3].position_m, s.bodies[0].position_m));
    CHECK(fabs(r1 / r0 - 1) < .001);
    CHECK(s.bodies[3].proper_time_s < s.time_s);
    CHECK(!solar_step(&s, NAN));
    CHECK(!solar_step(&s, -1));
    for (int i = 1; i < 9; i++) {
        CHECK(pd_length(s.bodies[i].velocity_mps) < 60000);
        CHECK(pd_length(pd_sub(s.bodies[i].position_m, s.bodies[0].position_m)) > s.bodies[i].radius_m);
    }
}
int main(void) {
    CHECK(relativity_gamma(0) == 1);
    CHECK(fabs(relativity_gamma(.6 * LIGHT_SPEED_MPS) - 1.25) < 1e-12);
    CHECK(isinf(relativity_gamma(LIGHT_SPEED_MPS)));
    CHECK(fabs(relativity_clock_rate(0, .6 * LIGHT_SPEED_MPS) - .8) < 1e-12);
    double surface = -GRAVITATIONAL_CONSTANT * EARTH_MASS_KG / EARTH_A_METERS;
    CHECK(relativity_clock_rate(surface, 0) < relativity_clock_rate(surface * .5, 0));
    DVec3 combined =
        relativity_add_velocity(dv3(.6 * LIGHT_SPEED_MPS, 0, 0), dv3(.6 * LIGHT_SPEED_MPS, 0, 0));
    CHECK(fabs(combined.x / LIGHT_SPEED_MPS - 15.0 / 17) < 1e-12);
    CHECK(pd_length(combined) < LIGHT_SPEED_MPS);
    WaterOptics water = water_default_optics();
    Vec3 clear = water_transmittance(water, 0), deep = water_transmittance(water, 20);
    CHECK(clear.x == 1 && clear.y == 1 && clear.z == 1);
    CHECK(deep.x < deep.y && deep.y < deep.z && deep.z < 1);
    mercury_precession();
    orbital_stability();
    puts("Solar orbits, clock dilation, and water optics passed");
    return 0;
}
