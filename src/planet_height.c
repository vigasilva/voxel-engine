#include "planet.h"
/* Continuous 3-D noise on the ellipsoid avoids longitude and polar seams.
   It supplies synthetic local relief, never claimed as measured Earth data. */
static double fade(double t) {
    return t * t * t * (t * (t * 6 - 15) + 10);
}
static uint32_t hash(uint32_t n) {
    n ^= n >> 16;
    n *= 0x7feb352du;
    n ^= n >> 15;
    n *= 0x846ca68bu;
    return n ^ (n >> 16);
}
static double noise(DVec3 p, double scale) {
    double v[3] = {p.x / scale, p.y / scale, p.z / scale}, t[3];
    int64_t cell[3];
    for (int a = 0; a < 3; a++) {
        cell[a] = (int64_t)floor(v[a]);
        t[a] = fade(v[a] - cell[a]);
    }
    double result = 0;
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 2; y++)
            for (int x = 0; x < 2; x++) {
                uint32_t n = hash((uint32_t)(cell[0] + x) * 73856093u ^ (uint32_t)(cell[1] + y) * 19349663u ^
                                  (uint32_t)(cell[2] + z) * 83492791u ^ 0x45a91e37u);
                result += (n / 2147483647.5 - 1) * (x ? t[0] : 1 - t[0]) * (y ? t[1] : 1 - t[1]) *
                          (z ? t[2] : 1 - t[2]);
            }
    return result;
}
size_t planet_elevation_bytes(const Planet *p) {
    size_t bytes = planet_terrain_bytes(p) + (size_t)p->earth_fine.rows * p->earth_fine.columns * 2 +
                   (size_t)p->earth.rows * p->earth.columns * 2;
    for (size_t i = 0; i < p->region_count; i++)
        bytes += (size_t)p->regions[i].rows * p->regions[i].columns * 2;
    return bytes;
}
double planet_elevation(const Planet *p, double lat, double lon) {
    double h = planet_measured_elevation(p, lat, lon);
    double level;
    if (planet_water_elevation(p, lat, lon, &level) && h >= 0)
        return level - 2 * p->a_m / EARTH_A_METERS;
    bool water;
    double native;
    bool measured_detail = planet_detail_sample(p, lat, lon, &native, &water) && isfinite(native);
    if (!p->relief_enabled || !isfinite(lat) || !isfinite(lon))
        return h;
    Planet reference = {.a_m = EARTH_A_METERS, .b_m = EARTH_B_METERS};
    DVec3 point = planet_from_geographic(&reference, lat, lon, 0);
    const double scales[] = {1024, 256, 64, 16, 4, 1, .25, .0625};
    const double amplitudes[] = {12, 4, 1, .18, .035, .012, .004, .001};
    double relief = 0;
    for (int i = measured_detail ? 4 : 0; i < 8; i++)
        relief += noise(point, scales[i]) * amplitudes[i];
    /* Preserve measured coastlines and the flat ocean surface. */
    double shore = fmin(1, fabs(h) / (20 * p->a_m / EARTH_A_METERS));
    return h + relief * shore * p->a_m / EARTH_A_METERS;
}
DVec3 planet_surface_normal(const Planet *p, DVec3 point) {
    PlanetGeo g = planet_geographic(p, point);
    DVec3 up = planet_up(p, point), east = pd_normalize(dv3(-up.y, up.x, 0));
    if (pd_length(east) < .5)
        east = dv3(1, 0, 0);
    DVec3 north =
        dv3(up.y * east.z - up.z * east.y, up.z * east.x - up.x * east.z, up.x * east.y - up.y * east.x);
    double water_level;
    if (planet_water_elevation(p, g.latitude_deg, g.longitude_deg, &water_level) &&
        g.altitude_m >= water_level - .01)
        return up;
    DVec3 directions[2] = {east, north};
    double slopes[2];
    double distance = .125 * p->a_m / EARTH_A_METERS;
    for (int i = 0; i < 2; i++) {
        PlanetGeo a = planet_geographic(p, pd_add(point, pd_scale(directions[i], distance))),
                  b = planet_geographic(p, pd_add(point, pd_scale(directions[i], -distance)));
        slopes[i] = (planet_elevation(p, a.latitude_deg, a.longitude_deg) -
                     planet_elevation(p, b.latitude_deg, b.longitude_deg)) /
                    (2 * distance);
    }
    return pd_normalize(pd_sub(pd_sub(up, pd_scale(east, slopes[0])), pd_scale(north, slopes[1])));
}

bool planet_water_elevation(const Planet *p, double lat, double lon, double *height) {
    double measured = planet_measured_elevation(p, lat, lon);
    if (measured < 0) {
        if (height)
            *height = 0;
        return true;
    }
    double native;
    bool water;
    if (planet_detail_sample(p, lat, lon, &native, &water)) {
        if (water && height)
            *height = fmax(0, isfinite(native) ? native : measured);
        return water;
    }
    double center_lat, center_lon, depth;
    if (!planet_river_sample(p, lat, lon, &center_lat, &center_lon, &depth))
        return false;
    if (height)
        *height = fmax(0, planet_measured_elevation(p, center_lat, center_lon));
    return true;
}
double planet_visible_elevation(const Planet *p, double lat, double lon) {
    double ground = planet_elevation(p, lat, lon), level;
    return planet_water_elevation(p, lat, lon, &level) ? fmax(level, ground) : ground;
}
uint16_t planet_visible_material(const Planet *p, double lat, double lon) {
    double level;
    if (planet_water_elevation(p, lat, lon, &level))
        return MAT_WATER;
    return planet_surface_material(lat, lon, planet_elevation(p, lat, lon) * EARTH_A_METERS / p->a_m);
}
