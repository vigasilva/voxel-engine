#include "planet.h"
#include "solar.h"
#include <float.h>
static PlanetBox ray_bounds(DVec3 a, DVec3 b) {
    PlanetBox box;
    double p[3] = {a.x, a.y, a.z}, q[3] = {b.x, b.y, b.z};
    for (int i = 0; i < 3; i++) {
        box.lo[i] = (int64_t)floor(fmin(p[i], q[i]) * 1000) - 2;
        box.hi[i] = (int64_t)ceil(fmax(p[i], q[i]) * 1000) + 3;
    }
    return box;
}
static bool contains(PlanetBox b, int64_t mm[3]) {
    for (int i = 0; i < 3; i++)
        if (mm[i] < b.lo[i] || mm[i] >= b.hi[i])
            return false;
    return true;
}
static bool intersection(PlanetBox b, DVec3 origin, DVec3 dir, double *enter, double *exit, Vec3 *normal,
                         Vec3 *exit_normal) {
    double o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z};
    *enter = 0;
    *exit = DBL_MAX;
    *normal = *exit_normal = v3(0, 0, 0);
    for (int a = 0; a < 3; a++) {
        double lo = b.lo[a] * .001, hi = b.hi[a] * .001;
        if (fabs(d[a]) < 1e-15) {
            if (o[a] < lo || o[a] >= hi)
                return false;
            continue;
        }
        double t0 = (lo - o[a]) / d[a], t1 = (hi - o[a]) / d[a];
        int sign = -1;
        if (t0 > t1) {
            double t = t0;
            t0 = t1;
            t1 = t;
            sign = 1;
        }
        if (t0 > *enter) {
            *enter = t0;
            float n[3] = {0};
            n[a] = (float)sign;
            *normal = v3(n[0], n[1], n[2]);
        }
        if (t1 < *exit) {
            *exit = t1;
            float n[3] = {0};
            n[a] = (float)-sign;
            *exit_normal = v3(n[0], n[1], n[2]);
        }
        if (*enter > *exit)
            return false;
    }
    return *exit >= 0;
}
static bool blocks(Voxel v, bool water) {
    return material_get(v.material)->solid || (water && v.material != MAT_AIR);
}
static double terrain_distance(const Planet *p, DVec3 point, bool water) {
    double r = pd_length(point);
    if (r < fmin(p->a_m, p->b_m) - 12000)
        return r - fmin(p->a_m, p->b_m);
    PlanetGeo g = planet_geographic(p, point);
    double h = planet_elevation(p, g.latitude_deg, g.longitude_deg);
    return g.altitude_m - (water ? planet_visible_elevation(p, g.latitude_deg, g.longitude_deg) : h);
}
static PlanetHit hit_at(Planet *p, DVec3 origin, DVec3 direction, double t, Vec3 normal, bool water) {
    PlanetHit hit = {0};
    double cosine = fabs(pd_dot(direction, dv3(normal.x, normal.y, normal.z)));
    double margin = .002 / fmax(.05, cosine), distance = fmax(0, t - margin), limit = t + margin;
    for (int attempt = 0; attempt < 128 && distance <= limit; attempt++) {
        DVec3 point = pd_add(origin, pd_scale(direction, distance));
        int64_t mm[3] = {(int64_t)floor(point.x * 1000), (int64_t)floor(point.y * 1000),
                         (int64_t)floor(point.z * 1000)};
        PlanetBox box;
        for (int a = 0; a < 3; a++) {
            box.lo[a] = mm[a];
            box.hi[a] = mm[a] + 1;
        }
        double enter, leave;
        Vec3 n, en;
        if (!intersection(box, origin, direction, &enter, &leave, &n, &en))
            break;
        Voxel value = planet_voxel(p, mm[0], mm[1], mm[2]);
        if (blocks(value, water)) {
            hit.hit = true;
            hit.distance_m = enter;
            hit.point_m = pd_add(origin, pd_scale(direction, enter));
            hit.normal = normal;
            hit.brush_normal = vdot(n, n) > .5 ? n : normal;
            hit.value = value;
            for (int a = 0; a < 3; a++)
                hit.mm[a] = mm[a];
            return hit;
        }
        double next = leave + .00000001;
        if (next <= distance)
            break;
        distance = next;
    }
    return hit;
}
static PlanetHit raycast(Planet *p, DVec3 origin, DVec3 direction, double maximum, bool water) {
    PlanetHit miss = {0};
    if (!isfinite(maximum) || maximum < 0 || maximum > 1e9 || !isfinite(origin.x) || !isfinite(origin.y) ||
        !isfinite(origin.z) || fabs(origin.x) > 2e9 || fabs(origin.y) > 2e9 || fabs(origin.z) > 2e9 ||
        !isfinite(direction.x) || !isfinite(direction.y) || !isfinite(direction.z))
        return miss;
    DVec3 d = pd_normalize(direction);
    if (pd_length(d) < .5)
        return miss;
    PlanetEdits edits = {0};
    if (!planet_collect(p, ray_bounds(origin, pd_add(origin, pd_scale(d, maximum))), &edits)) {
        planet_edits_destroy(&edits);
        return miss;
    }
    double best = maximum + 1;
    Vec3 best_normal = {0};
    for (size_t i = 0; i < edits.count; i++)
        if (blocks(edits.items[i].value, water)) {
            double t0, t1;
            Vec3 n, en;
            if (intersection(edits.items[i].box, origin, d, &t0, &t1, &n, &en) && t0 < best) {
                best = t0;
                best_normal = n;
            }
        }
    double t = 0, previous = 0;
    Vec3 cavity_normal = {0};
    PlanetHit result = miss;
    for (int iteration = 0; iteration < 32768 && t <= maximum; iteration++) {
        if (best <= t + .00000001) {
            result = hit_at(p, origin, d, best, best_normal, water);
            break;
        }
        DVec3 point = pd_add(origin, pd_scale(d, t));
        int64_t mm[3] = {(int64_t)floor(point.x * 1000), (int64_t)floor(point.y * 1000),
                         (int64_t)floor(point.z * 1000)};
        bool removed = false;
        for (size_t i = 0; i < edits.count; i++)
            if (!blocks(edits.items[i].value, water) && contains(edits.items[i].box, mm)) {
                double enter, leave;
                Vec3 n, en;
                if (intersection(edits.items[i].box, origin, d, &enter, &leave, &n, &en)) {
                    previous = t;
                    t = leave + .0000001;
                    cavity_normal = vscale(en, -1);
                    removed = true;
                    break;
                }
            }
        if (removed)
            continue;
        double distance = terrain_distance(p, point, water);
        if (distance <= 0) {
            if (vdot(cavity_normal, cavity_normal) > .5f)
                result = hit_at(p, origin, d, t, cavity_normal, water);
            else {
                double lo = previous, hi = t;
                for (int j = 0; j < 24 && hi - lo > .00000005; j++) {
                    double mid = (lo + hi) * .5;
                    if (terrain_distance(p, pd_add(origin, pd_scale(d, mid)), water) <= 0)
                        hi = mid;
                    else
                        lo = mid;
                }
                DVec3 up = planet_surface_normal(p, pd_add(origin, pd_scale(d, hi)));
                result = hit_at(p, origin, d, hi, v3((float)up.x, (float)up.y, (float)up.z), water);
            }
            break;
        }
        cavity_normal = v3(0, 0, 0);
        previous = t;
        double next = t + fmax(.00025, distance * .75);
        if (best < next)
            next = best;
        for (size_t i = 0; i < edits.count; i++)
            if (!blocks(edits.items[i].value, water)) {
                double enter, leave;
                Vec3 n, en;
                if (intersection(edits.items[i].box, origin, d, &enter, &leave, &n, &en) &&
                    enter > t + .00000001 && enter < next)
                    next = enter + .0000001;
            }
        if (next <= t)
            break;
        t = next;
    }
    if (!result.hit && best <= maximum)
        result = hit_at(p, origin, d, best, best_normal, water);
    planet_edits_destroy(&edits);
    if (result.distance_m > maximum)
        return miss;
    return result;
}
PlanetHit planet_raycast(Planet *p, DVec3 origin, DVec3 direction, double maximum) {
    return raycast(p, origin, direction, maximum, false);
}
PlanetHit planet_pick(Planet *p, DVec3 origin, DVec3 direction, double maximum) {
    return raycast(p, origin, direction, maximum, true);
}
bool planet_brush(PlanetHit h, int edge, bool place, PlanetBox *b) {
    if (!h.hit || edge < 1 || edge > 1000)
        return false;
    Vec3 brush = vdot(h.brush_normal, h.brush_normal) > .5f ? h.brush_normal : h.normal;
    double n[3] = {brush.x, brush.y, brush.z};
    int axis = 0;
    for (int i = 1; i < 3; i++)
        if (fabs(n[i]) > fabs(n[axis]))
            axis = i;
    if (fabs(n[axis]) < .1)
        return false;
    for (int a = 0; a < 3; a++) {
        if (a == axis && n[a] > 0) {
            b->hi[a] = h.mm[a] + 1 + (place ? edge : 0);
            b->lo[a] = b->hi[a] - edge;
        } else if (a == axis) {
            b->lo[a] = h.mm[a] - (place ? edge : 0);
            b->hi[a] = b->lo[a] + edge;
        } else {
            b->lo[a] = h.mm[a] - edge / 2;
            b->hi[a] = b->lo[a] + edge;
        }
    }
    return true;
}
bool planet_sphere_brush(PlanetHit hit, int diameter, bool place, PlanetBox *out) {
    if (!hit.hit || diameter < 1 || diameter > 1000)
        return false;
    /* A 1 mm sphere is one cell. Keep the exact ray-entry cell/adjacency. */
    if (diameter == 1)
        return planet_brush(hit, diameter, place, out);
    DVec3 normal = pd_normalize(dv3(hit.normal.x, hit.normal.y, hit.normal.z));
    DVec3 center = pd_scale(hit.point_m, 1000);
    if (place)
        center = pd_add(center, pd_scale(normal, diameter * .5 - .5));
    double xyz[3] = {center.x, center.y, center.z};
    for (int a = 0; a < 3; a++) {
        if (!isfinite(xyz[a]) || fabs(xyz[a]) > 1999999999000.0)
            return false;
        out->lo[a] = (int64_t)llround(xyz[a] - diameter * .5);
        out->hi[a] = out->lo[a] + diameter;
    }
    return true;
}
static DVec3 physical_gravity(const Planet *p, DVec3 position, DVec3 velocity) {
    DVec3 g = planet_gravity(p, position);
    double radius = pd_length(position), gm = GRAVITATIONAL_CONSTANT * p->mass_kg;
    if (radius >= p->a_m && gm / (radius * LIGHT_SPEED_MPS * LIGHT_SPEED_MPS) < 1e-4 &&
        pd_length(velocity) < .01 * LIGHT_SPEED_MPS)
        g = pd_add(g, relativity_acceleration(gm, position, velocity));
    return g;
}
static void physics_step(Planet *p, PlanetBody *body, DVec3 requested, bool jump, double dt) {
    DVec3 old = planet_position_m(body->position), up = planet_up(p, old),
          g = physical_gravity(p, old, body->velocity_mps);
    double vertical = pd_dot(body->velocity_mps, up);
    DVec3 tangent = pd_sub(body->velocity_mps, pd_scale(up, vertical));
    if (body->grounded) {
        tangent = requested;
        if (jump) {
            vertical = 5;
            body->grounded = false;
        } else
            vertical = 0;
    } else if (pd_length(requested) > 0)
        tangent = pd_add(tangent, pd_scale(requested, dt * .4));
    body->velocity_mps = pd_add(tangent, pd_scale(up, vertical));
    DVec3 half = pd_add(body->velocity_mps, pd_scale(g, dt * .5));
    DVec3 delta = pd_scale(half, dt), next = pd_add(old, delta);
    double length = pd_length(delta);
    /* Sweep the feet and torso against exact override boxes / implicit terrain. */
    if (length > 1e-8) {
        DVec3 direction = pd_scale(delta, 1 / length);
        for (int i = 0; i < 2; i++) {
            DVec3 start = pd_sub(old, pd_scale(up, i ? 1.79 : .9));
            PlanetHit hit = planet_raycast(p, start, direction, length + .001);
            if (hit.hit && pd_dot(direction, dv3(hit.normal.x, hit.normal.y, hit.normal.z)) < -.1) {
                next = pd_add(old, pd_scale(direction, fmax(0, hit.distance_m - .001)));
                DVec3 n = dv3(hit.normal.x, hit.normal.y, hit.normal.z);
                double into = pd_dot(half, n);
                if (into < 0)
                    half = pd_sub(half, pd_scale(n, into));
                break;
            }
        }
    }
    body->grounded = false;
    DVec3 east = pd_normalize(dv3(-up.y, up.x, 0));
    if (pd_length(east) < .5)
        east = dv3(1, 0, 0);
    DVec3 north =
        dv3(up.y * east.z - up.z * east.y, up.z * east.x - up.x * east.z, up.x * east.y - up.y * east.x);
    double correction = -HUGE_VAL;
    for (int i = 0; i < 5; i++) {
        DVec3 offset = i == 1   ? pd_scale(east, .25)
                       : i == 2 ? pd_scale(east, -.25)
                       : i == 3 ? pd_scale(north, .25)
                       : i == 4 ? pd_scale(north, -.25)
                                : dv3(0, 0, 0);
        PlanetHit floor = planet_raycast(p, pd_add(next, offset), pd_scale(up, -1), 1.83);
        if (floor.hit && floor.distance_m < 1.815 && pd_dot(half, up) <= 0) {
            correction = fmax(correction, 1.802 - floor.distance_m);
            body->grounded = true;
        }
    }
    if (body->grounded) {
        next = pd_add(next, pd_scale(up, correction));
        double down = pd_dot(half, up);
        if (down < 0)
            half = pd_sub(half, pd_scale(up, down));
    }
    DVec3 acceleration = physical_gravity(p, next, half);
    body->velocity_mps = pd_add(half, pd_scale(acceleration, dt * .5));
    if (body->grounded) {
        double down = pd_dot(body->velocity_mps, up);
        if (down < 0)
            body->velocity_mps = pd_sub(body->velocity_mps, pd_scale(up, down));
    }
    planet_set_position(next, &body->position);
}
void planet_body_step(Planet *p, PlanetBody *b, DVec3 requested, bool jump, double elapsed) {
    if (!isfinite(elapsed) || elapsed < 0)
        return;
    b->accumulator += fmin(elapsed, .1);
    b->jump_pending |= jump;
    while (b->accumulator >= 1.0 / 120) {
        physics_step(p, b, requested, b->jump_pending, 1.0 / 120);
        b->jump_pending = false;
        b->accumulator -= 1.0 / 120;
    }
}
