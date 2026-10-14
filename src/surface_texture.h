#ifndef SURFACE_TEXTURE_H
#define SURFACE_TEXTURE_H
#include "planet.h"
/* RG encodes signed elevation; B is engine material; A is mapped inland water.
   Shader filtering interpolates height, then thresholds water to keep shorelines crisp. */
bool planet_surface_texture_bake(const Planet *planet, const char *path);
bool planet_surface_texture_load(const char *path, unsigned char **rgba, uint32_t *width, uint32_t *height);
#endif
