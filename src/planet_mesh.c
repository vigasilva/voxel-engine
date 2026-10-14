#include "planet.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
typedef struct {
    DVec3 p[16];
    int count;
    Vec3 color, normal;
} Polygon;
typedef struct {
    PlanetBox bounds;
    ptrdiff_t first, count, child[2];
} EditNode;
typedef struct {
    PlanetEdits *edits;
    EditNode *nodes;
    ptrdiff_t used;
    ptrdiff_t *candidates;
    struct PolygonList *scratch;
} EditIndex;
typedef struct PolygonList {
    Polygon *items;
    size_t count, capacity;
} PolygonList;
typedef struct {
    uint16_t x, y, width, height;
} TerrainRectangle;
static bool polygon_append(PolygonList *list, Polygon value) {
    if (list->count == list->capacity) {
        if (list->capacity > SIZE_MAX / 2 / sizeof(Polygon))
            return false;
        size_t capacity = list->capacity ? list->capacity * 2 : 16;
        Polygon *items = realloc(list->items, capacity * sizeof(*items));
        if (!items)
            return false;
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = value;
    return true;
}
static void index_destroy(EditIndex *index) {
    free(index->nodes);
    free(index->candidates);
    if (index->scratch) {
        free(index->scratch[0].items);
        free(index->scratch[1].items);
        free(index->scratch);
    }
}
static bool box_overlap(PlanetBox a, PlanetBox b) {
    for (int axis = 0; axis < 3; axis++)
        if (a.lo[axis] >= b.hi[axis] || a.hi[axis] <= b.lo[axis])
            return false;
    return true;
}
static int64_t edit_center(const PlanetEdit *edit, int axis) {
    return edit->box.lo[axis] + edit->box.hi[axis];
}
static void select_middle(PlanetEdit *items, ptrdiff_t first, ptrdiff_t end, ptrdiff_t middle, int axis) {
    ptrdiff_t low = first, high = end - 1;
    while (low < high) {
        int64_t pivot = edit_center(&items[(low + high) / 2], axis);
        ptrdiff_t i = low, j = high;
        while (i <= j) {
            while (edit_center(&items[i], axis) < pivot)
                i++;
            while (edit_center(&items[j], axis) > pivot)
                j--;
            if (i <= j) {
                PlanetEdit temporary = items[i];
                items[i++] = items[j];
                items[j--] = temporary;
            }
        }
        if (middle <= j)
            high = j;
        else if (middle >= i)
            low = i;
        else
            break;
    }
}
static ptrdiff_t index_node(EditIndex *index, ptrdiff_t first, ptrdiff_t count) {
    ptrdiff_t id = index->used++;
    EditNode *node = &index->nodes[id];
    node->first = first;
    node->count = count;
    node->bounds = index->edits->items[first].box;
    for (ptrdiff_t i = first + 1; i < first + count; i++)
        for (int a = 0; a < 3; a++) {
            PlanetBox box = index->edits->items[i].box;
            if (box.lo[a] < node->bounds.lo[a])
                node->bounds.lo[a] = box.lo[a];
            if (box.hi[a] > node->bounds.hi[a])
                node->bounds.hi[a] = box.hi[a];
        }
    if (count <= 8)
        return id;
    int axis = 0;
    for (int a = 1; a < 3; a++)
        if (node->bounds.hi[a] - node->bounds.lo[a] > node->bounds.hi[axis] - node->bounds.lo[axis])
            axis = a;
    ptrdiff_t half = count / 2;
    select_middle(index->edits->items, first, first + count, first + half, axis);
    node->count = 0;
    node->child[0] = index_node(index, first, half);
    node->child[1] = index_node(index, first + half, count - half);
    return id;
}
static bool index_build(EditIndex *index, PlanetEdits *edits) {
    memset(index, 0, sizeof(*index));
    index->edits = edits;
    index->scratch = calloc(2, sizeof(*index->scratch));
    if (!index->scratch)
        return false;
    if (!edits->count)
        return true;
    if (edits->count > PTRDIFF_MAX / 2 || edits->count > SIZE_MAX / 2 / sizeof(*index->nodes)) {
        index_destroy(index);
        return false;
    }
    index->candidates = malloc(edits->count * sizeof(*index->candidates));
    index->nodes = malloc(edits->count * 2 * sizeof(*index->nodes));
    if (!index->nodes || !index->candidates) {
        index_destroy(index);
        return false;
    }
    index_node(index, 0, (ptrdiff_t)edits->count);
    return true;
}
static void index_query(const EditIndex *index, ptrdiff_t id, PlanetBox bounds, ptrdiff_t *items,
                        size_t *count) {
    const EditNode *node = &index->nodes[id];
    if (!box_overlap(node->bounds, bounds))
        return;
    if (node->count) {
        for (ptrdiff_t i = node->first; i < node->first + node->count; i++)
            if (box_overlap(index->edits->items[i].box, bounds))
                items[(*count)++] = i;
    } else {
        index_query(index, node->child[0], bounds, items, count);
        index_query(index, node->child[1], bounds, items, count);
    }
}
DVec3 planet_cube_direction(int face, double u, double v);
DVec3 planet_surface_point(const Planet *p, DVec3 direction, bool ocean);
static bool reserve(Mesh *m, size_t n) {
    if (n <= m->capacity)
        return true;
    if (n > SIZE_MAX / sizeof(Vertex))
        return false;
    size_t cap = m->capacity ? m->capacity : 2048;
    while (cap < n) {
        if (cap > SIZE_MAX / 2 / sizeof(Vertex)) {
            cap = n;
            break;
        }
        cap *= 2;
    }
    Vertex *v = realloc(m->vertices, cap * sizeof(*v));
    if (!v)
        return false;
    m->vertices = v;
    m->capacity = cap;
    return true;
}
static Vec3 color(uint16_t material) {
    const Material *m = material_get(material);
    return v3(m->color[0], m->color[1], m->color[2]);
}
static bool emit(Mesh *m, const Polygon *poly, CellCoord origin) {
    if (poly->count < 3)
        return true;
    DVec3 base = dv3(origin.x * .125, origin.y * .125, origin.z * .125);
    for (int i = 1; i + 1 < poly->count; i++) {
        DVec3 a = pd_sub(poly->p[i], poly->p[0]), b = pd_sub(poly->p[i + 1], poly->p[0]);
        DVec3 cross = dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
        if (pd_length(cross) < 1e-16)
            continue;
        if (!reserve(m, m->count + 3))
            return false;
        int indices[3] = {0, i, i + 1};
        if (pd_dot(cross, dv3(poly->normal.x, poly->normal.y, poly->normal.z)) < 0) {
            indices[1] = i + 1;
            indices[2] = i;
        }
        for (int j = 0; j < 3; j++) {
            DVec3 relative = pd_scale(pd_sub(poly->p[indices[j]], base), 8);
            m->vertices[m->count++] = (Vertex){v3((float)relative.x, (float)relative.y, (float)relative.z),
                                               poly->normal, poly->color};
        }
    }
    return true;
}
static double component(DVec3 p, int a) {
    return a == 0 ? p.x : a == 1 ? p.y : p.z;
}
static void split(const Polygon *p, int axis, double plane, int sign, Polygon *inside, Polygon *outside) {
    *inside = *outside = *p;
    inside->count = outside->count = 0;
    for (int i = 0; i < p->count; i++) {
        DVec3 a = p->p[i], b = p->p[(i + 1) % p->count];
        double da = (component(a, axis) - plane) * sign, db = (component(b, axis) - plane) * sign;
        bool ai = da >= -1e-10, bi = db >= -1e-10;
        if (ai)
            inside->p[inside->count++] = a;
        else
            outside->p[outside->count++] = a;
        if (ai != bi) {
            DVec3 point = pd_add(a, pd_scale(pd_sub(b, a), da / (da - db)));
            inside->p[inside->count++] = point;
            outside->p[outside->count++] = point;
        }
    }
}
static bool subtract(const Polygon *p, PlanetBox box, int skip_axis, PolygonList *out) {
    Polygon remaining = *p;
    for (int a = 0; a < 3; a++)
        if (a != skip_axis)
            for (int side = 0; side < 2; side++) {
                Polygon inside, outside;
                split(&remaining, a, (side ? box.hi[a] : box.lo[a]) * .001, side ? -1 : 1, &inside, &outside);
                if (outside.count >= 3) {
                    if (!polygon_append(out, outside))
                        return false;
                }
                remaining = inside;
                if (remaining.count < 3)
                    return true;
            }
    return true;
}
static bool intersects(const Polygon *p, PlanetBox b, int skip) {
    for (int a = 0; a < 3; a++)
        if (a != skip) {
            double lo = component(p->p[0], a), hi = lo;
            for (int i = 1; i < p->count; i++) {
                double q = component(p->p[i], a);
                lo = fmin(lo, q);
                hi = fmax(hi, q);
            }
            if (hi < b.lo[a] * .001 - 1e-10 || lo > b.hi[a] * .001 + 1e-10)
                return false;
        }
    /* A bounding-box overlap does not mean an oblique terrain triangle
       intersects this voxel. Avoid fragmenting polygons for off-plane edits. */
    Polygon remaining = *p;
    for (int a = 0; a < 3; a++)
        if (a != skip)
            for (int side = 0; side < 2; side++) {
                Polygon inside, outside;
                split(&remaining, a, (side ? b.hi[a] : b.lo[a]) * .001, side ? -1 : 1, &inside, &outside);
                remaining = inside;
                if (remaining.count < 3)
                    return false;
            }
    for (int i = 1; i + 1 < remaining.count; i++) {
        DVec3 a = pd_sub(remaining.p[i], remaining.p[0]), b = pd_sub(remaining.p[i + 1], remaining.p[0]);
        DVec3 cross = dv3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
        if (pd_length(cross) > 1e-16)
            return true;
    }
    return false;
}
static bool base_triangle(Mesh *m, Polygon triangle, CellCoord origin, const EditIndex *index) {
    PolygonList *a = &index->scratch[0], *b = &index->scratch[1];
    a->count = b->count = 0;
    if (!polygon_append(a, triangle))
        return false;
    PlanetBox bounds;
    for (int axis = 0; axis < 3; axis++) {
        double lo = component(triangle.p[0], axis), hi = lo;
        for (int i = 1; i < triangle.count; i++) {
            double v = component(triangle.p[i], axis);
            lo = fmin(lo, v);
            hi = fmax(hi, v);
        }
        bounds.lo[axis] = (int64_t)floor(lo * 1000) - 1;
        bounds.hi[axis] = (int64_t)ceil(hi * 1000) + 1;
    }
    ptrdiff_t *candidates = index->candidates;
    size_t candidate_count = 0;
    if (index->nodes)
        index_query(index, 0, bounds, candidates, &candidate_count);
    for (size_t candidate = 0; candidate < candidate_count; candidate++) {
        PlanetBox box = index->edits->items[candidates[candidate]].box;
        b->count = 0;
        for (size_t i = 0; i < a->count; i++) {
            if (!intersects(&a->items[i], box, -1)) {
                if (!polygon_append(b, a->items[i]))
                    return false;
            } else if (!subtract(&a->items[i], box, -1, b))
                return false;
        }
        PolygonList *swap = a;
        a = b;
        b = swap;
        if (!a->count)
            break;
    }
    for (size_t i = 0; i < a->count; i++)
        if (!emit(m, &a->items[i], origin))
            return false;
    return true;
}
static PlanetBox meters_box(DVec3 center, double radius) {
    double c[3] = {center.x, center.y, center.z};
    PlanetBox b;
    for (int a = 0; a < 3; a++) {
        b.lo[a] = (int64_t)floor((c[a] - radius) * 1000);
        b.hi[a] = (int64_t)ceil((c[a] + radius) * 1000) + 1;
    }
    return b;
}
static DVec3 triangle_normal(DVec3 a, DVec3 b, DVec3 c, DVec3 up) {
    b = pd_sub(b, a);
    c = pd_sub(c, a);
    DVec3 n = pd_normalize(dv3(b.y * c.z - b.z * c.y, b.z * c.x - b.x * c.z, b.x * c.y - b.y * c.x));
    return pd_dot(n, up) < 0 ? pd_scale(n, -1) : n;
}
static bool terrain_rectangle(DVec3 vertices[129][129], int x, int y, int width, int height, double error) {
    DVec3 a = vertices[y][x], b = vertices[y + height][x], c = vertices[y + height][x + width],
          d = vertices[y][x + width];
    for (int dy = 0; dy <= height; dy++)
        for (int dx = 0; dx <= width; dx++) {
            double u = (double)dx / width, v = (double)dy / height;
            DVec3 approximation =
                u <= v ? pd_add(pd_add(pd_scale(a, 1 - v), pd_scale(b, v - u)), pd_scale(c, u))
                       : pd_add(pd_add(pd_scale(a, 1 - u), pd_scale(d, u - v)), pd_scale(c, v));
            if (pd_length(pd_sub(vertices[y + dy][x + dx], approximation)) > error)
                return false;
        }
    return true;
}
static bool terrain_edge(DVec3 vertices[129][129], int edge, int first, int last, int grid, double error) {
    int x0 = edge == 0 ? 0 : edge == 1 ? grid : first;
    int y0 = edge == 2 ? 0 : edge == 3 ? grid : first;
    int x1 = edge == 0 ? 0 : edge == 1 ? grid : last;
    int y1 = edge == 2 ? 0 : edge == 3 ? grid : last;
    DVec3 a = vertices[y0][x0], b = vertices[y1][x1];
    for (ptrdiff_t i = first + 1; i < last; i++) {
        int x = edge == 0 ? 0 : edge == 1 ? grid : i;
        int y = edge == 2 ? 0 : edge == 3 ? grid : i;
        DVec3 line = pd_add(a, pd_scale(pd_sub(b, a), (double)(i - first) / (last - first)));
        if (pd_length(pd_sub(vertices[y][x], line)) > error)
            return false;
    }
    return true;
}
static Polygon clip_direction(Polygon poly, DVec3 normal) {
    Polygon result = poly;
    result.count = 0;
    for (int i = 0; i < poly.count; i++) {
        DVec3 a = poly.p[i], b = poly.p[(i + 1) % poly.count];
        double da = pd_dot(a, normal), db = pd_dot(b, normal);
        bool inside = da >= 0, other = db >= 0;
        if (inside)
            result.p[result.count++] = a;
        if (inside != other)
            result.p[result.count++] = pd_add(a, pd_scale(pd_sub(b, a), da / (da - db)));
    }
    return result;
}
static bool river_mesh(Mesh *mesh, Planet *p, const PlanetPatch *patch, const EditIndex *index) {
    if (patch->river_lod >= PLANET_RIVER_DISABLED_LOD)
        return true;
    double meters_per_pixel = ldexp(1.0, patch->river_lod);
    if (2 * planet_river_max_width(p) < meters_per_pixel * 1.5)
        return true;
    PlanetGeo center = planet_geographic(p, patch->center_m);
    uint32_t *ids = NULL;
    size_t count = 0;
    if (!planet_river_candidates(p, center.latitude_deg, center.longitude_deg, patch->radius_m, &ids, &count))
        return false;
    double size = 2.0 / (1u << patch->level), u0 = -1 + patch->x * size, v0 = -1 + patch->y * size;
    DVec3 f = planet_cube_direction(patch->face, 0, 0),
          u = pd_sub(pd_scale(planet_cube_direction(patch->face, 1, 0), sqrt(2)), f),
          v = pd_sub(pd_scale(planet_cube_direction(patch->face, 0, 1), sqrt(2)), f);
    DVec3 planes[5] = {f, pd_sub(u, pd_scale(f, u0)), pd_sub(pd_scale(f, u0 + size), u),
                       pd_sub(v, pd_scale(f, v0)), pd_sub(pd_scale(f, v0 + size), v)};
    bool ok = true;
    for (size_t j = 0; j < count && ok; j++) {
        PlanetRiverSegment r;
        planet_river_segment(p, ids[j], &r);
        if (2 * r.half_width_m < meters_per_pixel * 1.5)
            continue;
        DVec3 a = planet_from_geographic(p, r.lat[0], r.lon[0],
                                         planet_measured_elevation(p, r.lat[0], r.lon[0])),
              b = planet_from_geographic(p, r.lat[1], r.lon[1],
                                         planet_measured_elevation(p, r.lat[1], r.lon[1]));
        bool possible = true;
        for (int plane = 0; plane < 5; plane++)
            if (pd_dot(a, planes[plane]) < -r.half_width_m * 2 &&
                pd_dot(b, planes[plane]) < -r.half_width_m * 2)
                possible = false;
        if (!possible)
            continue;
        int steps = (int)ceil(pd_length(pd_sub(b, a)) /
                              fmax(50, fmin(5000, fmax(patch->span_m / 8, meters_per_pixel * 4))));
        if (steps < 1)
            steps = 1;
        double meters = 111319.49079327358 * p->a_m / EARTH_A_METERS;
        double cosine = fmax(.01, cos((r.lat[0] + r.lat[1]) * .5 * 3.141592653589793 / 180));
        double dx = (r.lon[1] - r.lon[0]) * cosine, dy = r.lat[1] - r.lat[0], length = hypot(dx, dy);
        if (length < 1e-12)
            continue;
        double dlat = dx / length * r.half_width_m / meters,
               dlon = -dy / length * r.half_width_m / meters / cosine;
        for (int step = 0; step < steps && ok; step++) {
            double lat[2], lon[2], height[2];
            for (int end = 0; end < 2; end++) {
                double t = (double)(step + end) / steps;
                lat[end] = r.lat[0] + t * (r.lat[1] - r.lat[0]);
                lon[end] = r.lon[0] + t * (r.lon[1] - r.lon[0]);
                height[end] = fmax(0, planet_measured_elevation(p, lat[end], lon[end]));
            }
            /* Precise satellite masks own coverage inside downloaded detail cells. */
            if (planet_detail_sample(p, (lat[0] + lat[1]) * .5, (lon[0] + lon[1]) * .5, NULL, NULL))
                continue;
            Polygon poly = {.count = 4, .color = color(MAT_WATER)};
            DVec3 up = planet_up(p, a);
            poly.normal = v3((float)up.x, (float)up.y, (float)up.z);
            poly.p[0] = planet_from_geographic(p, lat[0] + dlat, lon[0] + dlon, height[0]);
            poly.p[1] = planet_from_geographic(p, lat[0] - dlat, lon[0] - dlon, height[0]);
            poly.p[2] = planet_from_geographic(p, lat[1] - dlat, lon[1] - dlon, height[1]);
            poly.p[3] = planet_from_geographic(p, lat[1] + dlat, lon[1] + dlon, height[1]);
            for (int plane = 0; plane < 5 && poly.count >= 3; plane++)
                poly = clip_direction(poly, planes[plane]);
            if (poly.count >= 3)
                ok = base_triangle(mesh, poly, patch->origin, index);
        }
    }
    free(ids);
    return ok;
}
bool planet_mesh_patch(Mesh *m, Planet *p, const PlanetPatch *patch) {
    m->count = 0;
    m->unit_faces = 0;
    m->surface_area_mm2 = 0;
    PlanetEdits edits = {0};
    if (patch->span_m < PLANET_EDIT_SURFACE_SPAN_M &&
        !planet_collect(p, meters_box(patch->center_m, patch->radius_m), &edits)) {
        planet_edits_destroy(&edits);
        return false;
    }
    EditIndex index;
    if (!index_build(&index, &edits)) {
        planet_edits_destroy(&edits);
        return false;
    }
    PlanetGeo patch_geo = planet_geographic(p, patch->center_m);
    bool native = planet_detail_sample(p, patch_geo.latitude_deg, patch_geo.longitude_deg, NULL, NULL);
    int grid = patch->span_m < 12               ? 128
               : patch->span_m < 80             ? 64
               : native && patch->span_m < 4000 ? 64
                                                : PLANET_SURFACE_GRID;
    double error = edits.count ? .00025 : fmin(2000, fmax(.00025, patch->span_m * .00008));
    if (patch->terrain_lod > 0 && !edits.count) {
        error = fmin(2000, fmax(error, ldexp(.00025, patch->terrain_lod - 1)));
        double spacing = fmax(.05, error * 8);
        while (grid > 8 && patch->span_m / (grid / 2) <= spacing)
            grid /= 2;
    }
    DVec3 vertices[129][129], normals[129][129];
    uint16_t materials[128][128];
    uint16_t owners[128][128];
    TerrainRectangle rectangles[128 * 128];
    size_t rectangle_count = 0;
    bool used[128][128] = {{false}};
    double size = 2.0 / (1u << patch->level), u0 = -1 + patch->x * size, v0 = -1 + patch->y * size;
    for (int y = 0; y <= grid; y++)
        for (int x = 0; x <= grid; x++) {
            DVec3 direction = planet_cube_direction(patch->face, u0 + size * x / grid, v0 + size * y / grid);
            vertices[y][x] = planet_surface_point(p, direction, true);
            normals[y][x] = planet_up(p, vertices[y][x]);
        }
    /* Smooth density-height surface normals, derived from the same samples as geometry. */
    for (int y = 0; y <= grid; y++)
        for (int x = 0; x <= grid; x++) {
            DVec3 dx = pd_sub(vertices[y][x < grid ? x + 1 : x], vertices[y][x > 0 ? x - 1 : x]);
            DVec3 dy = pd_sub(vertices[y < grid ? y + 1 : y][x], vertices[y > 0 ? y - 1 : y][x]);
            DVec3 n = pd_normalize(
                dv3(dx.y * dy.z - dx.z * dy.y, dx.z * dy.x - dx.x * dy.z, dx.x * dy.y - dx.y * dy.x));
            if (pd_dot(n, normals[y][x]) < 0)
                n = pd_scale(n, -1);
            normals[y][x] = n;
        }
    for (int y = 0; y < grid; y++)
        for (int x = 0; x < grid; x++) {
            PlanetGeo g = planet_geographic(p, vertices[y][x]);
            bool native_water;
            if (planet_detail_sample(p, g.latitude_deg, g.longitude_deg, NULL, &native_water))
                materials[y][x] = planet_visible_material(p, g.latitude_deg, g.longitude_deg);
            else
                materials[y][x] = planet_surface_material(
                    g.latitude_deg, g.longitude_deg,
                    planet_measured_elevation(p, g.latitude_deg, g.longitude_deg) * EARTH_A_METERS / p->a_m);
        }
    m->unit_faces = (size_t)grid * grid + 4 * grid;
    /* Greedy rectangles preserve the sampled relief within the LOD error.
       Material edges and genuinely curved/rough regions keep their triangles. */
    for (int y = 0; y < grid; y++)
        for (int x = 0; x < grid; x++) {
            if (used[y][x])
                continue;
            int width = 1, height = 1;
            while (x + width < grid && !used[y][x + width] && materials[y][x + width] == materials[y][x] &&
                   terrain_rectangle(vertices, x, y, width + 1, 1, error))
                width++;
            while (y + height < grid) {
                bool compatible = true;
                for (int dx = 0; dx < width; dx++)
                    compatible &=
                        !used[y + height][x + dx] && materials[y + height][x + dx] == materials[y][x];
                if (!compatible || !terrain_rectangle(vertices, x, y, width, height + 1, error))
                    break;
                height++;
            }
            for (int dy = 0; dy < height; dy++)
                for (int dx = 0; dx < width; dx++) {
                    used[y + dy][x + dx] = true;
                    owners[y + dy][x + dx] = (uint16_t)rectangle_count;
                }
            rectangles[rectangle_count++] =
                (TerrainRectangle){(uint16_t)x, (uint16_t)y, (uint16_t)width, (uint16_t)height};
        }
    /* A merged edge must retain the corners of its neighbouring rectangles.
       Otherwise independently approximated edges leave visible sky cracks.
       Plan the partition first, then triangulate matching shared boundaries. */
    for (size_t rectangle = 0; rectangle < rectangle_count; rectangle++) {
        TerrainRectangle r = rectangles[rectangle];
        int x = r.x, y = r.y, width = r.width, height = r.height;
        DVec3 corners[4] = {vertices[y][x], vertices[y + height][x], vertices[y + height][x + width],
                            vertices[y][x + width]};
        uint16_t boundary[4 * 128];
        int boundary_count = 0;
        boundary[boundary_count++] = (uint16_t)(y * 129 + x);
        for (int i = 1; i < height; i++)
            if (x > 0 && owners[y + i - 1][x - 1] != owners[y + i][x - 1])
                boundary[boundary_count++] = (uint16_t)((y + i) * 129 + x);
        boundary[boundary_count++] = (uint16_t)((y + height) * 129 + x);
        for (int i = 1; i < width; i++)
            if (y + height < grid && owners[y + height][x + i - 1] != owners[y + height][x + i])
                boundary[boundary_count++] = (uint16_t)((y + height) * 129 + x + i);
        boundary[boundary_count++] = (uint16_t)((y + height) * 129 + x + width);
        for (int i = height - 1; i > 0; i--)
            if (x + width < grid && owners[y + i - 1][x + width] != owners[y + i][x + width])
                boundary[boundary_count++] = (uint16_t)((y + i) * 129 + x + width);
        boundary[boundary_count++] = (uint16_t)(y * 129 + x + width);
        for (int i = width - 1; i > 0; i--)
            if (y > 0 && owners[y - 1][x + i - 1] != owners[y - 1][x + i])
                boundary[boundary_count++] = (uint16_t)(y * 129 + x + i);
        const int indices[2][3] = {{0, 1, 2}, {0, 2, 3}};
        int previous[4 * 128], next[4 * 128], at = 0, remaining_area = width * height * 2;
        for (int i = 0; i < boundary_count; i++) {
            previous[i] = (i + boundary_count - 1) % boundary_count;
            next[i] = (i + 1) % boundary_count;
        }
        /* Convex ears retain all shared points with N-2 triangles and no extra
           center vertex. Integer grid coordinates avoid near-collinear tests. */
        for (int t = 0; t < boundary_count - 2; t++) {
            Polygon poly = {.count = 3, .color = color(materials[y][x])};
            if (boundary_count == 4) {
                for (int j = 0; j < 3; j++)
                    poly.p[j] = corners[indices[t][j]];
            } else {
                bool found = false;
                for (int scan = 0; scan < boundary_count; scan++) {
                    int a = boundary[previous[at]], b = boundary[at], c = boundary[next[at]];
                    int cross =
                        (b % 129 - a % 129) * (c / 129 - b / 129) - (b / 129 - a / 129) * (c % 129 - b % 129);
                    if (cross < 0 && (t == boundary_count - 3 || remaining_area + cross > 0)) {
                        poly.p[0] = vertices[a / 129][a % 129];
                        poly.p[1] = vertices[b / 129][b % 129];
                        poly.p[2] = vertices[c / 129][c % 129];
                        next[previous[at]] = next[at];
                        previous[next[at]] = previous[at];
                        remaining_area += cross;
                        at = next[at];
                        found = true;
                        break;
                    }
                    at = next[at];
                }
                if (!found)
                    goto failed;
            }
            DVec3 normal = triangle_normal(poly.p[0], poly.p[1], poly.p[2], normals[y][x]);
            poly.normal = v3((float)normal.x, (float)normal.y, (float)normal.z);
            if (!base_triangle(m, poly, patch->origin, &index))
                goto failed;
        }
    }
    DVec3 face = planet_cube_direction(patch->face, 0, 0),
          axis_u = pd_sub(pd_scale(planet_cube_direction(patch->face, 1, 0), sqrt(2)), face),
          axis_v = pd_sub(pd_scale(planet_cube_direction(patch->face, 0, 1), sqrt(2)), face);
    for (size_t i = 0; i < m->count; i++) {
        Vertex *vertex = &m->vertices[i];
        DVec3 pos = dv3(patch->origin.x * .125 + vertex->position.x * .125,
                        patch->origin.y * .125 + vertex->position.y * .125,
                        patch->origin.z * .125 + vertex->position.z * .125);
        double den = pd_dot(pos, face);
        double gx = fmax(0, fmin(grid - .000001, (pd_dot(pos, axis_u) / den - u0) / size * grid)),
               gy = fmax(0, fmin(grid - .000001, (pd_dot(pos, axis_v) / den - v0) / size * grid));
        int x = (int)gx, y = (int)gy;
        double tx = gx - x, ty = gy - y;
        DVec3 n = pd_normalize(pd_add(
            pd_add(pd_scale(normals[y][x], (1 - tx) * (1 - ty)), pd_scale(normals[y][x + 1], tx * (1 - ty))),
            pd_add(pd_scale(normals[y + 1][x], (1 - tx) * ty), pd_scale(normals[y + 1][x + 1], tx * ty))));
        if (vertex->color.z > vertex->color.x * 1.5 && vertex->color.z > vertex->color.y * 1.2)
            n = planet_up(p, pos);
        vertex->normal = v3((float)n.x, (float)n.y, (float)n.z);
    }
    double depth = fmax(.01, patch->span_m * .003);
    for (int edge = 0; edge < 4; edge++)
        for (int first = 0; first < grid;) {
            int last = first + 1;
            int owner = edge == 0   ? owners[first][0]
                        : edge == 1 ? owners[first][grid - 1]
                        : edge == 2 ? owners[0][first]
                                    : owners[grid - 1][first];
            /* Skirt tops must use the same endpoints as surface rectangles. */
            while (last < grid &&
                   owner == (edge == 0   ? owners[last][0]
                             : edge == 1 ? owners[last][grid - 1]
                             : edge == 2 ? owners[0][last]
                                         : owners[grid - 1][last]) &&
                   terrain_edge(vertices, edge, first, last + 1, grid, error))
                last++;
            int x0 = edge == 0 ? 0 : edge == 1 ? grid : first;
            int y0 = edge == 2 ? 0 : edge == 3 ? grid : first;
            int x1 = edge == 0 ? 0 : edge == 1 ? grid : last;
            int y1 = edge == 2 ? 0 : edge == 3 ? grid : last;
            DVec3 a = vertices[y0][x0], b = vertices[y1][x1], na = planet_up(p, vertices[y0][x0]),
                  nb = planet_up(p, vertices[y1][x1]);
            Polygon poly = {
                .count = 4, .color = color(MAT_STONE), .normal = v3((float)na.x, (float)na.y, (float)na.z)};
            poly.p[0] = a;
            poly.p[1] = b;
            poly.p[2] = pd_sub(b, pd_scale(nb, depth));
            poly.p[3] = pd_sub(a, pd_scale(na, depth));
            if (!emit(m, &poly, patch->origin))
                goto failed;
            first = last;
        }
    if (!river_mesh(m, p, patch, &index))
        goto failed;
    index_destroy(&index);
    planet_edits_destroy(&edits);
    return true;
failed:
    snprintf(p->error, sizeof(p->error), "Out of memory building planet mesh");
    index_destroy(&index);
    planet_edits_destroy(&edits);
    return false;
}
static double signed_base(Planet *p, DVec3 point) {
    double r = pd_length(point);
    if (r < p->b_m - 12000)
        return r - p->b_m;
    PlanetGeo g = planet_geographic(p, point);
    return g.altitude_m - planet_visible_elevation(p, g.latitude_deg, g.longitude_deg);
}
static Polygon clip_base(Planet *p, Polygon poly, int axis, int sign, bool outside) {
    Polygon result = poly;
    result.count = 0;
    double sample_offset = sign * .0005;
    for (int i = 0; i < poly.count; i++) {
        DVec3 a = poly.p[i], b = poly.p[(i + 1) % poly.count], pa = a, pb = b;
        if (axis == 0) {
            pa.x += sample_offset;
            pb.x += sample_offset;
        } else if (axis == 1) {
            pa.y += sample_offset;
            pb.y += sample_offset;
        } else {
            pa.z += sample_offset;
            pb.z += sample_offset;
        }
        double da = signed_base(p, pa) * (outside ? 1 : -1), db = signed_base(p, pb) * (outside ? 1 : -1);
        bool ai = da >= 0, bi = db >= 0;
        if (ai)
            result.p[result.count++] = a;
        if (ai != bi)
            result.p[result.count++] = pd_add(a, pd_scale(pd_sub(b, a), da / (da - db)));
    }
    return result;
}
static Polygon intersect_face(Polygon poly, PlanetBox b, int axis) {
    for (int a = 0; a < 3; a++)
        if (a != axis)
            for (int side = 0; side < 2; side++) {
                Polygon inside, outside;
                split(&poly, a, (side ? b.hi[a] : b.lo[a]) * .001, side ? -1 : 1, &inside, &outside);
                poly = inside;
                if (poly.count < 3)
                    return poly;
            }
    return poly;
}
bool planet_mesh_edits(Mesh *m, Planet *p, const PlanetPatch *patch) {
    m->count = 0;
    m->unit_faces = 0;
    m->surface_area_mm2 = 0;
    PlanetBox bounds;
    int64_t o[3] = {patch->edit_key.x * 2000, patch->edit_key.y * 2000, patch->edit_key.z * 2000};
    for (int a = 0; a < 3; a++) {
        bounds.lo[a] = o[a] - 1;
        bounds.hi[a] = o[a] + 2001;
    }
    PlanetEdits edits = {0};
    if (!planet_collect(p, bounds, &edits)) {
        planet_edits_destroy(&edits);
        return false;
    }
    EditIndex index;
    if (!index_build(&index, &edits)) {
        planet_edits_destroy(&edits);
        return false;
    }
    for (size_t j = 0; j < edits.count; j++) {
        PlanetEdit source = edits.items[j];
        bool own = true;
        for (int a = 0; a < 3; a++)
            if (source.box.lo[a] < o[a] || source.box.hi[a] > o[a] + 2000)
                own = false;
        if (!own)
            continue;
        bool solid = source.value.material != MAT_AIR;
        for (int axis = 0; axis < 3; axis++)
            for (int sign = -1; sign <= 1; sign += 2) {
                int ua = (axis + 1) % 3, va = (axis + 2) % 3;
                int64_t plane = sign > 0 ? source.box.hi[axis] : source.box.lo[axis],
                        slice = plane + (sign < 0 ? -1 : 0);
                Polygon base = {.count = 4, .color = color(solid ? source.value.material : MAT_STONE)};
                float n[3] = {0};
                n[axis] = (float)(solid ? sign : -sign);
                base.normal = v3(n[0], n[1], n[2]);
                for (int i = 0; i < 4; i++) {
                    double q[3];
                    q[axis] = plane * .001;
                    q[ua] = ((i == 1 || i == 2) ? source.box.hi[ua] : source.box.lo[ua]) * .001;
                    q[va] = (i >= 2 ? source.box.hi[va] : source.box.lo[va]) * .001;
                    base.p[i] = dv3(q[0], q[1], q[2]);
                }
                if (!solid) {
                    DVec3 center = pd_scale(pd_add(base.p[0], base.p[2]), .5);
                    Voxel v = planet_base_voxel(p, center);
                    if (v.material != MAT_AIR)
                        base.color = color(v.material);
                }
                PolygonList *polygons = &index.scratch[0], *next = &index.scratch[1];
                polygons->count = next->count = 0;
                if (!polygon_append(polygons, base))
                    goto failed;
                PlanetBox neighborhood = source.box;
                neighborhood.lo[axis] = slice;
                neighborhood.hi[axis] = slice + 1;
                ptrdiff_t *candidates = index.candidates;
                size_t candidate_count = 0;
                index_query(&index, 0, neighborhood, candidates, &candidate_count);
                for (size_t candidate = 0; candidate < candidate_count; candidate++) {
                    PlanetEdit other = edits.items[candidates[candidate]];
                    next->count = 0;
                    for (size_t i = 0; i < polygons->count; i++) {
                        if (!intersects(&polygons->items[i], other.box, axis)) {
                            if (!polygon_append(next, polygons->items[i]))
                                goto failed;
                            continue;
                        }
                        if (solid && other.value.material == MAT_AIR) {
                            Polygon part = intersect_face(polygons->items[i], other.box, axis);
                            if (!emit(m, &part, patch->origin))
                                goto failed;
                        }
                        if (!subtract(&polygons->items[i], other.box, axis, next))
                            goto failed;
                    }
                    PolygonList *swap = polygons;
                    polygons = next;
                    next = swap;
                }
                for (size_t i = 0; i < polygons->count; i++) {
                    Polygon part = clip_base(p, polygons->items[i], axis, sign, solid);
                    if (!emit(m, &part, patch->origin))
                        goto failed;
                }
            }
    }
    index_destroy(&index);
    planet_edits_destroy(&edits);
    return planet_mesh_greedy_edits(m, patch->origin, patch->edit_lod_mm) && planet_mesh_smooth_edits(m);
failed:
    snprintf(p->error, sizeof(p->error), "Out of memory building planet mesh");
    index_destroy(&index);
    planet_edits_destroy(&edits);
    return false;
}
