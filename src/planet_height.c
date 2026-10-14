#include "planet.h"
#include "hydrology.h"
#define PI 3.14159265358979323846
static void source_geometry(const Planet *p, double lat, double lon, double *spacing, double *slope) {
    double dx, dy;
    *slope = 0;
    if (!isfinite(lat) || !isfinite(lon)) {
        *spacing = 20000;
        return;
    }
    if (planet_detail_resolution(p, lat, lon, &dx, &dy)) {
        TerrainSourceInfo source;
        *spacing =
            (planet_detail_source(p, lat, lon, &source) ? source.effective_resolution_m : fmax(dx, dy)) *
            EARTH_A_METERS / p->a_m;
        planet_detail_slope(p, lat, lon, slope);
        return;
    }
    const EarthData *d = p->earth_fine.samples ? &p->earth_fine : &p->earth;
    double normalized = fmod(lon, 360);
    if (normalized < 0)
        normalized += 360;
    bool region = false;
    for (size_t i = 0; i < p->region_count; i++) {
        const EarthData *r = &p->regions[i];
        double x = (normalized - r->lon0) / r->lon_step, y = (lat - r->lat0) / r->lat_step;
        if (x >= 24 && y >= 24 && x < r->columns - 25 && y < r->rows - 25) {
            d = r;
            region = true;
            break;
        }
    }
    if (!d->samples || d->columns < 2 || d->rows < 2 || d->lat_step <= 0 || d->lon_step <= 0) {
        *spacing = 20000;
        return;
    }
    double x = region ? (normalized - d->lon0) / d->lon_step : fmod(lon - d->lon0, 360) / d->lon_step;
    if (x < 0)
        x += 360 / d->lon_step;
    double y = fmax(0, fmin(d->rows - 1, (lat - d->lat0) / d->lat_step));
    int x0 = (int)floor(x) % d->columns, x1 = (x0 + 1) % d->columns, y0 = (int)floor(y),
        y1 = y0 + 1 < (int)d->rows ? y0 + 1 : y0;
    double tx = x - floor(x), ty = y - y0;
    double a = d->samples[(size_t)y0 * d->columns + x0], b = d->samples[(size_t)y0 * d->columns + x1],
           c = d->samples[(size_t)y1 * d->columns + x0], e = d->samples[(size_t)y1 * d->columns + x1];
    dx = d->lon_step * 111319.49079327358 * fmax(.001, fabs(cos(lat * PI / 180)));
    dy = d->lat_step * 111319.49079327358;
    *spacing = fmax(dx, dy);
    *slope = hypot(((b - a) * (1 - ty) + (e - c) * ty) / dx, ((c - a) * (1 - tx) + (e - b) * tx) / dy);
    *slope *= fmin(1, fmax(0, 90 - fabs(lat))); /* Collapsed polar height ring. */
}
static bool water_profile(const Planet *p, double lat, double lon, double measured, bool rivers,
                          PlanetWaterProfile *out) {
    if (planet_detail_water(p, lat, lon, out))
        return true;
    if (measured < 0) {
        *out = (PlanetWaterProfile){.wet = true,
                                    .marine = true,
                                    .level_m = 0,
                                    .shore_m = 1e6,
                                    .depth_m = 2 * p->a_m / EARTH_A_METERS,
                                    .bank_width_m = 20 * p->a_m / EARTH_A_METERS};
        return true;
    }
    return rivers && planet_river_profile(p, lat, lon, out);
}
static LandscapeSample sample_landscape(const Planet *p, double lat, double lon, double measured,
                                        const PlanetWaterProfile *profile, bool detail) {
    Planet reference = {.a_m = EARTH_A_METERS, .b_m = EARTH_B_METERS};
    LandscapeInput input = {.point_m = planet_from_geographic(&reference, lat, lon, 0),
                            .latitude_deg = lat,
                            .height_m = measured * EARTH_A_METERS / p->a_m,
                            .requested = BIOME_AUTO};
    source_geometry(p, lat, lon, &input.source_spacing_m, &input.slope);
    planet_detail_land_cover(p, lat, lon, &input.cover);
    if (profile) {
        input.wet = profile->wet;
        input.marine = profile->marine;
    }
    LandscapeConfig config = landscape_default();
    return landscape_sample(&config, input, detail);
}
LandscapeSample planet_landscape_at(const Planet *p, double lat, double lon, bool detail) {
    double h = planet_measured_elevation(p, lat, lon);
    PlanetWaterProfile profile;
    bool has = water_profile(p, lat, lon, h, true, &profile);
    return sample_landscape(p, lat, lon, h, has ? &profile : NULL, detail && p->relief_enabled);
}
LandscapeSample planet_overview_landscape(const Planet *p, double lat, double lon) {
    double h = planet_measured_elevation(p, lat, lon);
    PlanetWaterProfile profile;
    /* Global centerline rivers are smaller than an overview texel. Keeping
       them in local geometry also avoids querying their index for the atlas. */
    bool has = water_profile(p, lat, lon, h, false, &profile);
    return sample_landscape(p, lat, lon, h, has ? &profile : NULL, false);
}
uint16_t planet_appearance_material(const Planet *p, double lat, double lon) {
    return planet_overview_landscape(p, lat, lon).appearance_material;
}
size_t planet_elevation_bytes(const Planet *p) {
    size_t bytes = planet_terrain_bytes(p) + (size_t)p->earth_fine.rows * p->earth_fine.columns * 2 +
                   (size_t)p->earth.rows * p->earth.columns * 2;
    for (size_t i = 0; i < p->region_count; i++)
        bytes += (size_t)p->regions[i].rows * p->regions[i].columns * 2;
    return bytes;
}
double planet_elevation_state(const Planet *p, double lat, double lon, LandscapeSample *state) {
    double h = planet_measured_elevation(p, lat, lon);
    PlanetWaterProfile profile;
    bool bank = water_profile(p, lat, lon, h, true, &profile);
    if (bank && profile.wet) {
        if (state)
            *state = sample_landscape(p, lat, lon, h, &profile, false);
        if (profile.marine && h < 0)
            return h;
        return hydrology_bed(h, profile.level_m, fmax(0, profile.shore_m), profile.depth_m,
                             profile.bank_width_m);
    }
    LandscapeSample sample = sample_landscape(p, lat, lon, h, bank ? &profile : NULL, p->relief_enabled);
    if (state)
        *state = sample;
    h += sample.detail_m * p->a_m / EARTH_A_METERS;
    return bank && isfinite(profile.level_m) ? hydrology_bed(h, profile.level_m, fmin(0, profile.shore_m),
                                                             profile.depth_m, profile.bank_width_m)
                                             : h;
}
double planet_elevation(const Planet *p, double lat, double lon) {
    return planet_elevation_state(p, lat, lon, NULL);
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
    PlanetWaterProfile profile;
    /* Native classifications override the coarse ocean/river inference, also
       for dry depressions and inland lakes below the sea-level datum. */
    if (planet_detail_water(p, lat, lon, &profile)) {
        if (profile.wet && height)
            *height = isfinite(profile.level_m) ? profile.level_m : planet_measured_elevation(p, lat, lon);
        return profile.wet;
    }
    double measured = planet_measured_elevation(p, lat, lon);
    if (measured < 0) {
        if (height)
            *height = 0;
        return true;
    }
    if (!planet_river_profile(p, lat, lon, &profile) || !profile.wet)
        return false;
    if (height)
        *height = profile.level_m;
    return true;
}
double planet_visible_elevation(const Planet *p, double lat, double lon) {
    double ground = planet_elevation(p, lat, lon), level;
    return planet_water_elevation(p, lat, lon, &level) ? fmax(level, ground) : ground;
}
uint16_t planet_visible_material(const Planet *p, double lat, double lon) {
    LandscapeSample sample;
    planet_elevation_state(p, lat, lon, &sample);
    return sample.surface_material;
}
