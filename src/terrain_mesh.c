#include "scene.h"
#include "terrain.h"
#include <stdlib.h>
static bool reserve_tile(Mesh *m, size_t count) {
    if (count <= m->capacity)
        return true;
    Vertex *v = realloc(m->vertices, count * sizeof(*v));
    if (!v)
        return false;
    m->vertices = v;
    m->capacity = count;
    return true;
}
static void triangle(Mesh *m, Vec3 a, Vec3 b, Vec3 c, Vec3 color) {
    Vec3 normal = vnormalize(vcross(vsub(b, a), vsub(c, a)));
    m->vertices[m->count++] = (Vertex){a, normal, color};
    m->vertices[m->count++] = (Vertex){b, normal, color};
    m->vertices[m->count++] = (Vertex){c, normal, color};
}
bool mesh_build_terrain_tile(Mesh *m, const World *w, CellCoord origin, int size) {
    if (size < 16 || size > 1024 || size % 16)
        return false;
    /* Fixed 16x16 patches: generation cost stays bounded as tile area grows. */
    if (!reserve_tile(m, (16 * 16 + 4 * 16) * 6))
        return false;
    m->count = 0;
    m->unit_faces = 0;
    m->surface_area_mm2 = 0;
    Vec3 p[17][17];
    int step = size / 16;
    for (int z = 0; z <= 16; z++)
        for (int x = 0; x <= 16; x++) {
            CellCoord cell = origin;
            if (!coord_add(origin.x, x * step, &cell.x) || !coord_add(origin.z, z * step, &cell.z))
                return false;
            p[z][x] = v3((float)(x * step), (float)terrain_height_mm(w, cell, 0, 0) / 125, (float)(z * step));
        }
    int64_t dz;
    uint16_t material = coord_sub(origin.z, w->origin.z, &dz) && dz < 20 ? MAT_SAND : MAT_GRASS;
    const Material *mat = material_get(material);
    Vec3 color = v3(mat->color[0], mat->color[1], mat->color[2]);
    for (int z = 0; z < 16; z++)
        for (int x = 0; x < 16; x++) {
            triangle(m, p[z][x], p[z + 1][x], p[z + 1][x + 1], color);
            triangle(m, p[z][x], p[z + 1][x + 1], p[z][x + 1], color);
        }
    /* Downward skirts cover height mismatches across different sampling densities. */
    Vec3 earth = v3(.38f, .29f, .16f);
    for (int edge = 0; edge < 4; edge++)
        for (int i = 0; i < 16; i++) {
            Vec3 a, b;
            if (edge == 0) {
                a = p[i + 1][0];
                b = p[i][0];
            } else if (edge == 1) {
                a = p[i][16];
                b = p[i + 1][16];
            } else if (edge == 2) {
                a = p[0][i];
                b = p[0][i + 1];
            } else {
                a = p[16][i + 1];
                b = p[16][i];
            }
            Vec3 lowa = a, lowb = b;
            lowa.y -= 32;
            lowb.y -= 32;
            triangle(m, a, lowa, lowb, earth);
            triangle(m, a, lowb, b, earth);
        }
    return true;
}
