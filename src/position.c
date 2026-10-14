#include "position.h"
#include <limits.h>
bool coord_add(int64_t a, int64_t b, int64_t *r) {
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b))
        return false;
    *r = a + b;
    return true;
}
bool coord_sub(int64_t a, int64_t b, int64_t *r) {
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b))
        return false;
    *r = a - b;
    return true;
}
static bool normalize_axis(int64_t cell, double mm, int64_t *new_cell, double *offset) {
    if (!isfinite(mm))
        return false;
    double shift = floor(mm / VOXEL_SIZE_MM);
    /* 2^63 is exactly representable; INT64_MAX is not exactly representable as a double. */
    if (shift < -0x1p63 || shift >= 0x1p63)
        return false;
    if (!coord_add(cell, (int64_t)shift, new_cell))
        return false;
    *offset = mm - shift * VOXEL_SIZE_MM;
    if (*offset >= VOXEL_SIZE_MM) {
        if (!coord_add(*new_cell, 1, new_cell))
            return false;
        *offset = 0;
    }
    if (*offset < 0) {
        if (!coord_add(*new_cell, -1, new_cell))
            return false;
        *offset += VOXEL_SIZE_MM;
    }
    return true;
}
bool position_move_mm(WorldPosition *p, DVec3 d) {
    WorldPosition next;
    if (!normalize_axis(p->cell.x, p->offset_mm.x + d.x, &next.cell.x, &next.offset_mm.x) ||
        !normalize_axis(p->cell.y, p->offset_mm.y + d.y, &next.cell.y, &next.offset_mm.y) ||
        !normalize_axis(p->cell.z, p->offset_mm.z + d.z, &next.cell.z, &next.offset_mm.z))
        return false;
    *p = next;
    return true;
}
bool position_from_local(CellCoord origin, DVec3 cells, WorldPosition *out) {
    WorldPosition p = {origin, {0, 0, 0}};
    if (!position_move_mm(&p, dv3(cells.x * VOXEL_SIZE_MM, cells.y * VOXEL_SIZE_MM, cells.z * VOXEL_SIZE_MM)))
        return false;
    *out = p;
    return true;
}
bool position_relative_cells(WorldPosition p, CellCoord origin, DVec3 *r) {
    int64_t x, y, z;
    if (!coord_sub(p.cell.x, origin.x, &x) || !coord_sub(p.cell.y, origin.y, &y) ||
        !coord_sub(p.cell.z, origin.z, &z))
        return false;
    *r = dv3((double)x + p.offset_mm.x / VOXEL_SIZE_MM, (double)y + p.offset_mm.y / VOXEL_SIZE_MM,
             (double)z + p.offset_mm.z / VOXEL_SIZE_MM);
    return true;
}
