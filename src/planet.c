#include "planet.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define PI 3.14159265358979323846
DVec3 pd_add(DVec3 a, DVec3 b) {
    return dv3(a.x + b.x, a.y + b.y, a.z + b.z);
}
DVec3 pd_sub(DVec3 a, DVec3 b) {
    return dv3(a.x - b.x, a.y - b.y, a.z - b.z);
}
DVec3 pd_scale(DVec3 a, double s) {
    return dv3(a.x * s, a.y * s, a.z * s);
}
double pd_dot(DVec3 a, DVec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
double pd_length(DVec3 a) {
    return sqrt(pd_dot(a, a));
}
DVec3 pd_normalize(DVec3 a) {
    double n = pd_length(a);
    return n > 0 ? pd_scale(a, 1 / n) : dv3(0, 0, 0);
}
DVec3 planet_position_m(WorldPosition p) {
    return dv3(p.cell.x * .125 + p.offset_mm.x * .001, p.cell.y * .125 + p.offset_mm.y * .001,
               p.cell.z * .125 + p.offset_mm.z * .001);
}
bool planet_set_position(DVec3 m, WorldPosition *out) {
    if (!isfinite(m.x) || !isfinite(m.y) || !isfinite(m.z) || fabs(m.x) > 2e9 || fabs(m.y) > 2e9 ||
        fabs(m.z) > 2e9)
        return false;
    return position_from_local(cell_coord(0, 0, 0), pd_scale(m, 8), out);
}
static uint32_t u32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static double f64(const unsigned char *p) {
    uint64_t b = 0;
    double d;
    for (int i = 0; i < 8; i++)
        b |= (uint64_t)p[i] << (i * 8);
    memcpy(&d, &b, 8);
    return d;
}
static bool earth_load(EarthData *d, const char *file, bool global) {
    FILE *f = fopen(file, "rb");
    if (!f)
        return false;
    unsigned char h[56];
    bool ok = fread(h, 1, 56, f) == 56 && !memcmp(h, "EDEM", 4) && u32(h + 4) == 1;
    if (ok) {
        d->rows = u32(h + 8);
        d->columns = u32(h + 12);
        d->lat0 = f64(h + 16);
        d->lon0 = f64(h + 24);
        d->lat_step = f64(h + 32);
        d->lon_step = f64(h + 40);
        d->checksum = u32(h + 48);
        ok = d->rows >= 2 && d->columns >= 4 && d->rows <= 21600 && d->columns <= 43200 &&
             (uint64_t)d->rows * d->columns * 2 <= 64u * 1024u * 1024u && isfinite(d->lat0) &&
             isfinite(d->lon0) && isfinite(d->lat_step) && isfinite(d->lon_step) && d->lat_step > 0 &&
             d->lon_step > 0 && (!global || fabs(d->lon_step * d->columns - 360) < .01) && d->lat0 >= -90 &&
             d->lat0 + (d->rows - 1) * d->lat_step <= 90 && (!global || (d->rows - 1) * d->lat_step >= 179) &&
             (global || ((uint64_t)d->rows * d->columns * 2 <= 2u * 1024u * 1024u && d->lat_step < .01 &&
                         d->lon_step < .01)) &&
             u32(h + 52) == 0;
    }
    size_t count = ok ? (size_t)d->rows * d->columns : 0;
    unsigned char *bytes = ok ? malloc(count * 2) : NULL;
    if (ok)
        ok = bytes && fread(bytes, 2, count, f) == count && fgetc(f) == EOF && !ferror(f);
    if (ok) {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < 56; i++)
            if (i < 48 || i >= 52)
                hash = (hash ^ h[i]) * 16777619u;
        for (size_t i = 0; i < count * 2; i++)
            hash = (hash ^ bytes[i]) * 16777619u;
        ok = hash == d->checksum;
        if (ok) {
            d->samples = (int16_t *)bytes;
            for (size_t i = 0; i < count; i++) {
                uint16_t v = (uint16_t)bytes[2 * i] | (uint16_t)bytes[2 * i + 1] << 8;
                memcpy(d->samples + i, &v, 2);
            }
        }
    }
    if (ok) {
        for (uint32_t i = 0; i < d->columns; i++) {
            d->pole_south += d->samples[i];
            d->pole_north += d->samples[(size_t)(d->rows - 1) * d->columns + i];
        }
        d->pole_south /= d->columns;
        d->pole_north /= d->columns;
    }
    if (!ok)
        free(bytes);
    fclose(f);
    return ok;
}
/* Store functions live in planet_store.c to keep SQLite out of public headers. */
bool planet_store_open(Planet *p, const char *file);
void planet_store_close(Planet *p);
bool planet_init(Planet *p, const char *earth_file, const char *save_file, double mass, double a, double b) {
    memset(p, 0, sizeof(*p));
    if (!isfinite(mass) || mass < 1e10 || mass > 1e30 || !isfinite(a) || a < 1000 || a > 1e8 ||
        !isfinite(b) || b < a * .5 || b > a) {
        snprintf(p->error, sizeof(p->error), "Invalid planetary mass/radii");
        return false;
    }
    p->mass_kg = mass;
    p->a_m = a;
    p->b_m = b;
    if (!earth_load(&p->earth, earth_file, true)) {
        snprintf(p->error, sizeof(p->error), "Cannot load/check Earth elevation data: %s", earth_file);
        return false;
    }
    const char *names[] = {"colorado", "sydney", "himalaya"};
    const char *slash = strrchr(earth_file, '/');
    size_t prefix = slash ? (size_t)(slash - earth_file + 1) : 0;
    p->terrain_checksum = p->earth.checksum ^ 0x52454c34u;
    char base[1024], fine[1200];
    if (prefix >= sizeof(base)) {
        planet_destroy(p);
        return false;
    }
    snprintf(base, sizeof(base), "%.*s", (int)prefix, earth_file);
    snprintf(fine, sizeof(fine), "%searth_5min.edem", base);
    if (earth_load(&p->earth_fine, fine, true))
        p->terrain_checksum = (p->terrain_checksum ^ p->earth_fine.checksum) * 16777619u;
    p->relief_enabled = true;
    for (size_t i = 0; i < PLANET_REGION_LIMIT; i++) {
        char path[1024];
        if (prefix >= sizeof(path) / 2)
            break;
        snprintf(path, sizeof(path), "%.*searth_regions/%s.edem", (int)prefix, earth_file, names[i]);
        EarthData region = {0};
        if (earth_load(&region, path, false)) {
            p->regions[p->region_count++] = region;
            p->terrain_checksum = (p->terrain_checksum ^ region.checksum) * 16777619u;
        }
    }
    if (!planet_terrain_open(p, base)) {
        planet_destroy(p);
        return false;
    }
    if (!planet_store_open(p, save_file)) {
        planet_terrain_close(p);
        free(p->earth_fine.samples);
        p->earth_fine.samples = NULL;
        planet_store_close(p);
        for (size_t i = 0; i < p->region_count; i++) {
            free(p->regions[i].samples);
            p->regions[i].samples = NULL;
        }
        free(p->earth.samples);
        p->earth.samples = NULL;
        return false;
    }
    return true;
}
void planet_destroy(Planet *p) {
    planet_store_close(p);
    planet_terrain_close(p);
    free(p->earth_fine.samples);
    free(p->earth.samples);
    for (size_t i = 0; i < p->region_count; i++)
        free(p->regions[i].samples);
    memset(p, 0, sizeof(*p));
}
static double global_elevation(const Planet *p, double lat, double lon) {
    const EarthData *d = p->earth_fine.samples ? &p->earth_fine : &p->earth;
    if (!d->samples || !isfinite(lat) || !isfinite(lon))
        return 0;
    lon = fmod(lon - d->lon0, 360);
    if (lon < 0)
        lon += 360;
    double x = lon / d->lon_step, y = fmax(0, fmin(d->rows - 1, (lat - d->lat0) / d->lat_step));
    int x0 = (int)floor(x) % d->columns, x1 = (x0 + 1) % d->columns, y0 = (int)floor(y),
        y1 = y0 + 1 < (int)d->rows ? y0 + 1 : y0;
    double tx = x - floor(x), ty = y - y0;
    double a =
        d->samples[(size_t)y0 * d->columns + x0] * (1 - tx) + d->samples[(size_t)y0 * d->columns + x1] * tx;
    double b =
        d->samples[(size_t)y1 * d->columns + x0] * (1 - tx) + d->samples[(size_t)y1 * d->columns + x1] * tx;
    double value = a * (1 - ty) + b * ty;
    /* Collapse the longitude singularity smoothly at each pole. */
    if (fabs(lat) > 89) {
        double mean = lat > 0 ? d->pole_north : d->pole_south;
        double blend = fmin(1, (fabs(lat) - 89));
        value = value * (1 - blend) + mean * blend;
    }
    return value * p->a_m / EARTH_A_METERS;
}
double planet_measured_elevation(const Planet *p, double lat, double lon) {
    double base = global_elevation(p, lat, lon);
    double detail;
    bool water;
    if (planet_detail_sample(p, lat, lon, &detail, &water) && isfinite(detail) &&
        !(water && detail <= .01 && base < 0))
        return detail;
    if (!isfinite(lat) || !isfinite(lon))
        return base;
    lon = fmod(lon, 360);
    if (lon < 0)
        lon += 360;
    for (size_t i = 0; i < p->region_count; i++) {
        const EarthData *d = &p->regions[i];
        double x = (lon - d->lon0) / d->lon_step, y = (lat - d->lat0) / d->lat_step;
        if (x < 0 || y < 0 || x >= d->columns - 1 || y >= d->rows - 1)
            continue;
        int ix = (int)floor(x), iy = (int)floor(y);
        double tx = x - ix, ty = y - iy;
        const int16_t *row = d->samples + (size_t)iy * d->columns;
        double a = row[ix] * (1 - tx) + row[ix + 1] * tx;
        row += d->columns;
        double b = row[ix] * (1 - tx) + row[ix + 1] * tx;
        double edge = fmin(fmin(x, d->columns - 1 - x), fmin(y, d->rows - 1 - y));
        double weight = fmin(1, edge / 24);
        weight = weight * weight * (3 - 2 * weight);
        return base * (1 - weight) + (a * (1 - ty) + b * ty) * weight * p->a_m / EARTH_A_METERS;
    }
    return base;
}
DVec3 planet_from_geographic(const Planet *p, double latitude, double longitude, double h) {
    double lat = latitude * PI / 180, lon = longitude * PI / 180, s = sin(lat), c = cos(lat),
           e2 = 1 - p->b_m * p->b_m / (p->a_m * p->a_m);
    double n = p->a_m / sqrt(1 - e2 * s * s);
    return dv3((n + h) * c * cos(lon), (n + h) * c * sin(lon), (n * (1 - e2) + h) * s);
}
PlanetGeo planet_geographic(const Planet *p, DVec3 v) {
    double rho = hypot(v.x, v.y), e2 = 1 - p->b_m * p->b_m / (p->a_m * p->a_m);
    if (rho < 1e-9)
        return (PlanetGeo){v.z >= 0 ? 90 : -90, 0, fabs(v.z) - p->b_m};
    double lat = atan2(v.z, rho * (1 - e2)), h = 0;
    for (int i = 0; i < 6; i++) {
        double s = sin(lat), n = p->a_m / sqrt(1 - e2 * s * s);
        h = fabs(cos(lat)) > .01 ? rho / cos(lat) - n : v.z / sin(lat) - n * (1 - e2);
        double denom = n + h;
        if (fabs(denom) < 1e-9)
            break;
        double next = atan2(v.z, rho * (1 - e2 * n / denom));
        if (fabs(next - lat) < 1e-13) {
            lat = next;
            break;
        }
        lat = next;
    }
    return (PlanetGeo){lat * 180 / PI, atan2(v.y, v.x) * 180 / PI, h};
}
DVec3 planet_up(const Planet *p, DVec3 position) {
    PlanetGeo g = planet_geographic(p, position);
    double lat = g.latitude_deg * PI / 180, lon = g.longitude_deg * PI / 180;
    return dv3(cos(lat) * cos(lon), cos(lat) * sin(lon), sin(lat));
}
DVec3 planet_gravity(const Planet *p, DVec3 v) {
    double r = pd_length(v);
    if (r < 1e-9)
        return dv3(0, 0, 0);
    DVec3 n = pd_scale(v, 1 / r);
    double surface = 1 / sqrt((n.x * n.x + n.y * n.y) / (p->a_m * p->a_m) + n.z * n.z / (p->b_m * p->b_m));
    double mu = GRAVITATIONAL_CONSTANT * p->mass_kg;
    double factor = r >= surface ? -mu / (r * r * r) : -mu / (surface * surface * surface);
    return pd_scale(v, factor);
}
DVec3 planet_sun_direction(void) {
    return pd_normalize(dv3(-.9, .2, .38));
}
double planet_view_distance(const Planet *p, WorldPosition camera, double requested) {
    DVec3 position = planet_position_m(camera);
    double r = pd_length(position), hull = p->a_m * (1 + 12000 / EARTH_A_METERS);
    double view = requested > 0 ? requested : 60000;
    /* An exterior space view always reaches the far side of the planet,
       even when a short surface view distance was selected. */
    if (planet_geographic(p, position).altitude_m > 50000)
        view = fmax(view, r + hull);
    return view;
}
double planet_near_distance(const Planet *p, WorldPosition camera) {
    double clearance = pd_length(planet_position_m(camera)) - p->a_m * (1 + 12000 / EARTH_A_METERS);
    /* Tiny near planes suit millimeter work. Space has no geometry near the
       camera, so a larger near plane also keeps integer depth buffers useful. */
    return fmax(.0001, clearance * .001);
}
Voxel planet_base_voxel(const Planet *p, DVec3 position) {
    double r = pd_length(position), small = fmin(p->a_m, p->b_m);
    if (r < small - 12000)
        return (Voxel){r < 3485000 * p->a_m / EARTH_A_METERS ? MAT_IRON : MAT_STONE, 0,
                       (float)fmin(5500, 300 + (small - r) * .001)};
    PlanetGeo g = planet_geographic(p, position);
    double h = planet_elevation(p, g.latitude_deg, g.longitude_deg), depth = h - g.altitude_m;
    if (depth < 0) {
        double level;
        bool wet = planet_water_elevation(p, g.latitude_deg, g.longitude_deg, &level);
        return (Voxel){wet && g.altitude_m < level ? MAT_WATER : MAT_AIR, 0, 288.15f};
    }
    uint16_t surface = planet_surface_material(g.latitude_deg, g.longitude_deg, h * EARTH_A_METERS / p->a_m);
    uint16_t material = depth > 2                                   ? MAT_STONE
                        : surface == MAT_SAND || surface == MAT_ICE ? surface
                        : depth > .04                               ? MAT_SOIL
                                                                    : surface;
    return (Voxel){material, 0, (float)fmin(5500, 288.15 + fmax(0, depth) * .025)};
}
uint16_t planet_surface_material(double latitude, double longitude, double height) {
    if (height < 0)
        return MAT_WATER;
    if (height > 4500 || fabs(latitude) > 70)
        return MAT_ICE;
    if (height < 10 || (fabs(latitude) > 15 && fabs(latitude) < 35 &&
                        ((longitude > -20 && longitude < 60) || (longitude > 110 && longitude < 150))))
        return MAT_SAND;
    return MAT_GRASS;
}
