#ifndef POSITION_H
#define POSITION_H
#include "math3d.h"
#include <stdbool.h>
#include <stdint.h>
#define VOXEL_SIZE_MM 125
#define VOXEL_SIZE_METERS (VOXEL_SIZE_MM / 1000.0f)
typedef struct {
    int64_t x, y, z;
} CellCoord;
typedef struct {
    double x, y, z;
} DVec3;
/* Global integer cell + normalized local millimeter offset, never a huge absolute float. */
typedef struct {
    CellCoord cell;
    DVec3 offset_mm;
} WorldPosition;
static inline CellCoord cell_coord(int64_t x, int64_t y, int64_t z) {
    return (CellCoord){x, y, z};
}
static inline DVec3 dv3(double x, double y, double z) {
    return (DVec3){x, y, z};
}
bool coord_add(int64_t a, int64_t b, int64_t *result);
bool coord_sub(int64_t a, int64_t b, int64_t *result);
bool position_from_local(CellCoord origin, DVec3 cells, WorldPosition *out);
bool position_move_mm(WorldPosition *position, DVec3 delta_mm);
bool position_relative_cells(WorldPosition position, CellCoord origin, DVec3 *relative);
#endif
