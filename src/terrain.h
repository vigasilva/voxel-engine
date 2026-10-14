#ifndef TERRAIN_H
#define TERRAIN_H
#include "detail.h"
DetailChunk *terrain_build_chunk(const World *world, int chunk);
/* Absolute cell addresses, small fractional offsets. */
int terrain_height_mm(const World *world, CellCoord cell, double x_offset_cells, double z_offset_cells);
Voxel terrain_voxel(const World *world, CellCoord cell, int x_mm, int y_mm, int z_mm);
bool terrain_empty_plane(const World *world, int chunk, int axis, int slice, MmRect rect,
                         DetailRectFn visitor, void *context);
#endif
