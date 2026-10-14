#include "planet.h"
#include <string.h>
#include <float.h>
#include <stdio.h>
DVec3 planet_cube_direction(int face, double u, double v) {
    DVec3 p;
    switch (face) {
    case 0:
        p = dv3(1, v, -u);
        break;
    case 1:
        p = dv3(-1, v, u);
        break;
    case 2:
        p = dv3(u, 1, -v);
        break;
    case 3:
        p = dv3(u, -1, v);
        break;
    case 4:
        p = dv3(u, v, 1);
        break;
    default:
        p = dv3(-u, v, -1);
        break;
    }
    return pd_normalize(p);
}
DVec3 planet_surface_point(const Planet *p, DVec3 direction, bool ocean) {
    double r = 1 / sqrt((direction.x * direction.x + direction.y * direction.y) / (p->a_m * p->a_m) +
                        direction.z * direction.z / (p->b_m * p->b_m));
    PlanetGeo g = planet_geographic(p, pd_scale(direction, r));
    double h = planet_elevation(p, g.latitude_deg, g.longitude_deg);
    double visible = h;
    if (ocean) {
        bool water;
        if (planet_detail_sample(p, g.latitude_deg, g.longitude_deg, NULL, &water) && water)
            visible = planet_visible_elevation(p, g.latitude_deg, g.longitude_deg);
        else
            visible = planet_measured_elevation(p, g.latitude_deg, g.longitude_deg) < 0 ? fmax(0, h) : h;
    }
    return planet_from_geographic(p, g.latitude_deg, g.longitude_deg, visible);
}
void planet_scene_init(PlanetScene *s) {
    memset(s, 0, sizeof(*s));
    s->viewport_height = 800;
}
static bool key_equal(const PlanetPatch *a, const PlanetPatch *b) {
    return a->edited == b->edited &&
           (a->edited ? (a->edit_key.x == b->edit_key.x && a->edit_key.y == b->edit_key.y &&
                         a->edit_key.z == b->edit_key.z)
                      : (a->face == b->face && a->level == b->level && a->x == b->x && a->y == b->y));
}
static bool request(PlanetScene *s, PlanetPatch patch) {
    int slot = -1;
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
        if (s->patches[i].span_m && key_equal(&s->patches[i], &patch)) {
            slot = i;
            break;
        }
    if (slot < 0) {
        uint64_t oldest = UINT64_MAX;
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
            if (!s->patches[i].visible && (!s->patches[i].span_m || s->patches[i].last_visible < oldest)) {
                slot = i;
                oldest = s->patches[i].last_visible;
                if (!s->patches[i].span_m)
                    break;
            }
        if (slot < 0)
            return false;
        patch.reset = true;
        patch.ready = false;
        s->patches[slot] = patch;
    }
    PlanetPatch *e = &s->patches[slot];
    if (!e->visible)
        s->visible++;
    e->center_m = patch.center_m;
    e->radius_m = patch.radius_m;
    e->edit_lod_mm = patch.edit_lod_mm;
    e->river_lod = patch.river_lod;
    e->terrain_lod = patch.terrain_lod;
    e->visible = true;
    e->last_visible = s->frame;
    return true;
}
typedef struct {
    PlanetScene *scene;
    PlanetPatch patches[PLANET_PATCH_LIMIT];
    size_t count;
} PatchPlan;
static bool visible(DVec3 camera, Vec3 d, Vec3 up, float aspect, double view, DVec3 center, double radius) {
    DVec3 r = pd_sub(center, camera);
    Vec3 f = vnormalize(d), right = vnormalize(vcross(f, up)), u = vcross(right, f);
    if (vdot(right, right) < .5f) {
        right = vnormalize(vcross(f, fabsf(f.z) < .9f ? v3(0, 0, 1) : v3(0, 1, 0)));
        u = vcross(right, f);
    }
    double depth = r.x * f.x + r.y * f.y + r.z * f.z, x = r.x * right.x + r.y * right.y + r.z * right.z,
           y = r.x * u.x + r.y * u.y + r.z * u.z;
    double tangent = tan(1.22173 * .5), horizontal = tangent * aspect;
    return depth + radius >= 0 && depth - radius <= view &&
           fabs(x) <= depth * horizontal + radius * sqrt(1 + horizontal * horizontal) &&
           fabs(y) <= depth * tangent + radius * sqrt(1 + tangent * tangent);
}
static bool visit(PatchPlan *plan, const Planet *p, DVec3 camera, Vec3 d, Vec3 up, float aspect, double view,
                  double refinement, int face, int level, uint32_t x, uint32_t y) {
    PlanetScene *s = plan->scene;
    double size = 2.0 / (1u << level), u = -1 + (x + .5) * size, v = -1 + (y + .5) * size;
    DVec3 direction = planet_cube_direction(face, u, v), center = planet_surface_point(p, direction, true);
    double span = p->a_m * size, radius = span * 1.5 + .01, distance = pd_length(pd_sub(center, camera));
    double camera_radius = pd_length(camera), rmin = p->b_m - 12000 * p->a_m / EARTH_A_METERS;
    if (camera_radius > rmin) {
        double angular = acos(fmax(-1, fmin(1, pd_dot(pd_normalize(camera), direction))));
        double beta = 0;
        for (int j = -1; j <= 1; j += 2)
            for (int i = -1; i <= 1; i += 2) {
                double a =
                    acos(fmax(-1, fmin(1, pd_dot(direction, planet_cube_direction(face, u + i * size * .5,
                                                                                  v + j * size * .5)))));
                if (a > beta)
                    beta = a;
            }
        if (angular > acos(fmin(1, rmin / camera_radius)) + beta + .002)
            return true;
    }
    if (distance - radius > view || !visible(camera, d, up, aspect, view, center, radius))
        return true;
    /* Keep enough samples to recognize real coastlines even from space. */
    if (level < 3 || (level < 21 && distance < span * refinement)) {
        for (uint32_t dy = 0; dy < 2; dy++)
            for (uint32_t dx = 0; dx < 2; dx++)
                if (!visit(plan, p, camera, d, up, aspect, view, refinement, face, level + 1, x * 2 + dx,
                           y * 2 + dy))
                    return false;
        return true;
    }
    WorldPosition origin;
    if (!planet_set_position(center, &origin))
        return false;
    double meters_per_pixel = fmax(s->river_clearance_m, fmax(0, distance - radius)) * s->river_pixel_factor;
    int river_lod = s->river_disabled ? PLANET_RIVER_DISABLED_LOD
                    : meters_per_pixel <= 1
                        ? 0
                        : (int)fmin(PLANET_RIVER_DISABLED_LOD, ceil(log2(meters_per_pixel)));
    /* Geometry only needs to reproduce relief visible at this distance.
       Quantized errors avoid rebuilding for every small camera movement. */
    double pixel_error = fmax(.00025, fmax(1, distance - span * .25) * s->river_pixel_factor);
    int terrain_lod = 1 + (int)fmin(23, floor(log2(pixel_error / .00025)));
    PlanetPatch patch = {.face = face,
                         .level = level,
                         .x = x,
                         .y = y,
                         .origin = origin.cell,
                         .center_m = center,
                         .radius_m = radius,
                         .span_m = span,
                         .river_lod = river_lod,
                         .terrain_lod = terrain_lod};
    if (plan->count == PLANET_PATCH_LIMIT)
        return false;
    plan->patches[plan->count++] = patch;
    return true;
}
bool planet_scene_update(PlanetScene *s, Planet *p, WorldPosition position, Vec3 d, Vec3 up, float aspect,
                         double view) {
    if (!isfinite(view) || view < 100 || view > 6e9 || !isfinite(aspect) || aspect <= 0)
        return false;
    s->frame++;
    WorldPosition previous = s->selection_camera;
    bool stationary = previous.cell.x == position.cell.x && previous.cell.y == position.cell.y &&
                      previous.cell.z == position.cell.z && previous.offset_mm.x == position.offset_mm.x &&
                      previous.offset_mm.y == position.offset_mm.y &&
                      previous.offset_mm.z == position.offset_mm.z;
    if (s->selection_valid && !s->pending && stationary && !memcmp(&d, &s->selection_direction, sizeof(d)) &&
        !memcmp(&up, &s->selection_up, sizeof(up)) && aspect == s->selection_aspect &&
        view == s->selection_view && s->viewport_height == s->selection_height &&
        p->revision == s->selection_revision && p->surface_revision == s->selection_surface_revision &&
        p->a_m == s->selection_a_m && p->b_m == s->selection_b_m &&
        p->terrain_checksum == s->selection_terrain_checksum && p->relief_enabled == s->selection_relief) {
        for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
            if (s->patches[i].visible)
                s->patches[i].last_visible = s->frame;
        return true;
    }
    s->planning_count++;
    s->visible = s->pending = 0;
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++)
        s->patches[i].visible = false;
    DVec3 camera = planet_position_m(position);
    double altitude = planet_geographic(p, camera).altitude_m;
    s->river_clearance_m = fmax(0, altitude - 12000 * p->a_m / EARTH_A_METERS);
    s->river_pixel_factor = 2 * tan(1.22173 * .5) / (s->viewport_height > 0 ? s->viewport_height : 800);
    s->river_disabled = 2 * planet_river_max_width(p) < s->river_clearance_m * s->river_pixel_factor * 1.5;
    /* Wide windows and unusual views coarsen instead of growing the working set. */
    bool planned = false;
    PatchPlan plan = {.scene = s};
    double refinement = 1.9;
    for (int attempt = 0; attempt < 5 && !planned; attempt++, refinement *= .75) {
        plan.count = 0;
        planned = true;
        for (int face = 0; face < 6; face++)
            if (!visit(&plan, p, camera, d, up, aspect, view, refinement, face, 0, 0, 0)) {
                planned = false;
                break;
            }
    }
    if (!planned) {
        snprintf(p->error, sizeof(p->error), "Planet visible-patch budget exceeded");
        return false;
    }
    /* Reserve all reusable slots before inserting new patches. A new request
       must not evict a cached patch that appears later in the traversal. */
    bool cached[PLANET_PATCH_LIMIT] = {false};
    for (size_t i = 0; i < plan.count; i++)
        for (int slot = 0; slot < PLANET_PATCH_LIMIT; slot++)
            if (s->patches[slot].span_m && key_equal(&s->patches[slot], &plan.patches[i])) {
                cached[i] = true;
                break;
            }
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < plan.count; i++)
            if (cached[i] == (pass == 0) && !request(s, plan.patches[i]))
                return false;
    PlanetBox nearby;
    double c[3] = {camera.x, camera.y, camera.z};
    for (int i = 0; i < 3; i++) {
        nearby.lo[i] = (int64_t)floor((c[i] - PLANET_EDIT_RANGE_M) * 1000);
        nearby.hi[i] = (int64_t)ceil((c[i] + PLANET_EDIT_RANGE_M) * 1000);
    }
    CellCoord keys[128];
    size_t count = planet_edit_keys(p, nearby, camera, keys, 128);
    if (count == SIZE_MAX)
        return false;
    for (size_t i = 0; i < count; i++) {
        CellCoord key = keys[i];
        DVec3 center =
            dv3((key.x * 2000 + 1000) * .001, (key.y * 2000 + 1000) * .001, (key.z * 2000 + 1000) * .001);
        double distance = pd_length(pd_sub(center, camera));
        if (distance > PLANET_EDIT_RANGE_M || !visible(camera, d, up, aspect, view, center, 1.8))
            continue;
        PlanetPatch patch = {.edited = true,
                             .edit_key = key,
                             .origin = cell_coord(key.x * 16, key.y * 16, key.z * 16),
                             .center_m = center,
                             .radius_m = 1.8,
                             .span_m = 2,
                             .edit_lod_mm = distance < 12   ? 1
                                            : distance < 32 ? 4
                                            : distance < 64 ? 16
                                                            : 32};
        if (!request(s, patch))
            break; /* Keep nearest edited meshes within the shared patch budget. */
    }
    for (int i = 0; i < PLANET_PATCH_LIMIT; i++) {
        PlanetPatch *e = &s->patches[i];
        if (e->visible &&
            (!e->ready || e->built_river_lod != e->river_lod || e->built_terrain_lod != e->terrain_lod ||
             e->built_surface_revision != p->surface_revision ||
             ((e->edited || e->span_m < PLANET_EDIT_SURFACE_SPAN_M) && e->built_revision != p->revision) ||
             (e->edited && e->built_edit_lod_mm != e->edit_lod_mm)))
            s->pending++;
    }
    s->selection_valid = true;
    s->selection_camera = position;
    s->selection_direction = d;
    s->selection_up = up;
    s->selection_aspect = aspect;
    s->selection_view = view;
    s->selection_height = s->viewport_height;
    s->selection_revision = p->revision;
    s->selection_surface_revision = p->surface_revision;
    s->selection_a_m = p->a_m;
    s->selection_b_m = p->b_m;
    s->selection_terrain_checksum = p->terrain_checksum;
    s->selection_relief = p->relief_enabled;
    return true;
}
