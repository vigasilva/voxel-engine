#include "planet.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
    Vec3 position, color;
    size_t index;
} SmoothVertex;
static int compare(const void *aa, const void *bb) {
    const SmoothVertex *a = aa, *b = bb;
    const float av[] = {a->position.x, a->position.y, a->position.z, a->color.x, a->color.y, a->color.z},
                bv[] = {b->position.x, b->position.y, b->position.z, b->color.x, b->color.y, b->color.z};
    for (int i = 0; i < 6; i++)
        if (av[i] != bv[i])
            return av[i] < bv[i] ? -1 : 1;
    return 0;
}
static bool natural(Vec3 color) {
    const uint16_t materials[] = {MAT_SOIL, MAT_GRASS, MAT_STONE, MAT_SAND, MAT_CLAY, MAT_ICE};
    for (size_t i = 0; i < sizeof(materials) / sizeof(materials[0]); i++) {
        const Material *m = material_get(materials[i]);
        if (color.x == m->color[0] && color.y == m->color[1] && color.z == m->color[2])
            return true;
    }
    return false;
}
bool planet_mesh_smooth_edits(Mesh *mesh) {
    size_t count = 0;
    for (size_t i = 0; i < mesh->count; i++)
        count += natural(mesh->vertices[i].color);
    if (!count)
        return true;
    if (count > SIZE_MAX / sizeof(SmoothVertex))
        return false;
    SmoothVertex *vertices = malloc(count * sizeof(*vertices));
    if (!vertices)
        return false;
    size_t next = 0;
    for (size_t i = 0; i < mesh->count; i++)
        if (natural(mesh->vertices[i].color))
            vertices[next++] = (SmoothVertex){mesh->vertices[i].position, mesh->vertices[i].color, i};
    qsort(vertices, count, sizeof(*vertices), compare);
    for (size_t first = 0; first < count;) {
        size_t last = first + 1;
        while (last < count && compare(&vertices[first], &vertices[last]) == 0)
            last++;
        Vec3 sum = v3(0, 0, 0);
        for (size_t i = first; i < last; i++) {
            Vec3 n = mesh->vertices[vertices[i].index].normal;
            bool duplicate = false;
            for (size_t j = first; j < i; j++) {
                Vec3 prior = mesh->vertices[vertices[j].index].normal;
                if (n.x == prior.x && n.y == prior.y && n.z == prior.z) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
                sum = vadd(sum, n);
        }
        if (vdot(sum, sum) > .001f) {
            Vec3 normal = vnormalize(sum);
            for (size_t i = first; i < last; i++)
                mesh->vertices[vertices[i].index].normal = normal;
        }
        first = last;
    }
    free(vertices);
    return true;
}
