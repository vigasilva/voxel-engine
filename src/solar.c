#include "solar.h"
#include <string.h>
#define PI 3.14159265358979323846
static DVec3 cross(DVec3 a, DVec3 b) {
    return dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static DVec3 equatorial(DVec3 v) {
    double e = 23.43928 * PI / 180;
    return dv3(v.x, v.y * cos(e) - v.z * sin(e), v.y * sin(e) + v.z * cos(e));
}
DVec3 relativity_acceleration(double gm, DVec3 r, DVec3 v) {
    double distance = pd_length(r);
    if (!(distance > 0) || !(gm > 0))
        return dv3(0, 0, 0);
    double c2 = LIGHT_SPEED_MPS * LIGHT_SPEED_MPS;
    return pd_scale(pd_add(pd_scale(r, 4 * gm / distance - pd_dot(v, v)), pd_scale(v, 4 * pd_dot(r, v))),
                    gm / (c2 * distance * distance * distance));
}
double relativity_gamma(double speed) {
    if (!isfinite(speed) || fabs(speed) >= LIGHT_SPEED_MPS)
        return INFINITY;
    double b = speed / LIGHT_SPEED_MPS;
    return 1 / sqrt((1 - b) * (1 + b));
}
DVec3 relativity_add_velocity(DVec3 frame, DVec3 local) {
    double speed = pd_length(frame);
    if (speed == 0)
        return local;
    double gamma = relativity_gamma(speed);
    if (!isfinite(gamma) || pd_length(local) >= LIGHT_SPEED_MPS)
        return dv3(NAN, NAN, NAN);
    DVec3 parallel = pd_scale(frame, pd_dot(frame, local) / (speed * speed));
    DVec3 perpendicular = pd_sub(local, parallel);
    double denominator = 1 + pd_dot(frame, local) / (LIGHT_SPEED_MPS * LIGHT_SPEED_MPS);
    return pd_scale(pd_add(pd_add(frame, parallel), pd_scale(perpendicular, 1 / gamma)), 1 / denominator);
}
double relativity_clock_rate(double potential, double speed) {
    if (!isfinite(potential) || !isfinite(speed) || fabs(speed) >= LIGHT_SPEED_MPS)
        return 0;
    double c2 = LIGHT_SPEED_MPS * LIGHT_SPEED_MPS;
    return sqrt(fmax(0, 1 + 2 * potential / c2 - (speed / LIGHT_SPEED_MPS) * (speed / LIGHT_SPEED_MPS)));
}
void solar_init(SolarSystem *s) {
    memset(s, 0, sizeof(*s));
    s->relativistic = true;
    const char *names[] = {"SUN",     "MERCURY", "VENUS",  "EARTH",  "MARS",
                           "JUPITER", "SATURN",  "URANUS", "NEPTUNE"};
    const double masses[] = {SOLAR_GM / GRAVITATIONAL_CONSTANT,
                             .330103e24,
                             4.86731e24,
                             EARTH_MASS_KG,
                             .641691e24,
                             1898.125e24,
                             568.317e24,
                             86.8099e24,
                             102.4092e24};
    const double radii[] = {696340e3, 2439.4e3, 6051.8e3, EARTH_A_METERS, 3389.5e3,
                            69911e3,  58232e3,  25362e3,  24622e3};
    const Vec3 colors[] = {{1, .93f, .68f},    {.52f, .49f, .46f}, {.82f, .73f, .52f},
                           {.15f, .35f, .6f},  {.65f, .28f, .12f}, {.73f, .61f, .48f},
                           {.80f, .73f, .55f}, {.40f, .74f, .78f}, {.18f, .32f, .77f}};
    /* JPL approximate elements, epoch J2000. Earth uses Earth-Moon barycenter. */
    const double elements[8][6] = {
        {.38709927, .20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593},
        {.72333566, .00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255},
        {1.00000261, .01671123, -.00001531, 100.46457166, 102.93768193, 0},
        {1.52371034, .09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891},
        {5.202887, .04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909},
        {9.53667594, .05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448},
        {19.18916464, .04725744, .77263783, 313.23810451, 170.95427630, 74.01692503},
        {30.06992276, .00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574}};
    for (int i = 0; i < SOLAR_COUNT; i++) {
        SolarBody *b = &s->bodies[i];
        b->name = names[i];
        b->mass_kg = masses[i];
        b->radius_m = radii[i];
        b->gm = GRAVITATIONAL_CONSTANT * masses[i];
        b->color = colors[i];
        if (!i)
            continue;
        const double *el = elements[i - 1];
        double a = el[0] * ASTRONOMICAL_UNIT_M, e = el[1], inc = el[2] * PI / 180,
               w = (el[4] - el[5]) * PI / 180, node = el[5] * PI / 180,
               M = remainder((el[3] - el[4]) * PI / 180, 2 * PI), E = M;
        for (int k = 0; k < 12; k++)
            E -= (E - e * sin(E) - M) / (1 - e * cos(E));
        DVec3 px = dv3(cos(node) * cos(w) - sin(node) * sin(w) * cos(inc),
                       sin(node) * cos(w) + cos(node) * sin(w) * cos(inc), sin(w) * sin(inc));
        DVec3 py = cross(dv3(sin(inc) * sin(node), -sin(inc) * cos(node), cos(inc)), px);
        b->position_m =
            equatorial(pd_add(pd_scale(px, a * (cos(E) - e)), pd_scale(py, a * sqrt(1 - e * e) * sin(E))));
        double factor = sqrt(SOLAR_GM / a) / (1 - e * cos(E));
        b->velocity_mps = equatorial(
            pd_add(pd_scale(px, -factor * sin(E)), pd_scale(py, factor * sqrt(1 - e * e) * cos(E))));
        b->semimajor_m = a;
        b->eccentricity = e;
    }
    DVec3 center = dv3(0, 0, 0), velocity = dv3(0, 0, 0);
    double mass = 0;
    for (int i = 0; i < SOLAR_COUNT; i++) {
        mass += masses[i];
        center = pd_add(center, pd_scale(s->bodies[i].position_m, masses[i]));
        velocity = pd_add(velocity, pd_scale(s->bodies[i].velocity_mps, masses[i]));
    }
    center = pd_scale(center, 1 / mass);
    velocity = pd_scale(velocity, 1 / mass);
    for (int i = 0; i < SOLAR_COUNT; i++) {
        s->bodies[i].position_m = pd_sub(s->bodies[i].position_m, center);
        s->bodies[i].velocity_mps = pd_sub(s->bodies[i].velocity_mps, velocity);
    }
}
static void acceleration(const SolarSystem *s, const DVec3 *p, const DVec3 *v, DVec3 *out) {
    for (int i = 0; i < SOLAR_COUNT; i++) {
        out[i] = dv3(0, 0, 0);
        for (int j = 0; j < SOLAR_COUNT; j++)
            if (i != j) {
                DVec3 d = pd_sub(p[j], p[i]);
                double r = pd_length(d);
                if (r > 0)
                    out[i] = pd_add(out[i], pd_scale(d, s->bodies[j].gm / (r * r * r)));
            }
    }
    if (s->relativistic)
        for (int i = 1; i < SOLAR_COUNT; i++) {
            DVec3 correction =
                relativity_acceleration(s->bodies[0].gm, pd_sub(p[i], p[0]), pd_sub(v[i], v[0]));
            out[i] = pd_add(out[i], correction);
            out[0] = pd_sub(out[0], pd_scale(correction, s->bodies[i].mass_kg / s->bodies[0].mass_kg));
        }
}
double solar_potential(const SolarSystem *s, DVec3 p, int exclude) {
    double phi = 0;
    for (int i = 0; i < SOLAR_COUNT; i++)
        if (i != exclude) {
            const SolarBody *b = &s->bodies[i];
            double r = pd_length(pd_sub(p, b->position_m));
            if (r >= b->radius_m)
                phi -= b->gm / r;
            else
                phi -= b->gm / (2 * b->radius_m) * (3 - r * r / (b->radius_m * b->radius_m));
        }
    return phi;
}
bool solar_step(SolarSystem *s, double seconds) {
    if (!isfinite(seconds) || seconds < 0 || seconds > 31557600)
        return false;
    int steps = (int)ceil(seconds / 3600);
    if (steps < 1)
        return true;
    double h = seconds / steps;
    for (int n = 0; n < steps; n++) {
        DVec3 p[9], v[9], q[9], w[9], kp[4][9], kv[4][9];
        for (int i = 0; i < 9; i++) {
            p[i] = s->bodies[i].position_m;
            v[i] = s->bodies[i].velocity_mps;
        }
        for (int k = 0; k < 4; k++) {
            double t = k == 3 ? h : h * .5;
            for (int i = 0; i < 9; i++) {
                q[i] = k ? pd_add(p[i], pd_scale(kp[k - 1][i], t)) : p[i];
                w[i] = k ? pd_add(v[i], pd_scale(kv[k - 1][i], t)) : v[i];
                kp[k][i] = w[i];
            }
            acceleration(s, q, w, kv[k]);
        }
        for (int i = 0; i < 9; i++) {
            DVec3 dp =
                pd_add(pd_add(kp[0][i], pd_scale(kp[1][i], 2)), pd_add(pd_scale(kp[2][i], 2), kp[3][i]));
            DVec3 dv =
                pd_add(pd_add(kv[0][i], pd_scale(kv[1][i], 2)), pd_add(pd_scale(kv[2][i], 2), kv[3][i]));
            s->bodies[i].position_m = pd_add(p[i], pd_scale(dp, h / 6));
            s->bodies[i].velocity_mps = pd_add(v[i], pd_scale(dv, h / 6));
            double rate = relativity_clock_rate(solar_potential(s, s->bodies[i].position_m, i),
                                                pd_length(s->bodies[i].velocity_mps));
            s->bodies[i].proper_time_s += h * rate;
        }
        s->time_s += h;
    }
    return true;
}
DVec3 solar_sun_direction(const SolarSystem *s, DVec3 local) {
    return pd_normalize(pd_sub(pd_sub(s->bodies[0].position_m, s->bodies[SOLAR_EARTH].position_m), local));
}
