#include "resources.h"
#include <stdio.h>
#include <string.h>
#ifdef __linux__
#include <unistd.h>
#endif
void resources_sample(ResourceStats *s, double elapsed, double update_ms) {
    if (!isfinite(elapsed) || elapsed <= 0 || !isfinite(update_ms))
        return;
    s->update_ms = update_ms;
    s->history[s->history_next] = elapsed * 1000;
    s->history_next = (s->history_next + 1) % RESOURCE_HISTORY;
    if (s->history_count < RESOURCE_HISTORY)
        s->history_count++;
    clock_t cpu = clock();
    if (!s->initialized) {
        s->previous_cpu = cpu;
        s->initialized = true;
    }
    s->sample_seconds += elapsed;
    if (s->sample_seconds < .5)
        return;
    if (cpu != (clock_t)-1 && s->previous_cpu != (clock_t)-1 && cpu >= s->previous_cpu)
        s->cpu_percent = 100.0 * (double)(cpu - s->previous_cpu) / CLOCKS_PER_SEC / s->sample_seconds;
    s->previous_cpu = cpu;
    s->sample_seconds = 0;
#ifdef __linux__
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long pages, resident;
    long page_bytes = sysconf(_SC_PAGESIZE);
    if (f) {
        if (fscanf(f, "%lu %lu", &pages, &resident) == 2 && page_bytes > 0 &&
            resident <= SIZE_MAX / (size_t)page_bytes)
            s->process_bytes = resident * (size_t)page_bytes;
        fclose(f);
    }
    f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[192], key[48];
        unsigned long long kib;
        while (fgets(line, sizeof(line), f))
            if (sscanf(line, "%47s %llu", key, &kib) == 2 && kib <= SIZE_MAX / 1024) {
                if (!strcmp(key, "MemTotal:"))
                    s->system_bytes = (size_t)kib * 1024;
                if (!strcmp(key, "MemAvailable:"))
                    s->available_bytes = (size_t)kib * 1024;
            }
        fclose(f);
    }
#endif
}
bool resources_scan(ResourceScan *s, Planet *p, DVec3 center, DVec3 right, DVec3 forward, double depth,
                    double span) {
    if (!isfinite(depth) || depth < 0 || !isfinite(span) || span <= 0 || !isfinite(pd_length(center)) ||
        pd_length(center) > 2e9)
        return false;
    p->error[0] = 0;
    DVec3 up = planet_up(p, center);
    right = pd_normalize(pd_sub(right, pd_scale(up, pd_dot(right, up))));
    forward = pd_normalize(pd_sub(forward, pd_scale(up, pd_dot(forward, up))));
    if (pd_length(right) < .5 || pd_length(forward) < .5)
        return false;
    ResourceScan result = {.depth_m = depth, .span_m = span, .center = center, .ready = true};
    for (int y = 0; y < RESOURCE_SCAN_GRID; y++)
        for (int x = 0; x < RESOURCE_SCAN_GRID; x++) {
            double dx = (x / (double)(RESOURCE_SCAN_GRID - 1) - .5) * span;
            double dy = (.5 - y / (double)(RESOURCE_SCAN_GRID - 1)) * span;
            DVec3 point = pd_add(pd_add(center, pd_scale(right, dx)), pd_scale(forward, dy));
            point = pd_sub(point, pd_scale(up, depth));
            if (fmax(fmax(fabs(point.x), fabs(point.y)), fabs(point.z)) > 2e9)
                return false;
            Voxel value = planet_voxel(p, (int64_t)floor(point.x * 1000), (int64_t)floor(point.y * 1000),
                                       (int64_t)floor(point.z * 1000));
            if (p->error[0])
                return false;
            result.materials[y * RESOURCE_SCAN_GRID + x] = value.material;
            result.counts[value.material]++;
        }
    *s = result;
    return true;
}
