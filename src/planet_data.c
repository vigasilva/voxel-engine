#define _DEFAULT_SOURCE
#include "planet.h"
#include "hydrology.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <zlib.h>
#ifdef __unix__
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif
#define PI 3.14159265358979323846
#define BINS (360 * 180)
typedef struct {
    char name[256];
    unsigned char header[64];
    unsigned char *data;
    size_t bytes;
    uint32_t nx, ny, wx, wy, version;
    double lon0, lat0, dx, dy;
    int lat_key, lon_key;
    Hydrology hydrology;
    bool hydrology_attempted;
} DetailMap;
typedef struct {
    float lon0, lat0, lon1, lat1, width;
} River;
struct PlanetTerrainData {
    DetailMap *maps;
    size_t count, capacity, bytes;
    char directory[1024];
    uint32_t *buckets;
    size_t bucket_count;
    River *rivers;
    uint32_t river_count, *offsets, *ids, river_checksum;
    double river_max_width;
};
static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static double get64(const unsigned char *p) {
    uint64_t bits = 0;
    double value;
    for (int i = 0; i < 8; i++)
        bits |= (uint64_t)p[i] << (i * 8);
    memcpy(&value, &bits, 8);
    return value;
}
static void unmap(DetailMap *m) {
    hydrology_destroy(&m->hydrology);
#ifdef __unix__
    if (m->data)
        munmap(m->data, m->bytes);
#else
    free(m->data);
#endif
}
static bool load_map(DetailMap *m, const char *path) {
    unsigned char *data = NULL;
    size_t bytes = 0;
#ifdef __unix__
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0)
        return false;
    if (fstat(fd, &st) == 0 && st.st_size >= 64 && st.st_size <= 256 * 1024 * 1024) {
        bytes = (size_t)st.st_size;
        void *mapped = mmap(NULL, bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapped != MAP_FAILED)
            data = mapped;
    }
    close(fd);
#else
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    if (!fseek(f, 0, SEEK_END)) {
        long end = ftell(f);
        if (end >= 64 && end <= 256 * 1024 * 1024)
            bytes = (size_t)end;
    }
    rewind(f);
    if (bytes) {
        data = malloc(bytes);
        if (data && fread(data, 1, bytes, f) != bytes) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
#endif
    if (!data)
        return false;
    *m = (DetailMap){.data = data, .bytes = bytes};
    m->version = get32(data + 4);
    m->nx = get32(data + 8);
    m->ny = get32(data + 12);
    m->wx = get32(data + 16);
    m->wy = get32(data + 20);
    m->lon0 = get64(data + 24);
    m->lat0 = get64(data + 32);
    m->dx = get64(data + 40);
    m->dy = get64(data + 48);
    uint64_t expected = 64 + (uint64_t)m->nx * m->ny * 4 + (uint64_t)m->wx * m->wy;
    bool ok = !memcmp(data, "PDEM", 4) && (m->version == 1 || m->version == 2) && get32(data + 60) == 0 &&
              expected == bytes && m->nx >= 2 && m->ny >= 2 && m->wx >= 2 && m->wy >= 2 && m->nx <= 20000 &&
              m->ny <= 20000 && isfinite(m->lat0) && isfinite(m->lon0) && isfinite(m->dx) &&
              isfinite(m->dy) && m->lat0 >= -90 && m->lon0 >= -180 && m->dx > 0 && m->dy > 0 &&
              m->dx <= .001 && m->dy <= .001 && m->lat0 + (m->ny - 1) * m->dy <= 90 &&
              m->lon0 + (m->nx - 1) * m->dx <= 180 &&
              crc32(0, data + 64, (uInt)(bytes - 64)) == get32(data + 56);
    if (ok) {
        const unsigned char *cover = data + 64 + (size_t)m->nx * m->ny * 4;
        for (size_t i = 0; i < (size_t)m->wx * m->wy; i++) {
            unsigned c = cover[i];
            if (m->version == 1 ? c > 1
                                : c != 0 && c != 10 && c != 20 && c != 30 && c != 40 && c != 50 && c != 60 &&
                                      c != 70 && c != 80 && c != 90 && c != 95 && c != 100) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        unmap(m);
        *m = (DetailMap){0};
        return false;
    }
    memcpy(m->header, data, 64);
#ifdef __unix__
    madvise(data, bytes, MADV_RANDOM);
    /* CRC validation touches the whole file; release these initial page mappings. */
    madvise(data, bytes, MADV_DONTNEED);
#endif
    return true;
}
static size_t tile_hash(int lat, int lon, size_t mask) {
    return ((uint32_t)lat * 73856093u ^ (uint32_t)lon * 19349663u) & mask;
}
static void map_index(PlanetTerrainData *t) {
    size_t capacity = 16;
    while (capacity < t->count * 2)
        capacity *= 2;
    uint32_t *buckets = calloc(capacity, sizeof(*buckets));
    free(t->buckets);
    t->buckets = NULL;
    t->bucket_count = 0;
    if (!buckets)
        return;
    for (size_t i = 0; i < t->count; i++) {
        DetailMap *m = &t->maps[i];
        m->lat_key = (int)floor((m->lat0 + (m->ny - 1) * m->dy * .5) * 10);
        m->lon_key = (int)floor((m->lon0 + (m->nx - 1) * m->dx * .5) * 10);
        size_t slot = tile_hash(m->lat_key, m->lon_key, capacity - 1);
        while (buckets[slot])
            slot = (slot + 1) & (capacity - 1);
        buckets[slot] = (uint32_t)i + 1;
    }
    t->buckets = buckets;
    t->bucket_count = capacity;
}
static const DetailMap *lookup(const PlanetTerrainData *t, int lat, int lon) {
    if (!t->buckets)
        return NULL;
    size_t slot = tile_hash(lat, lon, t->bucket_count - 1);
    while (t->buckets[slot]) {
        const DetailMap *m = &t->maps[t->buckets[slot] - 1];
        if (m->lat_key == lat && m->lon_key == lon)
            return m;
        slot = (slot + 1) & (t->bucket_count - 1);
    }
    return NULL;
}
size_t planet_detail_refresh(Planet *p) {
    PlanetTerrainData *t = p->terrain;
    if (!t)
        return 0;
    DIR *d = opendir(t->directory);
    if (!d)
        return 0;
    size_t added = 0;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || n >= 256 || strcmp(entry->d_name + n - 5, ".pdem"))
            continue;
        size_t known = SIZE_MAX;
        for (size_t i = 0; i < t->count; i++)
            if (!strcmp(t->maps[i].name, entry->d_name)) {
                known = i;
                break;
            }
        char path[1400];
        snprintf(path, sizeof(path), "%s/%s", t->directory, entry->d_name);
        if (known != SIZE_MAX) {
            unsigned char header[64];
            FILE *f = fopen(path, "rb");
            bool changed = f && fread(header, 1, 64, f) == 64 && memcmp(header, t->maps[known].header, 64);
            if (f)
                fclose(f);
            if (!changed)
                continue;
        }
        DetailMap m;
        if (!load_map(&m, path))
            continue;
        snprintf(m.name, sizeof(m.name), "%s", entry->d_name);
        if (known != SIZE_MAX) {
            DetailMap *old = &t->maps[known];
            size_t profile_bytes =
                !old->hydrology.body_ids ? 0
                : old->hydrology.storage_bytes
                    ? old->hydrology.storage_bytes
                    : (size_t)old->wx * old->wy * 8 + old->hydrology.body_capacity * sizeof(HydroBody);
            t->bytes -= old->bytes + profile_bytes;
            unmap(old);
            *old = m;
            t->bytes += m.bytes;
            added++;
            continue;
        }
        if (t->count == t->capacity) {
            size_t capacity = t->capacity ? t->capacity * 2 : 8;
            DetailMap *maps = realloc(t->maps, capacity * sizeof(*maps));
            if (!maps) {
                unmap(&m);
                break;
            }
            t->maps = maps;
            t->capacity = capacity;
        }
        t->maps[t->count++] = m;
        t->bytes += m.bytes;
        added++;
    }
    closedir(d);
    if (added) {
        map_index(t);
        p->surface_revision++;
    }
    return added;
}
static DetailMap *detail_at(const Planet *p, double lat, double lon, double *x, double *y) {
    if (!p->terrain || !isfinite(lat) || !isfinite(lon) || fabs(lat) > 90)
        return NULL;
    lon = remainder(lon, 360);
    int lat_key = (int)floor(lat * 10), lon_key = (int)floor(lon * 10);
    static const int offsets[9][2] = {{0, 0},   {-1, 0}, {1, 0},  {0, -1}, {0, 1},
                                      {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};
    size_t tries = p->terrain->buckets ? 9 : p->terrain->count;
    for (size_t i = 0; i < tries; i++) {
        const DetailMap *m = p->terrain->buckets
                                 ? lookup(p->terrain, lat_key + offsets[i][0], lon_key + offsets[i][1])
                                 : &p->terrain->maps[i];
        if (!m)
            continue;
        *x = (lon - m->lon0) / m->dx;
        *y = (lat - m->lat0) / m->dy;
        if (*x >= 0 && *y >= 0 && *x < m->nx - 1 && *y < m->ny - 1)
            return (DetailMap *)m; /* This Planet owns its lazily prepared cache. */
    }
    return NULL;
}
static double detail_height(const DetailMap *m, double x, double y) {
    x = fmax(0, fmin(m->nx - 1.000001, x));
    y = fmax(0, fmin(m->ny - 1.000001, y));
    int ix = (int)x, iy = (int)y;
    double tx = x - ix, ty = y - iy, values[4];
    for (int corner = 0; corner < 4; corner++) {
        int32_t cm = (int32_t)get32(m->data + 64 + 4 * ((size_t)(iy + corner / 2) * m->nx + ix + corner % 2));
        if (cm == INT32_MIN || cm < -1200000 || cm > 1000000)
            return NAN;
        values[corner] = cm * .01;
    }
    return (values[0] * (1 - tx) + values[1] * tx) * (1 - ty) + (values[2] * (1 - tx) + values[3] * tx) * ty;
}
static const uint8_t *water_mask(const DetailMap *m) {
    return m->data + 64 + (size_t)m->nx * m->ny * 4;
}
static void water_coordinates(const DetailMap *m, double *x, double *y) {
    *x = (*x + .5) * m->wx / m->nx - .5;
    *y = (*y + .5) * m->wy / m->ny - .5;
}
static size_t water_index(const DetailMap *m, double x, double y) {
    size_t ix = (size_t)fmax(0, fmin(m->wx - 1, floor(x + .5))),
           iy = (size_t)fmax(0, fmin(m->wy - 1, floor(y + .5)));
    return iy * m->wx + ix;
}
bool planet_detail_sample(const Planet *p, double lat, double lon, double *height, bool *water) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m)
        return false;
    if (height)
        *height = detail_height(m, x, y) * p->a_m / EARTH_A_METERS;
    water_coordinates(m, &x, &y);
    if (water)
        *water = water_mask(m)[water_index(m, x, y)] == (m->version == 1 ? 1 : COVER_WATER);
    return true;
}
const char *planet_land_cover_name(LandCover cover) {
    switch (cover) {
    case COVER_TREE:
        return "tree cover";
    case COVER_SHRUB:
        return "shrubland";
    case COVER_GRASS:
        return "grassland";
    case COVER_CROP:
        return "cropland";
    case COVER_BUILT:
        return "built-up";
    case COVER_BARE:
        return "bare/sparse vegetation";
    case COVER_SNOW:
        return "snow/ice";
    case COVER_WATER:
        return "permanent water";
    case COVER_WETLAND:
        return "herbaceous wetland";
    case COVER_MANGROVE:
        return "mangroves";
    case COVER_MOSS:
        return "moss/lichen";
    default:
        return "unknown";
    }
}
bool planet_detail_land_cover(const Planet *p, double lat, double lon, LandCover *cover) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m)
        return false;
    water_coordinates(m, &x, &y);
    unsigned c = water_mask(m)[water_index(m, x, y)];
    if (m->version == 1)
        c = c == 1 ? COVER_WATER : COVER_UNKNOWN;
    if (cover)
        *cover = (LandCover)c;
    return c != COVER_UNKNOWN;
}
bool planet_detail_water_mask(const Planet *p, double lat, double lon, bool *water) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m)
        return false;
    water_coordinates(m, &x, &y);
    unsigned c = water_mask(m)[water_index(m, x, y)];
    if (m->version == 2 && !c)
        return false;
    if (water)
        *water = c == (m->version == 1 ? 1 : COVER_WATER);
    return true;
}
bool planet_detail_slope(const Planet *p, double lat, double lon, double *slope) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m)
        return false;
    int ix = (int)x, iy = (int)y;
    double tx = x - ix, ty = y - iy, values[4];
    for (int corner = 0; corner < 4; corner++) {
        int32_t cm = (int32_t)get32(m->data + 64 + 4 * ((size_t)(iy + corner / 2) * m->nx + ix + corner % 2));
        if (cm == INT32_MIN || cm < -1200000 || cm > 1000000)
            return false;
        values[corner] = cm * .01;
    }
    double dx = m->dx * 111319.49079327358 * fmax(.001, fabs(cos(lat * PI / 180))),
           dy = m->dy * 111319.49079327358;
    if (slope)
        *slope = hypot(((values[1] - values[0]) * (1 - ty) + (values[3] - values[2]) * ty) / dx,
                       ((values[2] - values[0]) * (1 - tx) + (values[3] - values[1]) * tx) / dy);
    return true;
}
bool planet_detail_resolution(const Planet *p, double lat, double lon, double *dx, double *dy) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m || !isfinite(detail_height(m, x, y)))
        return false;
    double scale = p->a_m / EARTH_A_METERS * 111319.49079327358;
    if (dx)
        *dx = m->dx * scale * fabs(cos(lat * PI / 180));
    if (dy)
        *dy = m->dy * scale;
    return true;
}
static bool prepare_hydrology(PlanetTerrainData *t, DetailMap *m) {
    if (m->hydrology_attempted)
        return m->hydrology.body_ids != NULL;
    m->hydrology_attempted = true;
    char path[1400];
    snprintf(path, sizeof(path), "%s/%s.hydr", t->directory, m->name);
    uint32_t checksum = (uint32_t)crc32(0, m->data, 64);
    if (!hydrology_load(&m->hydrology, path, checksum)) {
        size_t count = (size_t)m->wx * m->wy;
        float *elevation = malloc(count * sizeof(*elevation));
        if (!elevation)
            return false;
        for (size_t y = 0; y < m->wy; y++)
            for (size_t x = 0; x < m->wx; x++)
                elevation[y * m->wx + x] =
                    (float)detail_height(m, (x + .5) * m->nx / m->wx - .5, (y + .5) * m->ny / m->wy - .5);
        double dx = m->dx * m->nx / m->wx * 111319.49079327358 *
                    fmax(.001, cos((m->lat0 + m->ny * m->dy * .5) * PI / 180)),
               dy = m->dy * m->ny / m->wy * 111319.49079327358;
        uint8_t *mask = NULL;
        const uint8_t *water = water_mask(m);
        if (m->version == 2) {
            mask = malloc(count);
            if (!mask) {
                free(elevation);
                return false;
            }
            for (size_t i = 0; i < count; i++)
                mask[i] = water[i] == COVER_WATER;
            water = mask;
        }
        bool ok = hydrology_build(&m->hydrology, elevation, water, m->wx, m->wy, dx, dy);
        free(mask);
        free(elevation);
        if (!ok)
            return false;
        if (hydrology_save(&m->hydrology, path, checksum)) {
            Hydrology mapped = {0};
            if (hydrology_load(&mapped, path, checksum)) {
                hydrology_destroy(&m->hydrology);
                m->hydrology = mapped;
            }
        }
    }
    t->bytes += m->hydrology.storage_bytes
                    ? m->hydrology.storage_bytes
                    : (size_t)m->wx * m->wy * 8 + m->hydrology.body_capacity * sizeof(HydroBody);
    return true;
}
bool planet_detail_water(const Planet *p, double lat, double lon, PlanetWaterProfile *profile) {
    double x, y;
    DetailMap *m = detail_at(p, lat, lon, &x, &y);
    if (!m)
        return false;
    double native = detail_height(m, x, y);
    water_coordinates(m, &x, &y);
    if (m->version == 2 && !water_mask(m)[water_index(m, x, y)])
        return false; /* Unclassified data is not proof of dry land. */
    bool wet = water_mask(m)[water_index(m, x, y)] == (m->version == 1 ? 1 : COVER_WATER);
    *profile = (PlanetWaterProfile){.wet = wet, .level_m = NAN, .shore_m = -1e6};
    /* Dry queries far from a shore need neither a water cache nor a build. */
    bool nearby = wet || m->hydrology.body_ids;
    for (int ky = -3; !nearby && ky <= 3; ky++)
        for (int kx = -3; !nearby && kx <= 3; kx++)
            nearby = water_mask(m)[water_index(m, x + kx, y + ky)] == (m->version == 1 ? 1 : COVER_WATER);
    if (!nearby)
        return true;
    double level = NAN, shore = -1e6;
    bool marine = false;
    if (prepare_hydrology(p->terrain, m))
        level = hydrology_sample(&m->hydrology, x, y, &shore, &marine);
    if (!isfinite(level) && wet)
        level = native; /* Allocation/read-only fallback still honors the mask. */
    double scale = p->a_m / EARTH_A_METERS;
    profile->marine = marine;
    profile->level_m = level * scale;
    profile->shore_m = shore * scale;
    profile->depth_m = 2 * scale;
    profile->bank_width_m = 20 * scale;
    return true;
}
static void river_bins(River r, int *x0, int *x1, int *y0, int *y1) {
    double pad = (r.width + 30) / 111000 + 1e-5;
    double longitude_pad = pad / fmax(.05, cos((r.lat0 + r.lat1) * .5 * PI / 180));
    *x0 = (int)floor(fmin(r.lon0, r.lon1) - longitude_pad + 180);
    *x1 = (int)floor(fmax(r.lon0, r.lon1) + longitude_pad + 180);
    *y0 = (int)floor(fmin(r.lat0, r.lat1) - pad + 90);
    *y1 = (int)floor(fmax(r.lat0, r.lat1) + pad + 90);
    if (*y0 < 0)
        *y0 = 0;
    if (*y1 > 179)
        *y1 = 179;
}
static void load_rivers(PlanetTerrainData *t, const char *path) {
    FILE *f = fopen(path, "rb");
    unsigned char header[16];
    if (!f)
        return;
    if (fread(header, 1, 16, f) != 16 || memcmp(header, "RIVR", 4) || get32(header + 4) != 1) {
        fclose(f);
        return;
    }
    uint32_t count = get32(header + 8);
    if (!count || count > 2000000) {
        fclose(f);
        return;
    }
    unsigned char *raw = malloc((size_t)count * 20);
    bool ok = raw && fread(raw, 20, count, f) == count && fgetc(f) == EOF &&
              crc32(0, raw, count * 20) == get32(header + 12);
    fclose(f);
    if (!ok) {
        free(raw);
        return;
    }
    River *rivers = calloc(count, sizeof(*rivers));
    uint32_t *counts = calloc(BINS + 1, sizeof(*counts));
    if (!rivers || !counts) {
        free(raw);
        free(rivers);
        free(counts);
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        float v[5];
        for (int k = 0; k < 5; k++) {
            uint32_t bits = get32(raw + i * 20 + k * 4);
            memcpy(&v[k], &bits, 4);
        }
        if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) || !isfinite(v[3]) || !isfinite(v[4]) ||
            fabs(v[0]) > 180 || fabs(v[2]) > 180 || fabs(v[1]) > 90 || fabs(v[3]) > 90 || v[4] <= 0 ||
            v[4] > 2000 || fabs(v[0] - v[2]) > .051 || fabs(v[1] - v[3]) > .051) {
            free(raw);
            free(rivers);
            free(counts);
            return;
        }
        rivers[i] = (River){v[0], v[1], v[2], v[3], v[4]};
        if (v[4] > t->river_max_width)
            t->river_max_width = v[4];
        int x0, x1, y0, y1;
        river_bins(rivers[i], &x0, &x1, &y0, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                counts[y * 360 + (x + 360) % 360 + 1]++;
    }
    free(raw);
    for (int i = 1; i <= BINS; i++)
        counts[i] += counts[i - 1];
    uint32_t *ids = malloc((size_t)counts[BINS] * sizeof(*ids)), *cursor = malloc(BINS * sizeof(*cursor));
    if (!ids || !cursor) {
        free(rivers);
        free(counts);
        free(ids);
        free(cursor);
        return;
    }
    memcpy(cursor, counts, BINS * sizeof(*cursor));
    for (uint32_t i = 0; i < count; i++) {
        int x0, x1, y0, y1;
        river_bins(rivers[i], &x0, &x1, &y0, &y1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                ids[cursor[y * 360 + (x + 360) % 360]++] = i;
    }
    free(cursor);
    t->rivers = rivers;
    t->river_count = count;
    t->offsets = counts;
    t->ids = ids;
    t->river_checksum = get32(header + 12);
    t->bytes += (size_t)count * sizeof(River) + (BINS + 1) * sizeof(uint32_t) +
                (size_t)counts[BINS] * sizeof(uint32_t);
}
bool planet_river_sample(const Planet *p, double lat, double lon, double *center_lat, double *center_lon,
                         double *depth) {
    PlanetTerrainData *t = p->terrain;
    if (!t || !t->rivers || !isfinite(lat) || !isfinite(lon) || fabs(lat) >= 90)
        return false;
    lon = remainder(lon, 360);
    int x = (int)floor(lon + 180), y = (int)floor(lat + 90);
    if (x == 360)
        x = 0;
    double scale = 111319.49079327358, cosine = fmax(.01, cos(lat * PI / 180)), best = HUGE_VAL;
    bool hit = false;
    for (uint32_t j = t->offsets[y * 360 + x]; j < t->offsets[y * 360 + x + 1]; j++) {
        River r = t->rivers[t->ids[j]];
        double ax = (r.lon0 - lon) * scale * cosine, ay = (r.lat0 - lat) * scale;
        double dx = (r.lon1 - r.lon0) * scale * cosine, dy = (r.lat1 - r.lat0) * scale;
        double length = dx * dx + dy * dy,
               u = length > 0 ? fmax(0, fmin(1, -(ax * dx + ay * dy) / length)) : 0;
        double distance = hypot(ax + u * dx, ay + u * dy), ratio = distance / r.width;
        if (ratio <= 1 && ratio < best) {
            best = ratio;
            hit = true;
            if (center_lat)
                *center_lat = r.lat0 + u * (r.lat1 - r.lat0);
            if (center_lon)
                *center_lon = r.lon0 + u * (r.lon1 - r.lon0);
            if (depth)
                *depth = fmax(2, fmin(20, r.width * .02)) * p->a_m / EARTH_A_METERS;
        }
    }
    return hit;
}
bool planet_river_profile(const Planet *p, double lat, double lon, PlanetWaterProfile *profile) {
    PlanetTerrainData *t = p->terrain;
    if (!t || !t->rivers || !isfinite(lat) || !isfinite(lon) || fabs(lat) >= 90)
        return false;
    lon = remainder(lon, 360);
    int x = (int)floor(lon + 180), y = (int)floor(lat + 90);
    if (x == 360)
        x = 0;
    double scale = 111319.49079327358, cosine = fmax(.01, cos(lat * PI / 180)), best = HUGE_VAL;
    bool hit = false;
    for (uint32_t j = t->offsets[y * 360 + x]; j < t->offsets[y * 360 + x + 1]; j++) {
        River r = t->rivers[t->ids[j]];
        double ax = (r.lon0 - lon) * scale * cosine, ay = (r.lat0 - lat) * scale;
        double dx = (r.lon1 - r.lon0) * scale * cosine, dy = (r.lat1 - r.lat0) * scale;
        double length = dx * dx + dy * dy,
               u = length > 0 ? fmax(0, fmin(1, -(ax * dx + ay * dy) / length)) : 0;
        double distance = hypot(ax + u * dx, ay + u * dy), ratio = distance / r.width;
        if (distance <= r.width + 30 && ratio < best) {
            best = ratio;
            hit = true;
            double scale = p->a_m / EARTH_A_METERS;
            *profile =
                (PlanetWaterProfile){.wet = distance <= r.width,
                                     .level_m = planet_measured_elevation(p, r.lat0 + u * (r.lat1 - r.lat0),
                                                                          r.lon0 + u * (r.lon1 - r.lon0)),
                                     .shore_m = (r.width - distance) * scale,
                                     .depth_m = fmax(2, fmin(20, r.width * .02)) * scale,
                                     .bank_width_m = fmin(20, r.width * .5) * scale};
        }
    }
    return hit;
}
bool planet_terrain_open(Planet *p, const char *base) {
    p->terrain = calloc(1, sizeof(*p->terrain));
    if (!p->terrain)
        return false;
    snprintf(p->terrain->directory, sizeof(p->terrain->directory), "%searth_detail", base);
    char path[1400];
    snprintf(path, sizeof(path), "%searth_rivers.rivr", base);
    load_rivers(p->terrain, path);
    planet_detail_refresh(p);
    p->terrain_checksum = (p->terrain_checksum ^ p->terrain->river_checksum) * 16777619u;
    return true;
}
void planet_terrain_close(Planet *p) {
    if (!p->terrain)
        return;
    for (size_t i = 0; i < p->terrain->count; i++)
        unmap(&p->terrain->maps[i]);
    free(p->terrain->buckets);
    free(p->terrain->maps);
    free(p->terrain->rivers);
    free(p->terrain->offsets);
    free(p->terrain->ids);
    free(p->terrain);
    p->terrain = NULL;
}
size_t planet_terrain_bytes(const Planet *p) {
    return p->terrain ? p->terrain->bytes + sizeof(*p->terrain) + p->terrain->capacity * sizeof(DetailMap) +
                            p->terrain->bucket_count * sizeof(uint32_t)
                      : 0;
}
size_t planet_detail_count(const Planet *p) {
    return p->terrain ? p->terrain->count : 0;
}
double planet_river_max_width(const Planet *p) {
    return p->terrain ? p->terrain->river_max_width * p->a_m / EARTH_A_METERS : 0;
}
size_t planet_river_count(const Planet *p) {
    return p->terrain ? p->terrain->river_count : 0;
}
const char *planet_detail_directory(const Planet *p) {
    return p->terrain ? p->terrain->directory : "";
}
static int id_order(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}
bool planet_river_candidates(const Planet *p, double lat, double lon, double radius_m, uint32_t **ids,
                             size_t *count) {
    *ids = NULL;
    *count = 0;
    PlanetTerrainData *t = p->terrain;
    if (!t || !t->offsets)
        return true;
    double latitude_pad = fmin(180, radius_m / p->a_m * 180 / PI + .02);
    double longitude_pad = fmin(180, latitude_pad / fmax(.01, cos(lat * PI / 180)));
    int y0 = (int)floor(fmax(0, lat - latitude_pad + 90)),
        y1 = (int)floor(fmin(179, lat + latitude_pad + 90));
    int x0 = (int)floor(lon - longitude_pad + 180), x1 = (int)floor(lon + longitude_pad + 180);
    if (x1 - x0 >= 360)
        x1 = x0 + 359;
    size_t total = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int bin = y * 360 + (x % 360 + 360) % 360;
            total += t->offsets[bin + 1] - t->offsets[bin];
        }
    if (!total)
        return true;
    uint32_t *result = malloc(total * sizeof(*result));
    if (!result)
        return false;
    size_t at = 0;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int bin = y * 360 + (x % 360 + 360) % 360;
            size_t n = t->offsets[bin + 1] - t->offsets[bin];
            memcpy(result + at, t->ids + t->offsets[bin], n * sizeof(*result));
            at += n;
        }
    qsort(result, total, sizeof(*result), id_order);
    size_t unique = 1;
    for (size_t i = 1; i < total; i++)
        if (result[i] != result[unique - 1])
            result[unique++] = result[i];
    *ids = result;
    *count = unique;
    return true;
}
bool planet_river_segment(const Planet *p, uint32_t id, PlanetRiverSegment *out) {
    if (!p->terrain || id >= p->terrain->river_count)
        return false;
    River r = p->terrain->rivers[id];
    *out = (PlanetRiverSegment){
        .lat = {r.lat0, r.lat1}, .lon = {r.lon0, r.lon1}, .half_width_m = r.width * p->a_m / EARTH_A_METERS};
    return true;
}
