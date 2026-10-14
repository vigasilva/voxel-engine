#ifndef TERRAIN_STREAM_H
#define TERRAIN_STREAM_H
#include "planet.h"
typedef struct {
    long process;
    bool enabled;
    int lat_key, lon_key;
    double age, cooldown, stable;
} TerrainStream;
void terrain_stream_stop(TerrainStream *stream);
bool terrain_stream_step(TerrainStream *stream, Planet *planet, PlanetGeo location, double elapsed);
#endif
