#ifndef SOLAR_H
#define SOLAR_H
#include "planet.h"
#define SOLAR_COUNT 9
#define SOLAR_EARTH 3
#define LIGHT_SPEED_MPS 299792458.0
#define ASTRONOMICAL_UNIT_M 149597870700.0
#define SOLAR_GM 1.32712440018e20
/* Barycentric, J2000 equatorial axes; independent of Earth-local millimeter edits. */
typedef struct {
    const char *name;
    double mass_kg, radius_m, gm, semimajor_m, eccentricity;
    DVec3 position_m, velocity_mps;
    Vec3 color;
    double proper_time_s;
} SolarBody;
typedef struct {
    SolarBody bodies[SOLAR_COUNT];
    double time_s;
    bool relativistic;
} SolarSystem;
void solar_init(SolarSystem *system);
bool solar_step(SolarSystem *system, double seconds);
/* Test-particle Schwarzschild correction in weak-field harmonic coordinates. */
DVec3 relativity_acceleration(double gm, DVec3 relative_position, DVec3 relative_velocity);
double relativity_gamma(double speed_mps);
DVec3 relativity_add_velocity(DVec3 frame_velocity, DVec3 local_velocity);
double relativity_clock_rate(double potential_m2ps2, double speed_mps);
double solar_potential(const SolarSystem *system, DVec3 position_m, int exclude);
DVec3 solar_sun_direction(const SolarSystem *system, DVec3 earth_local_m);
#endif
