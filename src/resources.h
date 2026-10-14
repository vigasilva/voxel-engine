#ifndef RESOURCES_H
#define RESOURCES_H
#include "planet.h"
#include <time.h>
#define RESOURCE_HISTORY 120
#define RESOURCE_SCAN_GRID 25
typedef struct {
    double cpu_percent, update_ms;
    size_t process_bytes, system_bytes, available_bytes;
    size_t gpu_buffer_bytes, gpu_depth_bytes, gpu_texture_bytes, gpu_heap_bytes;
    size_t occluded_patches, occluded_max;
    size_t cached_meshes, cached_mesh_bytes;
    size_t fallback_meshes;
    double history[RESOURCE_HISTORY], sample_seconds;
    size_t history_next, history_count;
    clock_t previous_cpu;
    bool initialized, texture_only;
} ResourceStats;
typedef struct {
    uint16_t materials[RESOURCE_SCAN_GRID * RESOURCE_SCAN_GRID];
    size_t counts[MAT_COUNT];
    double depth_m, span_m;
    DVec3 center;
    bool ready;
} ResourceScan;
void resources_sample(ResourceStats *stats, double frame_seconds, double update_ms);
bool resources_scan(ResourceScan *scan, Planet *planet, DVec3 center, DVec3 right, DVec3 forward,
                    double depth_m, double span_m);
#endif
