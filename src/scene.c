#include "scene.h"
#include <string.h>
#include <float.h>
static bool same(CellCoord a, CellCoord b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}
void scene_init(Scene *s) {
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < SCENE_SLOTS; i++) {
        s->entries[i].lod = -1;
        s->entries[i].base_chunk = i < CHUNK_COUNT ? i : -1;
    }
}
static double distance_box(WorldPosition camera, CellCoord origin, Vec3 size) {
    DVec3 r;
    if (!position_relative_cells(camera, origin, &r))
        return DBL_MAX;
    double p[3] = {r.x, r.y, r.z}, extent[3] = {size.x, size.y, size.z}, result = 0;
    for (int a = 0; a < 3; a++) {
        double d = p[a] < 0 ? -p[a] : p[a] > extent[a] ? p[a] - extent[a] : 0;
        result += d * d;
    }
    return sqrt(result) * VOXEL_SIZE_METERS;
}
bool scene_box_visible(WorldPosition camera, Vec3 direction, float aspect, float view_m, CellCoord origin,
                       Vec3 size) {
    DVec3 relative;
    if (!position_relative_cells(camera, origin, &relative) || aspect <= 0)
        return false;
    Vec3 center = v3((float)(size.x * .5 - relative.x), (float)(size.y * .5 - relative.y),
                     (float)(size.z * .5 - relative.z));
    float radius = sqrtf(vdot(size, size)) * .5f;
    Vec3 f = vnormalize(direction), right = vnormalize(vcross(f, v3(0, 1, 0))), up = vcross(right, f);
    if (vdot(right, right) < .5f) {
        right = v3(1, 0, 0);
        up = vcross(right, f);
    }
    float depth = vdot(center, f), x = vdot(center, right), y = vdot(center, up);
    float tangent = tanf(1.22173f * .5f), horizontal = tangent * aspect;
    return depth + radius >= 0 && depth - radius <= view_m / VOXEL_SIZE_METERS &&
           fabsf(x) <= depth * horizontal + radius * sqrtf(1 + horizontal * horizontal) &&
           fabsf(y) <= depth * tangent + radius * sqrtf(1 + tangent * tangent);
}
int scene_lod(double distance, int previous) {
    const double boundaries[] = {2, 12, 32, 96};
    int desired = 0;
    while (desired < 4 && distance > boundaries[desired])
        desired++;
    if (previous >= 0 && previous <= 4 && desired != previous) {
        if (desired > previous && distance < boundaries[previous] * 1.15)
            return previous;
        if (desired < previous && distance > boundaries[previous - 1] * .85)
            return previous;
    }
    return desired;
}
static bool request_tile(Scene *s, CellCoord origin, int size) {
    int slot = -1;
    for (int i = CHUNK_COUNT; i < SCENE_SLOTS; i++) {
        SceneEntry *e = &s->entries[i];
        if (e->size_cells == size && same(e->origin, origin)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = CHUNK_COUNT; i < SCENE_SLOTS; i++) {
            SceneEntry *e = &s->entries[i];
            if (!e->visible && (!e->size_cells || e->last_visible < oldest)) {
                slot = i;
                oldest = e->last_visible;
                if (!e->size_cells)
                    break;
            }
        }
        if (slot < 0)
            return false;
        SceneEntry *e = &s->entries[slot];
        e->origin = origin;
        e->size_cells = size;
        e->lod = -1;
        e->reset = true;
    }
    SceneEntry *e = &s->entries[slot];
    e->visible = true;
    e->last_visible = s->frame;
    e->target_lod = 0;
    s->visible++;
    return true;
}
static bool overlaps_map(const World *w, CellCoord o, int size) {
    int64_t x, z;
    if (!coord_sub(o.x, w->origin.x, &x) || !coord_sub(o.z, w->origin.z, &z))
        return false;
    return x < WORLD_X && z < WORLD_Z && x + size > 0 && z + size > 0;
}
static bool tiles(Scene *s, const World *w, WorldPosition camera, Vec3 d, float aspect, CellCoord o,
                  int size) {
    Vec3 bounds = v3((float)size, 40, (float)size);
    if (!scene_box_visible(camera, d, aspect, s->view_m, o, bounds) ||
        distance_box(camera, o, bounds) > s->view_m) {
        s->culled++;
        return true;
    }
    bool overlap = overlaps_map(w, o, size);
    if (size > 16 && (overlap || distance_box(camera, o, bounds) < size * VOXEL_SIZE_METERS * 2)) {
        int half = size / 2;
        for (int z = 0; z < 2; z++)
            for (int x = 0; x < 2; x++) {
                CellCoord child = o;
                if (!coord_add(o.x, x * half, &child.x) || !coord_add(o.z, z * half, &child.z))
                    continue;
                if (!tiles(s, w, camera, d, aspect, child, half))
                    return false;
            }
        return true;
    }
    if (overlap)
        return true; /* The editable map occupies aligned 2 m columns. */
    return request_tile(s, o, size);
}
bool scene_update(Scene *s, World *w, WorldPosition camera, Vec3 d, float aspect, float view_m,
                  bool dynamic) {
    if (!isfinite(view_m) || view_m < 32 || view_m > 1024 || !isfinite(aspect) || aspect <= 0 ||
        !isfinite(d.x) || !isfinite(d.y) || !isfinite(d.z) || vdot(vnormalize(d), vnormalize(d)) < .5f)
        return false;
    if (!s->initialized || s->seed != w->seed || !same(s->world_origin, w->origin)) {
        scene_init(s);
        s->initialized = true;
        s->seed = w->seed;
        s->world_origin = w->origin;
        for (int i = 0; i < SCENE_SLOTS; i++)
            s->entries[i].reset = true;
    }
    s->frame++;
    s->view_m = view_m;
    s->visible = s->culled = s->pending = 0;
    memset(s->lod_counts, 0, sizeof(s->lod_counts));
    for (int i = 0; i < SCENE_SLOTS; i++)
        s->entries[i].visible = false;
    RayHit focus = world_raycast(w, camera, d, 10 / VOXEL_SIZE_METERS);
    for (int c = 0; c < CHUNK_COUNT; c++) {
        SceneEntry *e = &s->entries[c];
        e->origin = world_chunk_origin(w, c);
        e->size_cells = 16;
        double distance = distance_box(camera, e->origin, v3(16, 16, 16));
        e->visible = !dynamic || (distance <= view_m &&
                                  scene_box_visible(camera, d, aspect, view_m, e->origin, v3(16, 16, 16)));
        e->target_lod = dynamic ? scene_lod(distance, e->target_lod) : 0;
        if (focus.hit && focus.fine && focus.chunk == c)
            e->target_lod = 0;
        if (e->visible) {
            e->last_visible = s->frame;
            s->visible++;
            s->lod_counts[e->target_lod]++;
        } else {
            s->culled++;
        }
        if (dynamic && (distance > 20 || (!e->visible && s->frame - e->last_visible > 120)))
            world_release_chunk(w, c);
    }
    if (dynamic) {
        const int root = 1024;
        uint64_t rx = ((uint64_t)camera.cell.x - (uint64_t)w->origin.x) & (root - 1);
        uint64_t rz = ((uint64_t)camera.cell.z - (uint64_t)w->origin.z) & (root - 1);
        CellCoord center = cell_coord(camera.cell.x, w->origin.y, camera.cell.z);
        if (!coord_add(center.x, -(int64_t)rx, &center.x) || !coord_add(center.z, -(int64_t)rz, &center.z))
            return false;
        int radius = (int)ceil(view_m / (root * VOXEL_SIZE_METERS)) + 1;
        for (int z = -radius; z <= radius; z++)
            for (int x = -radius; x <= radius; x++) {
                CellCoord o = center;
                if (!coord_add(center.x, (int64_t)x * root, &o.x) ||
                    !coord_add(center.z, (int64_t)z * root, &o.z) || o.x > INT64_MAX - root ||
                    o.z > INT64_MAX - root)
                    continue;
                if (!tiles(s, w, camera, d, aspect, o, root))
                    return false;
            }
    }
    for (int i = 0; i < SCENE_SLOTS; i++) {
        SceneEntry *e = &s->entries[i];
        if (e->visible && (e->lod != e->target_lod || (i < CHUNK_COUNT && w->dirty[i])))
            s->pending++;
        if (i >= CHUNK_COUNT && !e->visible && e->size_cells && s->frame - e->last_visible > 120) {
            e->size_cells = 0;
            e->lod = -1;
            e->reset = true;
        }
    }
    return true;
}
