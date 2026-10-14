#include "planet.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
    int64_t plane, lo[2], hi[2];
    int axis, sign;
    Vec3 color;
} Face;
static float coordinate(Vec3 v, int axis) {
    return axis == 0 ? v.x : axis == 1 ? v.y : v.z;
}
static int compare64(int64_t a, int64_t b) {
    return a < b ? -1 : a > b;
}
static int group(const Face *a, const Face *b) {
    int c = compare64(a->axis, b->axis);
    if (!c)
        c = compare64(a->sign, b->sign);
    if (!c)
        c = compare64(a->plane, b->plane);
    for (int i = 0; i < 3 && !c; i++) {
        float x = coordinate(a->color, i), y = coordinate(b->color, i);
        c = x < y ? -1 : x > y;
    }
    return c;
}
static int horizontal(const void *aa, const void *bb) {
    const Face *a = aa, *b = bb;
    int c = group(a, b);
    if (!c)
        c = compare64(a->lo[1], b->lo[1]);
    if (!c)
        c = compare64(a->hi[1], b->hi[1]);
    return c ? c : compare64(a->lo[0], b->lo[0]);
}
static int vertical(const void *aa, const void *bb) {
    const Face *a = aa, *b = bb;
    int c = group(a, b);
    if (!c)
        c = compare64(a->lo[0], b->lo[0]);
    if (!c)
        c = compare64(a->hi[0], b->hi[0]);
    return c ? c : compare64(a->lo[1], b->lo[1]);
}
static size_t merge(Face *faces, size_t count, int axis) {
    qsort(faces, count, sizeof(*faces), axis ? vertical : horizontal);
    size_t out = 0;
    int other = 1 - axis;
    for (size_t i = 0; i < count; i++) {
        Face *last = out ? &faces[out - 1] : NULL;
        if (last && !group(last, &faces[i]) && last->lo[other] == faces[i].lo[other] &&
            last->hi[other] == faces[i].hi[other] && last->hi[axis] == faces[i].lo[axis])
            last->hi[axis] = faces[i].hi[axis];
        else
            faces[out++] = faces[i];
    }
    return out;
}
static bool rectangle(const Vertex *v, Face *face) {
    int axis = -1, sign = 1;
    for (int a = 0; a < 3; a++) {
        float n = coordinate(v[0].normal, a);
        if (fabsf(n) > .9999f) {
            axis = a;
            sign = n > 0 ? 1 : -1;
        }
    }
    if (axis < 0)
        return false;
    int u = (axis + 1) % 3, w = (axis + 2) % 3;
    int64_t xyz[6][3];
    for (int i = 0; i < 6; i++) {
        if (memcmp(&v[i].normal, &v[0].normal, sizeof(Vec3)) ||
            memcmp(&v[i].color, &v[0].color, sizeof(Vec3)))
            return false;
        for (int a = 0; a < 3; a++) {
            double mm = coordinate(v[i].position, a) * 125.0;
            xyz[i][a] = (int64_t)llround(mm);
            if (fabs(mm - xyz[i][a]) > .001)
                return false;
        }
        if (xyz[i][axis] != xyz[0][axis])
            return false;
    }
    *face = (Face){.axis = axis, .sign = sign, .plane = xyz[0][axis], .color = v[0].color};
    face->lo[0] = face->hi[0] = xyz[0][u];
    face->lo[1] = face->hi[1] = xyz[0][w];
    for (int i = 1; i < 6; i++) {
        if (xyz[i][u] < face->lo[0])
            face->lo[0] = xyz[i][u];
        if (xyz[i][u] > face->hi[0])
            face->hi[0] = xyz[i][u];
        if (xyz[i][w] < face->lo[1])
            face->lo[1] = xyz[i][w];
        if (xyz[i][w] > face->hi[1])
            face->hi[1] = xyz[i][w];
    }
    if (face->lo[0] == face->hi[0] || face->lo[1] == face->hi[1])
        return false;
    unsigned corners[2] = {0};
    for (int i = 0; i < 6; i++) {
        if ((xyz[i][u] != face->lo[0] && xyz[i][u] != face->hi[0]) ||
            (xyz[i][w] != face->lo[1] && xyz[i][w] != face->hi[1]))
            return false;
        int corner = xyz[i][w] == face->lo[1] ? (xyz[i][u] == face->lo[0] ? 0 : 1)
                                              : (xyz[i][u] == face->lo[0] ? 3 : 2);
        unsigned bit = 1u << corner;
        if (corners[i / 3] & bit)
            return false;
        corners[i / 3] |= bit;
    }
    unsigned shared = corners[0] & corners[1];
    return (corners[0] | corners[1]) == 15 && (shared == 5 || shared == 10);
}
bool planet_mesh_greedy_edits(Mesh *m, CellCoord origin, int lod) {
    if (!m->count)
        return true;
    if (lod < 1)
        lod = 1;
    /* Rendering may coarsen vertices; authoritative voxel trees never change.
       Global grid alignment keeps adjacent edited chunks aligned at each LOD. */
    if (lod > 1) {
        int64_t base[3] = {origin.x * 125, origin.y * 125, origin.z * 125};
        size_t out = 0;
        for (size_t i = 0; i < m->count; i += 3) {
            Vertex triangle[3];
            for (int j = 0; j < 3; j++) {
                triangle[j] = m->vertices[i + j];
                double xyz[3];
                for (int a = 0; a < 3; a++) {
                    int64_t whole = base[a] / lod, remainder = base[a] % lod;
                    double snapped =
                        floor((remainder + coordinate(triangle[j].position, a) * 125.0) / lod + .5);
                    xyz[a] = ((whole * lod - base[a]) + snapped * lod) / 125.0;
                }
                triangle[j].position = v3((float)xyz[0], (float)xyz[1], (float)xyz[2]);
            }
            Vec3 cross = vcross(vsub(triangle[1].position, triangle[0].position),
                                vsub(triangle[2].position, triangle[0].position));
            if (vdot(cross, cross) > 1e-18f) {
                memcpy(m->vertices + out, triangle, sizeof(triangle));
                out += 3;
            }
        }
        m->count = out;
    }
    Face *faces = malloc((m->count / 6 + 1) * sizeof(*faces));
    if (!faces)
        return false;
    size_t count = 0, other = 0;
    for (size_t i = 0; i < m->count;) {
        if (i + 6 <= m->count && rectangle(m->vertices + i, &faces[count])) {
            count++;
            i += 6;
        } else {
            memmove(m->vertices + other, m->vertices + i, 3 * sizeof(Vertex));
            other += 3;
            i += 3;
        }
    }
    for (int pass = 0; pass < 8 && count; pass++) {
        size_t before = count;
        count = merge(faces, count, 0);
        count = merge(faces, count, 1);
        if (before == count)
            break;
    }
    m->count = other;
    const int triangles[6] = {0, 1, 2, 0, 2, 3};
    for (size_t i = 0; i < count; i++) {
        Face *f = &faces[i];
        int u = (f->axis + 1) % 3, w = (f->axis + 2) % 3;
        Vec3 positions[4],
            normal = v3(f->axis == 0 ? f->sign : 0, f->axis == 1 ? f->sign : 0, f->axis == 2 ? f->sign : 0);
        for (int j = 0; j < 4; j++) {
            double xyz[3];
            xyz[f->axis] = f->plane / 125.0;
            xyz[u] = (j == 1 || j == 2 ? f->hi[0] : f->lo[0]) / 125.0;
            xyz[w] = (j >= 2 ? f->hi[1] : f->lo[1]) / 125.0;
            positions[j] = v3((float)xyz[0], (float)xyz[1], (float)xyz[2]);
        }
        for (int j = 0; j < 6; j++) {
            int corner = triangles[f->sign > 0 ? j : j / 3 * 3 + (j % 3 ? 3 - j % 3 : 0)];
            m->vertices[m->count++] = (Vertex){positions[corner], normal, f->color};
        }
    }
    free(faces);
    return true;
}
