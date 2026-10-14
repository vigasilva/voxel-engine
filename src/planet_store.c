#include "planet.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {
    bool used;
    CellCoord key;
    uint64_t age;
    DetailChunk *mask, *values;
} Cache;
struct PlanetStore {
    sqlite3 *db;
    Cache cache[PLANET_CACHE_LIMIT];
    uint64_t clock;
    size_t saved_count;
};
static Voxel mask_air(void) {
    return (Voxel){MAT_AIR, 0, 293.15f};
}
static Voxel marker(void) {
    return (Voxel){MAT_IRON, 0, 293.15f};
}
static int64_t floor_div(int64_t n, int64_t d) {
    int64_t q = n / d;
    return q - (n % d < 0);
}
static bool same(CellCoord a, CellCoord b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
static bool error(Planet *p, const char *s) {
    snprintf(p->error, sizeof(p->error), "%s", s);
    return false;
}
static bool sql(Planet *p, const char *s) {
    char *e = NULL;
    int rc = sqlite3_exec(p->store->db, s, NULL, NULL, &e);
    if (rc != SQLITE_OK) {
        error(p, e ? e : "SQLite failure");
        sqlite3_free(e);
        return false;
    }
    return true;
}
static void release(Cache *c) {
    detail_destroy(c->mask);
    detail_destroy(c->values);
    memset(c, 0, sizeof(*c));
}
size_t planet_storage_bytes(const Planet *p) {
    size_t n = sizeof(*p) + planet_elevation_bytes(p);
    if (p->store) {
        n += sizeof(*p->store);
        for (int i = 0; i < PLANET_CACHE_LIMIT; i++)
            n += detail_storage_bytes(p->store->cache[i].mask) +
                 detail_storage_bytes(p->store->cache[i].values);
    }
    return n;
}
size_t planet_resident_chunks(const Planet *p) {
    size_t n = 0;
    if (p->store)
        for (int i = 0; i < PLANET_CACHE_LIMIT; i++)
            n += p->store->cache[i].mask != NULL;
    return n;
}
static void budget(Planet *p, Cache *keep) {
    while (planet_storage_bytes(p) > PLANET_EDIT_BUDGET + planet_elevation_bytes(p)) {
        Cache *old = NULL;
        for (int i = 0; i < PLANET_CACHE_LIMIT; i++) {
            Cache *c = &p->store->cache[i];
            if (c != keep && c->used && (!old || c->age < old->age))
                old = c;
        }
        if (!old)
            break;
        release(old);
    }
}
static Cache *slot(Planet *p, CellCoord key) {
    Cache *old = NULL;
    for (int i = 0; i < PLANET_CACHE_LIMIT; i++) {
        Cache *c = &p->store->cache[i];
        if (c->used && same(c->key, key)) {
            c->age = ++p->store->clock;
            return c;
        }
        if (!c->used) {
            if (!old || old->used)
                old = c;
        } else if (!old || (old->used && c->age < old->age))
            old = c;
    }
    release(old);
    old->used = true;
    old->key = key;
    old->age = ++p->store->clock;
    return old;
}
static uint32_t hash(const unsigned char *b, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
static void put32(unsigned char *b, uint32_t n) {
    for (int i = 0; i < 4; i++)
        b[i] = (unsigned char)(n >> (8 * i));
}
static uint32_t get32(const unsigned char *b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static bool mask_valid(MmBox b, Voxel v, void *ctx) {
    (void)b;
    (void)ctx;
    return voxel_equal(v, mask_air()) || voxel_equal(v, marker());
}
static bool decode(Planet *p, Cache *c, const unsigned char *bytes, size_t size) {
    if (size < 20 || memcmp(bytes, "PVED", 4) || get32(bytes + 4) != 1 ||
        get32(bytes + 8) + (uint64_t)get32(bytes + 12) + 20 != size ||
        hash(bytes + 20, size - 20) != get32(bytes + 16))
        return error(p, "Invalid planet chunk record/checksum");
    size_t at = 20, end = 20 + get32(bytes + 8);
    DetailChunk *mask = detail_decode_unlimited(bytes, end, &at);
    if (!mask || at != end || !detail_visit(mask, mask_valid, NULL)) {
        detail_destroy(mask);
        return error(p, "Invalid planet override mask");
    }
    DetailChunk *values = detail_decode_unlimited(bytes, size, &at);
    if (!values || at != size) {
        detail_destroy(mask);
        detail_destroy(values);
        return error(p, "Invalid planet voxel values");
    }
    c->mask = mask;
    c->values = values;
    return true;
}
static Cache *get(Planet *p, CellCoord key) {
    for (int i = 0; i < PLANET_CACHE_LIMIT; i++) {
        Cache *c = &p->store->cache[i];
        if (c->used && same(c->key, key)) {
            c->age = ++p->store->clock;
            return c;
        }
    }
    Cache *c = slot(p, key);
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(p->store->db, "SELECT data FROM chunks WHERE x=? AND y=? AND z=?", -1, &s, NULL) !=
        SQLITE_OK) {
        error(p, sqlite3_errmsg(p->store->db));
        release(c);
        return NULL;
    }
    sqlite3_bind_int64(s, 1, key.x);
    sqlite3_bind_int64(s, 2, key.y);
    sqlite3_bind_int64(s, 3, key.z);
    int rc = sqlite3_step(s);
    bool ok = rc == SQLITE_DONE;
    if (rc == SQLITE_ROW)
        ok = decode(p, c, sqlite3_column_blob(s, 0), (size_t)sqlite3_column_bytes(s, 0));
    if (rc != SQLITE_ROW && rc != SQLITE_DONE)
        error(p, sqlite3_errmsg(p->store->db));
    sqlite3_finalize(s);
    s = NULL;
    if (!ok) {
        release(c);
        return NULL;
    }
    budget(p, c);
    return c;
}
bool planet_store_open(Planet *p, const char *file) {
    p->store = calloc(1, sizeof(*p->store));
    if (!p->store)
        return error(p, "Out of memory");
    if (sqlite3_open_v2(file, &p->store->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK)
        return error(p, sqlite3_errmsg(p->store->db));
    sqlite3_busy_timeout(p->store->db, 1000);
    if (!sql(p, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;PRAGMA cache_size=-4096;PRAGMA mmap_size=0;"
                "CREATE TABLE IF NOT EXISTS config(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER,mass "
                "REAL,a REAL,b REAL,dem INTEGER);"
                "CREATE TABLE IF NOT EXISTS chunks(id INTEGER PRIMARY KEY,x INTEGER,y INTEGER,z INTEGER,data "
                "BLOB,UNIQUE(x,y,z));"
                "CREATE VIRTUAL TABLE IF NOT EXISTS chunk_index USING rtree_i32(id,x0,x1,y0,y1,z0,z1);"))
        return false;
    sqlite3_stmt *s = NULL;
    bool ok = sqlite3_prepare_v2(p->store->db, "INSERT OR IGNORE INTO config VALUES(1,1,?,?,?,?)", -1, &s,
                                 NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_double(s, 1, p->mass_kg);
        sqlite3_bind_double(s, 2, p->a_m);
        sqlite3_bind_double(s, 3, p->b_m);
        sqlite3_bind_int64(s, 4, p->earth.checksum);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    if (ok)
        ok = sqlite3_prepare_v2(p->store->db, "SELECT version,mass,a,b,dem FROM config WHERE id=1", -1, &s,
                                NULL) == SQLITE_OK;
    if (ok)
        ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1 &&
             sqlite3_column_double(s, 1) == p->mass_kg && sqlite3_column_double(s, 2) == p->a_m &&
             sqlite3_column_double(s, 3) == p->b_m &&
             (uint32_t)sqlite3_column_int64(s, 4) == p->earth.checksum;
    sqlite3_finalize(s);
    s = NULL;
    if (!ok)
        return error(p, "Planet save parameters/data differ; use another --planet-save file");
    /* Old saves retain every absolute edit while their implicit terrain adopts
       the new height recipe. Subsequent opens bind that recipe/data pack. */
    if (!sql(p, "CREATE TABLE IF NOT EXISTS terrain_config(id INTEGER PRIMARY KEY CHECK(id=1),"
                "version INTEGER,checksum INTEGER);"))
        return false;
    if (sqlite3_prepare_v2(p->store->db, "INSERT OR IGNORE INTO terrain_config VALUES(1,6,?)", -1, &s,
                           NULL) != SQLITE_OK)
        return error(p, "Cannot bind terrain data");
    sqlite3_bind_int64(s, 1, p->terrain_checksum);
    ok = sqlite3_step(s) == SQLITE_DONE;
    sqlite3_finalize(s);
    s = NULL;
    if (ok) {
        ok = sqlite3_prepare_v2(
                 p->store->db,
                 "UPDATE terrain_config SET version=6,checksum=? WHERE id=1 AND version IN (2,3,4,5)", -1, &s,
                 NULL) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(s, 1, p->terrain_checksum);
            ok = sqlite3_step(s) == SQLITE_DONE;
        }
        sqlite3_finalize(s);
        s = NULL;
    }
    if (ok)
        ok = sqlite3_prepare_v2(p->store->db, "SELECT version,checksum FROM terrain_config WHERE id=1", -1,
                                &s, NULL) == SQLITE_OK;
    if (ok)
        ok = sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 6 &&
             (uint32_t)sqlite3_column_int64(s, 1) == p->terrain_checksum;
    sqlite3_finalize(s);
    s = NULL;
    if (!ok)
        return error(p, "Planet terrain pack/recipe differs; restore its data or use another save");
    if (sqlite3_prepare_v2(p->store->db, "SELECT count(*) FROM chunks", -1, &s, NULL) != SQLITE_OK ||
        sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        return error(p, "Cannot count planet chunks");
    }
    p->store->saved_count = (size_t)sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return true;
}
void planet_store_close(Planet *p) {
    if (!p->store)
        return;
    for (int i = 0; i < PLANET_CACHE_LIMIT; i++)
        release(&p->store->cache[i]);
    sqlite3_close(p->store->db);
    free(p->store);
    p->store = NULL;
}
bool planet_checkpoint(Planet *p) {
    int rc = sqlite3_wal_checkpoint_v2(p->store->db, NULL, SQLITE_CHECKPOINT_FULL, NULL, NULL);
    return rc == SQLITE_OK ? true : error(p, sqlite3_errmsg(p->store->db));
}
size_t planet_saved_chunks(const Planet *p) {
    return p->store ? p->store->saved_count : 0;
}
Voxel planet_voxel(Planet *p, int64_t x, int64_t y, int64_t z) {
    if (x < -2000000000000LL || x > 2000000000000LL || y < -2000000000000LL || y > 2000000000000LL ||
        z < -2000000000000LL || z > 2000000000000LL)
        return (Voxel){MAT_AIR, 0, 288.15f};
    CellCoord key = cell_coord(floor_div(x, 2000), floor_div(y, 2000), floor_div(z, 2000));
    Cache *c = get(p, key);
    if (!c)
        return (Voxel){MAT_AIR, 0, 288.15f};
    int local[3] = {(int)(x - key.x * 2000), (int)(y - key.y * 2000), (int)(z - key.z * 2000)};
    if (c->mask && detail_get(c->mask, local[0], local[1], local[2]).material != MAT_AIR)
        return detail_get(c->values, local[0], local[1], local[2]);
    return planet_base_voxel(p, dv3((x + .5) * .001, (y + .5) * .001, (z + .5) * .001));
}
static bool write_chunk(Planet *p, CellCoord key, DetailChunk *mask, DetailChunk *values) {
    size_t a = detail_encoded_size(mask), b = detail_encoded_size(values), n = a + b + 20;
    if (a > UINT32_MAX || b > UINT32_MAX || n > (size_t)sqlite3_limit(p->store->db, SQLITE_LIMIT_LENGTH, -1))
        return error(p, "Edited chunk exceeds SQLite record size");
    unsigned char *data = malloc(n);
    if (!data)
        return error(p, "Out of memory");
    memcpy(data, "PVED", 4);
    put32(data + 4, 1);
    put32(data + 8, (uint32_t)a);
    put32(data + 12, (uint32_t)b);
    size_t at = 20;
    detail_encode(mask, data, &at);
    detail_encode(values, data, &at);
    put32(data + 16, hash(data + 20, n - 20));
    sqlite3_stmt *s = NULL;
    bool ok = sqlite3_prepare_v2(p->store->db,
                                 "INSERT INTO chunks(x,y,z,data) VALUES(?,?,?,?) ON CONFLICT(x,y,z) DO "
                                 "UPDATE SET data=excluded.data",
                                 -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(s, 1, key.x);
        sqlite3_bind_int64(s, 2, key.y);
        sqlite3_bind_int64(s, 3, key.z);
        sqlite3_bind_blob(s, 4, data, (int)n, SQLITE_TRANSIENT);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    free(data);
    if (ok)
        ok = sqlite3_prepare_v2(p->store->db,
                                "INSERT OR REPLACE INTO chunk_index SELECT id,x,x,y,y,z,z FROM chunks WHERE "
                                "x=? AND y=? AND z=?",
                                -1, &s, NULL) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(s, 1, key.x);
        sqlite3_bind_int64(s, 2, key.y);
        sqlite3_bind_int64(s, 3, key.z);
        ok = sqlite3_step(s) == SQLITE_DONE;
    }
    sqlite3_finalize(s);
    s = NULL;
    return ok ? true : error(p, sqlite3_errmsg(p->store->db));
}
typedef struct {
    MmBox bounds;
    double center[3], radius2;
    int resolution;
    const DetailNode *old;
    Voxel replacement, inherited;
} SphereBuild;
static bool inherit_region(MmBox box, Voxel *value, int *axis, int *cut, const SphereBuild *s) {
    const DetailNode *node = s->old;
    while (node && node->child[0]) {
        if (box.hi[node->axis] <= node->cut)
            node = node->child[0];
        else if (box.lo[node->axis] >= node->cut)
            node = node->child[1];
        else {
            *axis = node->axis;
            *cut = node->cut;
            return true;
        }
    }
    *axis = -1;
    *value = node ? node->value : s->inherited;
    return true;
}
static bool sphere_region(MmBox box, Voxel *value, int *axis, int *cut, void *context) {
    const SphereBuild *s = context;
    for (int a = 0; a < 3; a++)
        if (box.hi[a] <= s->bounds.lo[a] || box.lo[a] >= s->bounds.hi[a])
            return inherit_region(box, value, axis, cut, s);
    /* First isolate the stroke bounds; adaptive leaves never spill outside. */
    for (int a = 0; a < 3; a++) {
        int plane = box.lo[a] < s->bounds.lo[a]   ? s->bounds.lo[a]
                    : box.hi[a] > s->bounds.hi[a] ? s->bounds.hi[a]
                                                  : box.lo[a];
        if (plane > box.lo[a] && plane < box.hi[a]) {
            *axis = a;
            *cut = plane;
            return true;
        }
    }
    double closest2 = 0, farthest2 = 0, center2 = 0;
    int longest = 0;
    for (int a = 0; a < 3; a++) {
        double lo = box.lo[a] + .5 - s->center[a], hi = box.hi[a] - .5 - s->center[a];
        double nearest = lo > 0 ? lo : hi < 0 ? hi : 0;
        closest2 += nearest * nearest;
        farthest2 += fmax(lo * lo, hi * hi);
        double mid = (lo + hi) * .5;
        center2 += mid * mid;
        if (box.hi[a] - box.lo[a] > box.hi[longest] - box.lo[longest])
            longest = a;
    }
    if (closest2 > s->radius2)
        return inherit_region(box, value, axis, cut, s);
    if (farthest2 <= s->radius2 || box.hi[longest] - box.lo[longest] <= s->resolution) {
        if (farthest2 > s->radius2 && center2 > s->radius2)
            return inherit_region(box, value, axis, cut, s);
        *axis = -1;
        *value = s->replacement;
        return true;
    }
    *axis = longest;
    *cut = (box.lo[longest] + box.hi[longest]) / 2;
    return true;
}
static bool paint(Planet *p, PlanetBox box, Voxel value, bool sphere) {
    if (!voxel_valid(value))
        return error(p, "Invalid voxel properties");
    int64_t lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        if (box.lo[a] < -2000000000000LL || box.hi[a] > 2000000000000LL || box.lo[a] >= box.hi[a] ||
            box.hi[a] - box.lo[a] > 1000)
            return error(p, "Brush bounds exceed 1 m edit budget");
        lo[a] = floor_div(box.lo[a], 2000);
        hi[a] = floor_div(box.hi[a] - 1, 2000);
    }
    DetailChunk *masks[8] = {0}, *values[8] = {0};
    CellCoord keys[8];
    int count = 0;
    size_t added = 0;
    bool transaction = false;
    for (int64_t z = lo[2]; z <= hi[2]; z++)
        for (int64_t y = lo[1]; y <= hi[1]; y++)
            for (int64_t x = lo[0]; x <= hi[0]; x++) {
                keys[count] = cell_coord(x, y, z);
                Cache *old = get(p, keys[count]);
                if (!old)
                    goto failed;
                if (!old->mask)
                    added++;
                int64_t origin[3] = {x * 2000, y * 2000, z * 2000};
                MmBox local;
                for (int a = 0; a < 3; a++) {
                    local.lo[a] = (int)(box.lo[a] > origin[a] ? box.lo[a] - origin[a] : 0);
                    local.hi[a] = (int)(box.hi[a] < origin[a] + 2000 ? box.hi[a] - origin[a] : 2000);
                }
                bool ok;
                if (sphere) {
                    int diameter = (int)(box.hi[0] - box.lo[0]);
                    SphereBuild shape = {.radius2 = diameter * (double)diameter * .25,
                                         .resolution = (diameter + 31) / 32,
                                         .old = old->mask ? old->mask->root : NULL,
                                         .replacement = marker(),
                                         .inherited = mask_air()};
                    for (int a = 0; a < 3; a++) {
                        shape.bounds.lo[a] = (int)(box.lo[a] - origin[a]);
                        shape.bounds.hi[a] = (int)(box.hi[a] - origin[a]);
                        shape.center[a] = shape.bounds.lo[a] + diameter * .5;
                    }
                    masks[count] = detail_build_unlimited(sphere_region, &shape);
                    shape.old = old->values ? old->values->root : NULL;
                    shape.replacement = value;
                    shape.inherited = (Voxel){MAT_AIR, 0, 288.15f};
                    values[count] = detail_build_unlimited(sphere_region, &shape);
                    ok = masks[count] && values[count];
                } else {
                    masks[count] = old->mask ? detail_clone(old->mask) : detail_create(mask_air());
                    values[count] =
                        old->values ? detail_clone(old->values) : detail_create((Voxel){MAT_AIR, 0, 288.15f});
                    if (masks[count])
                        masks[count]->unlimited = true;
                    if (values[count])
                        values[count]->unlimited = true;
                    ok = masks[count] && values[count] && detail_paint_build(masks[count], local, marker()) &&
                         detail_paint_build(values[count], local, value);
                }
                if (!ok) {
                    error(p, "Edit allocation failed or tree depth exceeded");
                    goto failed;
                }
                count++;
            }
    if (!sql(p, "BEGIN IMMEDIATE"))
        goto failed;
    transaction = true;
    for (int i = 0; i < count; i++)
        if (!write_chunk(p, keys[i], masks[i], values[i]))
            goto failed;
    if (!sql(p, "COMMIT"))
        goto failed;
    transaction = false;
    for (int i = 0; i < count; i++) {
        Cache *c = slot(p, keys[i]);
        detail_destroy(c->mask);
        detail_destroy(c->values);
        c->mask = masks[i];
        c->values = values[i];
        masks[i] = values[i] = NULL;
        budget(p, c);
    }
    p->store->saved_count += added;
    p->revision++;
    return true;
failed:
    if (transaction)
        sqlite3_exec(p->store->db, "ROLLBACK", NULL, NULL, NULL);
    for (int i = 0; i < 8; i++) {
        detail_destroy(masks[i]);
        detail_destroy(values[i]);
    }
    if (!p->error[0])
        error(p, "Out of memory during edit");
    return false;
}
bool planet_paint(Planet *p, PlanetBox box, Voxel value) {
    return paint(p, box, value, false);
}
bool planet_paint_sphere(Planet *p, PlanetBox box, Voxel value) {
    int64_t diameter;
    /* Validate before subtracting any untrusted endpoints. */
    for (int a = 0; a < 3; a++)
        if (box.lo[a] < -2000000000000LL || box.hi[a] > 2000000000000LL || box.lo[a] >= box.hi[a])
            return error(p, "Invalid sphere bounds");
    diameter = box.hi[0] - box.lo[0];
    for (int a = 0; a < 3; a++)
        if (box.hi[a] - box.lo[a] != diameter)
            return error(p, "Sphere bounds must have equal dimensions");
    return paint(p, box, value, true);
}
static bool overlap(MmBox a, MmBox b) {
    for (int i = 0; i < 3; i++)
        if (a.lo[i] >= b.hi[i] || a.hi[i] <= b.lo[i])
            return false;
    return true;
}
static bool append(PlanetEdits *out, PlanetBox b, Voxel v) {
    if (out->count == out->capacity) {
        if (out->capacity > SIZE_MAX / 2 / sizeof(*out->items))
            return false;
        size_t n = out->capacity ? out->capacity * 2 : 32;
        PlanetEdit *items = realloc(out->items, n * sizeof(*items));
        if (!items)
            return false;
        out->items = items;
        out->capacity = n;
    }
    out->items[out->count++] = (PlanetEdit){b, v};
    return true;
}
static bool values_visit(const DetailNode *node, MmBox bounds, MmBox mask, CellCoord key, PlanetEdits *out) {
    if (!overlap(bounds, mask))
        return true;
    if (!node->child[0]) {
        PlanetBox b;
        int64_t origin[3] = {key.x * 2000, key.y * 2000, key.z * 2000};
        for (int i = 0; i < 3; i++) {
            b.lo[i] = origin[i] + (bounds.lo[i] > mask.lo[i] ? bounds.lo[i] : mask.lo[i]);
            b.hi[i] = origin[i] + (bounds.hi[i] < mask.hi[i] ? bounds.hi[i] : mask.hi[i]);
        }
        return append(out, b, node->value);
    }
    MmBox low = bounds, high = bounds;
    low.hi[node->axis] = high.lo[node->axis] = node->cut;
    return values_visit(node->child[0], low, mask, key, out) &&
           values_visit(node->child[1], high, mask, key, out);
}
typedef struct {
    Cache *c;
    PlanetEdits *out;
} Visit;
static bool mask_visit(MmBox bounds, Voxel v, void *context) {
    Visit *visit = context;
    if (v.material == MAT_AIR)
        return true;
    return values_visit(visit->c->values->root, (MmBox){{0, 0, 0}, {2000, 2000, 2000}}, bounds, visit->c->key,
                        visit->out);
}
bool planet_collect(Planet *p, PlanetBox bounds, PlanetEdits *out) {
    out->count = 0;
    for (int a = 0; a < 3; a++)
        if (bounds.lo[a] >= bounds.hi[a] || bounds.lo[a] < -4000000000000LL || bounds.hi[a] > 4000000000000LL)
            return error(p, "Invalid planet query bounds");
    sqlite3_stmt *s = NULL;
    if (sqlite3_prepare_v2(p->store->db,
                           "SELECT c.x,c.y,c.z FROM chunk_index i JOIN chunks c ON c.id=i.id WHERE i.x1>=? "
                           "AND i.x0<=? AND i.y1>=? AND i.y0<=? AND i.z1>=? AND i.z0<=?",
                           -1, &s, NULL) != SQLITE_OK)
        return error(p, sqlite3_errmsg(p->store->db));
    for (int a = 0; a < 3; a++) {
        sqlite3_bind_int64(s, 2 * a + 1, floor_div(bounds.lo[a], 2000));
        sqlite3_bind_int64(s, 2 * a + 2, floor_div(bounds.hi[a] - 1, 2000));
    }
    int rc;
    /* Stream rows: decoding a chunk may evict older cached chunks, but collected
       values own their data. Nested read statements on this connection are safe. */
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        CellCoord key =
            cell_coord(sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2));
        Cache *c = get(p, key);
        if (!c) {
            sqlite3_finalize(s);
            return false;
        }
        Visit visit = {c, out};
        if (c->mask && !detail_visit(c->mask, mask_visit, &visit)) {
            sqlite3_finalize(s);
            return error(p, "Out of memory collecting visible edits");
        }
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE)
        return error(p, "Cannot query edited region");
    return true;
}
void planet_edits_destroy(PlanetEdits *e) {
    free(e->items);
    memset(e, 0, sizeof(*e));
}
size_t planet_edit_keys(Planet *p, PlanetBox bounds, DVec3 camera, CellCoord *keys, size_t capacity) {
    sqlite3_stmt *s = NULL;
    const char *query = "SELECT c.x,c.y,c.z FROM chunk_index i JOIN chunks c ON c.id=i.id "
                        "WHERE i.x1>=? AND i.x0<=? AND i.y1>=? AND i.y0<=? AND i.z1>=? AND i.z0<=? "
                        "ORDER BY (c.x-?)*(c.x-?)+(c.y-?)*(c.y-?)+(c.z-?)*(c.z-?) LIMIT ?";
    if (sqlite3_prepare_v2(p->store->db, query, -1, &s, NULL) != SQLITE_OK) {
        error(p, "Cannot plan edited chunk visibility");
        return SIZE_MAX;
    }
    for (int a = 0; a < 3; a++) {
        if (bounds.lo[a] >= bounds.hi[a] || bounds.lo[a] < -4000000000000LL ||
            bounds.hi[a] > 4000000000000LL) {
            sqlite3_finalize(s);
            error(p, "Invalid edit visibility bounds");
            return SIZE_MAX;
        }
        sqlite3_bind_int64(s, 2 * a + 1, floor_div(bounds.lo[a], 2000));
        sqlite3_bind_int64(s, 2 * a + 2, floor_div(bounds.hi[a] - 1, 2000));
    }
    double xyz[3] = {camera.x, camera.y, camera.z};
    for (int a = 0; a < 3; a++) {
        sqlite3_bind_double(s, 7 + a * 2, xyz[a] * .5 - .5);
        sqlite3_bind_double(s, 8 + a * 2, xyz[a] * .5 - .5);
    }
    sqlite3_bind_int64(s, 13, (sqlite3_int64)capacity);
    size_t count = 0;
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW)
        keys[count++] =
            cell_coord(sqlite3_column_int64(s, 0), sqlite3_column_int64(s, 1), sqlite3_column_int64(s, 2));
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        error(p, "Cannot enumerate edited chunks");
        return SIZE_MAX;
    }
    return count;
}
