#include "hud.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <inttypes.h>
/* Five columns, seven bits per column. A tiny built-in font keeps the UI asset-free. */
static const unsigned char glyphs[128][5] = {
    ['A'] = {0x7e, 0x11, 0x11, 0x11, 0x7e}, ['B'] = {0x7f, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3e, 0x41, 0x41, 0x41, 0x22}, ['D'] = {0x7f, 0x41, 0x41, 0x22, 0x1c},
    ['E'] = {0x7f, 0x49, 0x49, 0x49, 0x41}, ['F'] = {0x7f, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3e, 0x41, 0x49, 0x49, 0x7a}, ['H'] = {0x7f, 0x08, 0x08, 0x08, 0x7f},
    ['I'] = {0, 0x41, 0x7f, 0x41, 0},       ['J'] = {0x20, 0x40, 0x41, 0x3f, 0x01},
    ['K'] = {0x7f, 0x08, 0x14, 0x22, 0x41}, ['L'] = {0x7f, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7f, 0x02, 0x0c, 0x02, 0x7f}, ['N'] = {0x7f, 0x04, 0x08, 0x10, 0x7f},
    ['O'] = {0x3e, 0x41, 0x41, 0x41, 0x3e}, ['P'] = {0x7f, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3e, 0x41, 0x51, 0x21, 0x5e}, ['R'] = {0x7f, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31}, ['T'] = {0x01, 0x01, 0x7f, 0x01, 0x01},
    ['U'] = {0x3f, 0x40, 0x40, 0x40, 0x3f}, ['V'] = {0x1f, 0x20, 0x40, 0x20, 0x1f},
    ['W'] = {0x3f, 0x40, 0x38, 0x40, 0x3f}, ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07}, ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['0'] = {0x3e, 0x51, 0x49, 0x45, 0x3e}, ['1'] = {0, 0x42, 0x7f, 0x40, 0},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46}, ['3'] = {0x21, 0x41, 0x45, 0x4b, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7f, 0x10}, ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3c, 0x4a, 0x49, 0x49, 0x30}, ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36}, ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1e},
    ['.'] = {0, 0x60, 0x60, 0, 0},          [','] = {0, 0x40, 0x20, 0, 0},
    [':'] = {0, 0x36, 0x36, 0, 0},          ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02}, ['+'] = {0x08, 0x08, 0x3e, 0x08, 0x08},
    ['%'] = {0x23, 0x13, 0x08, 0x64, 0x62}, ['['] = {0, 0x7f, 0x41, 0x41, 0},
    [']'] = {0, 0x41, 0x41, 0x7f, 0},       ['('] = {0, 0x1c, 0x22, 0x41, 0},
    [')'] = {0, 0x41, 0x22, 0x1c, 0},       ['='] = {0x14, 0x14, 0x14, 0x14, 0x14}};
static void rect(Hud *h, float x, float y, float w, float height, Vec3 color) {
    if (h->count + 6 > h->capacity) {
        h->overflow = true;
        return;
    }
    const Vec3 p[] = {{x, y, 0}, {x + w, y, 0},          {x + w, y + height, 0},
                      {x, y, 0}, {x + w, y + height, 0}, {x, y + height, 0}};
    for (int i = 0; i < 6; i++)
        h->vertices[h->count++] = (Vertex){p[i], {0, 0, 0}, color};
}
static void text(Hud *h, int x, int y, int scale, Vec3 color, const char *s) {
    for (; *s; s++, x += 6 * scale) {
        unsigned c = (unsigned)toupper((unsigned char)*s);
        if (c >= 128)
            c = ' ';
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7;) {
                if (!(glyphs[c][col] & (1u << row))) {
                    row++;
                    continue;
                }
                int first = row++;
                while (row < 7 && (glyphs[c][col] & (1u << row)))
                    row++;
                rect(h, (float)(x + col * scale), (float)(y + first * scale), (float)scale,
                     (float)((row - first) * scale), color);
            }
    }
}
bool hud_init(Hud *h, size_t capacity) {
    *h = (Hud){0};
    h->vertices = calloc(capacity, sizeof(Vertex));
    h->capacity = capacity;
    return h->vertices != NULL;
}
void hud_destroy(Hud *h) {
    free(h->vertices);
    *h = (Hud){0};
}
void hud_build(Hud *h, const World *w, RayHit hit, uint16_t selected, WorldPosition camera, int width,
               int height, float fps, size_t vertices, bool captured, const char *geometry_view,
               bool workshop, int brush_mm, const char *status, const Scene *scene) {
    h->count = 0;
    h->overflow = false;
    Vec3 panel = v3(.018f, .03f, .042f), white = v3(.88f, .93f, .94f), muted = v3(.37f, .48f, .53f),
         accent = v3(.37f, .79f, .59f);
    char line[160];
    rect(h, 18, 18, 398, 226, panel);
    rect(h, 18, 18, 3, 226, accent);
    text(h, 34, 34, 2, white, "VOXEL LABORATORY");
    text(h, 34, 58, 1, accent, "MATERIAL WORLD / FOUNDATION 0.5");
    snprintf(line, sizeof(line), "%.0f FPS  /  %zu TRIANGLES", fps, vertices / 3);
    text(h, 34, 78, 1, muted, line);
    snprintf(line, sizeof(line), "BASE %d X %d X %d / %d MM / SEED %u", WORLD_X, WORLD_Y, WORLD_Z,
             VOXEL_SIZE_MM, w->seed);
    text(h, 34, 92, 1, muted, line);
    DVec3 local = {0};
    position_relative_cells(camera, w->origin, &local);
    snprintf(line, sizeof(line), "LOCAL %.3f %.3f %.3f M", local.x * VOXEL_SIZE_METERS,
             local.y * VOXEL_SIZE_METERS, local.z * VOXEL_SIZE_METERS);
    text(h, 34, 106, 1, muted, line);
    snprintf(line, sizeof(line), "64-BIT CELLS / %.1f KIB STORAGE", world_storage_bytes(w) / 1024.0);
    text(h, 34, 122, 1, muted, line);
    snprintf(line, sizeof(line), "F3 GEOMETRY: %s", geometry_view);
    text(h, 34, 138, 1, accent, line);
    snprintf(line, sizeof(line), "F4 WORKSHOP: %s / [ ] BRUSH %d MM", workshop ? "ON" : "OFF", brush_mm);
    text(h, 34, 154, 1, accent, line);
    snprintf(line, sizeof(line), "1 MM REGIONS / %zu OF %d CHUNKS", world_detail_count(w), CHUNK_COUNT);
    text(h, 34, 170, 1, muted, line);
    snprintf(line, sizeof(line), "VIEW %.0f M / VISIBLE %zu / QUEUE %zu", scene->view_m, scene->visible,
             scene->pending);
    text(h, 34, 186, 1, muted, line);
    snprintf(line, sizeof(line), "LOD CHUNKS %zu / %zu / %zu / %zu / %zu", scene->lod_counts[0],
             scene->lod_counts[1], scene->lod_counts[2], scene->lod_counts[3], scene->lod_counts[4]);
    text(h, 34, 202, 1, muted, line);
    if (status && *status)
        text(h, 34, 218, 1, accent, status);
    int px = width - 344;
    rect(h, (float)px, 18, 326, hit.hit ? (hit.fine ? 290 : 266) : 78, panel);
    text(h, px + 16, 34, 1, accent, "MATERIAL INSPECTOR");
    if (hit.hit) {
        Voxel v = world_hit_voxel(w, hit);
        const Material *m = material_get(v.material);
        text(h, px + 16, 56, 2, white, m->name);
        snprintf(line, sizeof(line), "CELL X %" PRId64, hit.x);
        text(h, px + 16, 82, 1, muted, line);
        snprintf(line, sizeof(line), "CELL Y %" PRId64, hit.y);
        text(h, px + 16, 98, 1, muted, line);
        snprintf(line, sizeof(line), "CELL Z %" PRId64, hit.z);
        text(h, px + 16, 114, 1, muted, line);
        snprintf(line, sizeof(line), "DISTANCE %.3f M", hit.distance * VOXEL_SIZE_METERS);
        text(h, px + 16, 130, 1, muted, line);
        snprintf(line, sizeof(line), "TEMPERATURE %.2f K / %.1f C", v.temperature_k,
                 v.temperature_k - 273.15f);
        text(h, px + 16, 146, 1, white, line);
        snprintf(line, sizeof(line), "DENSITY %.0f KG/M3", m->density_kg_m3);
        text(h, px + 16, 162, 1, white, line);
        float s = VOXEL_SIZE_METERS;
        if (hit.fine)
            snprintf(line, sizeof(line), "1 MM CELL MASS %.3f MG", m->density_kg_m3 * .001f);
        else
            snprintf(line, sizeof(line), "CELL MASS %.3f KG", m->density_kg_m3 * s * s * s);
        text(h, px + 16, 178, 1, white, line);
        text(h, px + 16, 200, 1, accent, "COMPOSITION / MASS FRACTIONS");
        for (int i = 0; i < m->constituent_count; i++) {
            snprintf(line, sizeof(line), "%s %.1f%%", element_symbol(m->composition[i].element),
                     m->composition[i].mass_fraction * 100);
            text(h, px + 16 + (i % 3) * 100, 218 + (i / 3) * 16, 1, white, line);
        }
        if (hit.fine) {
            snprintf(line, sizeof(line), "CHUNK %d / MM %d %d %d", hit.chunk, hit.mm[0], hit.mm[1],
                     hit.mm[2]);
            text(h, px + 16, 256, 1, accent, line);
        }
        text(h, px + 16, hit.fine ? 280 : 256, 1, muted, "ILLUSTRATIVE MATERIAL PRESETS");
    } else
        text(h, px + 16, 60, 1, muted, "POINT AT A VOXEL TO INSPECT IT");
    int tray_width = (MAT_COUNT - 1) * 66 + 14, tx = (width - tray_width) / 2, ty = height - 90;
    rect(h, (float)tx, (float)(ty - 28), (float)tray_width, 100, panel);
    snprintf(line, sizeof(line), "PLACE: %s", material_get(selected)->name);
    text(h, tx + 14, ty - 16, 1, white, line);
    for (int i = 1; i < MAT_COUNT; i++) {
        int x = tx + 10 + (i - 1) * 66;
        const Material *m = material_get((uint16_t)i);
        rect(h, (float)x, (float)ty, 60, 56, i == selected ? accent : muted);
        rect(h, (float)x + 2, (float)ty + 2, 56, 52, panel);
        rect(h, (float)x + 10, (float)ty + 8, 40, 28, v3(m->color[0], m->color[1], m->color[2]));
        snprintf(line, sizeof(line), "%d", i);
        text(h, x + 26, ty + 42, 1, white, line);
    }
    rect(h, (float)tx, (float)(ty - 82), (float)tray_width, 46, panel);
    text(h, tx + 12, ty - 70, 1, muted,
         workshop ? "WASD 60 MM/S / SHIFT 300 / ALT 5 / SPACE UP / CTRL DOWN"
                  : "WASD FLY / SPACE UP / CTRL DOWN / SHIFT FAST");
    text(h, tx + 12, ty - 56, 1, muted, "LMB REMOVE / RMB PLACE / MMB PICK / 1-8 SELECT");
    text(h, 18, height - 16, 1, muted,
         "F3 GEOMETRY / F4 WORKSHOP / F5 SAVE / F9 LOAD / TAB MOUSE / ESC QUIT");
    int cx = width / 2, cy = height / 2;
    rect(h, (float)cx - 7, (float)cy - 1, 15, 3, panel);
    rect(h, (float)cx - 1, (float)cy - 7, 3, 15, panel);
    rect(h, (float)cx - 6, (float)cy, 13, 1, white);
    rect(h, (float)cx, (float)cy - 6, 1, 13, white);
    if (!captured) {
        rect(h, (float)cx - 174, (float)cy + 32, 348, 54, panel);
        text(h, cx - 144, cy + 44, 2, white, "TAB TO EXPLORE");
        text(h, cx - 144, cy + 68, 1, muted, "MOUSE RELEASED / ESC TO QUIT");
    }
}

void hud_build_planet(Hud *h, Planet *p, const PlanetBody *body, PlanetHit hit, uint16_t selected, int width,
                      int height, float fps, size_t vertices, const PlanetScene *scene, bool fly, int brush,
                      bool sphere, const char *geometry, const char *status, const char *terrain_status) {
    h->count = 0;
    h->overflow = false;
    Vec3 panel = v3(.018f, .03f, .042f), white = v3(.88f, .93f, .94f), muted = v3(.48f, .58f, .62f),
         accent = v3(.37f, .79f, .59f);
    char line[160];
    DVec3 position = planet_position_m(body->position);
    PlanetGeo geo = planet_geographic(p, position);
    double terrain = planet_elevation(p, geo.latitude_deg, geo.longitude_deg),
           gravity = pd_length(planet_gravity(p, position));
    rect(h, 18, 18, 438, 238, panel);
    rect(h, 18, 18, 3, 238, accent);
    text(h, 34, 34, 2, white, "EARTH / VOXEL LABORATORY");
    snprintf(line, sizeof(line), "%.0f FPS / %zu TRIANGLES / 1 MM EDITS", fps, vertices / 3);
    text(h, 34, 64, 1, muted, line);
    snprintf(line, sizeof(line), "LAT %.6f / LON %.6f", geo.latitude_deg, geo.longitude_deg);
    text(h, 34, 82, 1, white, line);
    snprintf(line, sizeof(line), "ALT %.3f M / GROUND %.3f M", geo.altitude_m, terrain);
    text(h, 34, 98, 1, muted, line);
    snprintf(line, sizeof(line), "GRAVITY %.5f M/S2 / SPEED %.3f M/S", gravity,
             pd_length(body->velocity_mps));
    text(h, 34, 114, 1, white, line);
    snprintf(line, sizeof(line), "MASS %.5E KG / RADIUS %.3f KM", p->mass_kg, p->a_m / 1000);
    text(h, 34, 130, 1, muted, line);
    snprintf(line, sizeof(line), "%s / F6 FLY / F7 EARTH / F8 SPACE",
             fly              ? "FREE FLIGHT"
             : body->grounded ? "GROUNDED"
                              : "FALLING");
    text(h, 34, 150, 1, accent, line);
    snprintf(line, sizeof(line), "PATCHES %zu / QUEUE %zu / F3 %s", scene->visible, scene->pending, geometry);
    text(h, 34, 168, 1, muted, line);
    snprintf(line, sizeof(line), "WORLD DATA %.1f MIB / CACHE %zu / SAVED %zu",
             planet_storage_bytes(p) / 1048576.0, planet_resident_chunks(p), planet_saved_chunks(p));
    text(h, 34, 186, 1, muted, line);
    snprintf(line, sizeof(line), "%s %d MM / DETAIL %d MM / [ ] SIZE / B SHAPE", sphere ? "SPHERE" : "CUBE",
             brush, sphere ? (brush + 31) / 32 : 1);
    text(h, 34, 204, 1, accent, line);
    snprintf(line, sizeof(line), "DETAIL %zu AREAS / F10 %s", planet_detail_count(p), terrain_status);
    text(h, 34, 218, 1, muted, line);
    if (status && *status)
        text(h, 34, 236, 1, accent, status);
    int px = width - 332;
    rect(h, (float)px, 18, 314, hit.hit ? 192 : 70, panel);
    text(h, px + 16, 34, 1, accent, "PLANET MATERIAL INSPECTOR");
    if (hit.hit) {
        const Material *mat = material_get(hit.value.material);
        text(h, px + 16, 56, 2, white, mat->name);
        snprintf(line, sizeof(line), "DISTANCE %.4f M / %.2f K", hit.distance_m, hit.value.temperature_k);
        text(h, px + 16, 82, 1, muted, line);
        snprintf(line, sizeof(line), "1 MM MASS %.3f MG", mat->density_kg_m3 * .001);
        text(h, px + 16, 100, 1, white, line);
        for (int a = 0; a < 3; a++) {
            snprintf(line, sizeof(line), "%c MM %" PRId64, "XYZ"[a], hit.mm[a]);
            text(h, px + 16, 120 + a * 16, 1, muted, line);
        }
        text(h, px + 16, 178, 1, accent, "SATELLITE TERRAIN + WATER / F2 SUN");
    } else
        text(h, px + 16, 56, 1, muted, "POINT AT THE SURFACE TO EDIT");
    rect(h, width * .5f - 232, height - 70, 464, 52, panel);
    snprintf(line, sizeof(line), "PLACE %s / 1-9,0 OR WHEEL", material_get(selected)->name);
    text(h, width * .5f - 216, height - 56, 1, white, line);
    text(h, width * .5f - 216, height - 38, 1, muted, "LMB REMOVE / RMB PLACE / MMB PICK / TAB MOUSE");
    text(h, 20, height - 14, 1, muted,
         "WASD MOVE / SPACE JUMP / SHIFT FAST / ALT SLOW / F5 FLUSH / ESC RELEASE");
    float cx = width * .5f, cy = height * .5f;
    rect(h, cx - 7, cy - .5f, 5, 1, white);
    rect(h, cx + 2, cy - .5f, 5, 1, white);
    rect(h, cx - .5f, cy - 7, 1, 5, white);
    rect(h, cx - .5f, cy + 2, 1, 5, white);
}

void hud_resources(Hud *h, const ResourceStats *s, const ResourceScan *scan, int width, int height,
                   bool performance, bool materials, uint16_t selected) {
    Vec3 panel = v3(.018f, .03f, .042f), white = v3(.88f, .93f, .94f), muted = v3(.48f, .58f, .62f),
         accent = v3(.37f, .79f, .59f);
    char line[160];
    if (performance) {
        float y = 270;
        rect(h, 18, y, 438, 234, panel);
        text(h, 34, y + 14, 1, accent, "COMPUTER RESOURCES / F1 TOGGLE");

        snprintf(line, sizeof(line), "CPU %.1f%% (100 = ONE CORE) / UPDATE %.2F MS", s->cpu_percent,
                 s->update_ms);
        text(h, 34, y + 34, 1, white, line);
        if (s->system_bytes) {
            snprintf(line, sizeof(line), "PROCESS RAM %.1F MIB / SYSTEM FREE %.1F GIB",
                     s->process_bytes / 1048576.0, s->available_bytes / 1073741824.0);
            text(h, 34, y + 52, 1, white, line);
            rect(h, 34, y + 68, 400, 4, muted);
            rect(h, 34, y + 68, 400 * (1.0f - (float)s->available_bytes / s->system_bytes), 4, accent);
        } else
            text(h, 34, y + 52, 1, muted, "PROCESS RAM: UNAVAILABLE ON THIS PLATFORM");
        snprintf(line, sizeof(line), "GPU BUFFERS %.1F MIB / IMAGES %.1F MIB",
                 s->gpu_buffer_bytes / 1048576.0, (s->gpu_depth_bytes + s->gpu_texture_bytes) / 1048576.0);
        text(h, 34, y + 82, 1, white, line);
        snprintf(line, sizeof(line), "DEVICE LOCAL HEAPS %.1F GIB / NO ALLOCATION CAP",
                 s->gpu_heap_bytes / 1073741824.0);
        text(h, 34, y + 100, 1, muted, line);
        text(h, 34, y + 118, 1, muted, "FRAME TIME / LAST 120 FRAMES / SCALE 100 MS");
        rect(h, 34, y + 135, 400, 68, v3(.03f, .055f, .075f));
        rect(h, 34, y + 203 - 16.667f * .68f, 400, 1, muted);
        for (size_t i = 0; i < s->history_count; i++) {
            size_t slot = (s->history_next + RESOURCE_HISTORY - s->history_count + i) % RESOURCE_HISTORY;
            float ms = (float)s->history[slot], bar = fminf(100, fmaxf(0, ms)) * .68f;
            rect(h, 34 + i * (400.0f / RESOURCE_HISTORY), y + 203 - bar, 2, bar,
                 ms > 33.334 ? v3(.95f, .48f, .28f) : accent);
        }
        if (s->texture_only)
            snprintf(line, sizeof(line), "DISTANT EARTH / ENGINE TEXTURE / NO TERRAIN MESH");
        else
            snprintf(line, sizeof(line), "O OCCLUSION / HIDDEN %zu / GPU LOAD N/A", s->occluded_patches);
        text(h, 34, y + 214, 1, muted, line);
    }
    if (materials) {
        bool compact = height < 750;
        int x = width - 332, y = compact ? 220 : 238;
        int step = compact ? 4 : 8, delta = RESOURCE_SCAN_GRID * (8 - step), shorter = compact ? 24 : 0;
        rect(h, x, y, 314, 432 - delta - shorter, panel);
        text(h, x + 16, y + 14, 1, accent, "MATERIAL SLICE / F4 TOGGLE");
        if (!scan->ready) {
            text(h, x + 16, y + 36, 1, muted, "WAITING FOR MATERIAL SAMPLES");
        } else {
            snprintf(line, sizeof(line), "DEPTH %.3F M / WIDTH %.3F M", scan->depth_m, scan->span_m);
            text(h, x + 16, y + 34, 1, white, line);
            snprintf(line, sizeof(line), "SPACING %.3F MM / FORWARD AT TOP",
                     scan->span_m * 1000 / (RESOURCE_SCAN_GRID - 1));
            text(h, x + 16, y + 50, 1, muted, line);
            int mx = x + (314 - RESOURCE_SCAN_GRID * step) / 2, my = y + 72;
            for (int row = 0; row < RESOURCE_SCAN_GRID; row++)
                for (int col = 0; col < RESOURCE_SCAN_GRID; col++) {
                    const Material *m = material_get(scan->materials[row * RESOURCE_SCAN_GRID + col]);
                    rect(h, mx + col * step, my + row * step, step, step,
                         v3(m->color[0], m->color[1], m->color[2]));
                }
            rect(h, mx + 12 * step, my + 12 * step, step, 1, white);
            rect(h, mx + 12 * step, my + 12 * step + step - 1, step, 1, white);
            text(h, x + 16, y + 286 - delta, 1, accent, "ACTUAL VOXEL SAMPLES / NOT DEPOSIT VOLUME");
            for (int id = 0; id < MAT_COUNT; id++) {
                const Material *m = material_get((uint16_t)id);
                int lx = x + 16 + (id % 2) * 146, ly = y + 306 - delta + (id / 2) * (compact ? 10 : 14);
                rect(h, lx, ly, 7, 7, v3(m->color[0], m->color[1], m->color[2]));
                snprintf(line, sizeof(line), "%s %.1F%%", m->name,
                         100.0 * scan->counts[id] / (RESOURCE_SCAN_GRID * RESOURCE_SCAN_GRID));
                text(h, lx + 11, ly, 1, id == selected ? accent : muted, line);
            }
        }
        text(h, x + 16, y + 400 - delta - shorter, 1, white, "PGUP/PGDN DEPTH / - = ZOOM");
        text(h, x + 16, y + 416 - delta - shorter, 1, muted, "LOCAL TANGENT SLICE / AIM OR GROUND CENTER");
    }
    text(h, 20, height - 28, 1, muted, "F1 COMPUTER RESOURCES / F4 MATERIALS / F2 AIM SUN");
}

void hud_relativity(Hud *h, int width, int height, double time, double scale, double rate, double deficit) {
    (void)height;
    Vec3 panel = v3(.018f, .03f, .042f), accent = v3(.37f, .79f, .59f), muted = v3(.48f, .58f, .62f);
    int x = width - 332;
    char line[160];
    rect(h, (float)x, 230, 314, 102, panel);
    text(h, x + 16, 244, 1, accent, "SOLAR SYSTEM / F11 INSPECT");
    snprintf(line, sizeof(line), "J2000 + %.3F DAYS / %.0F X", time / 86400, scale);
    text(h, x + 16, 262, 1, muted, line);
    snprintf(line, sizeof(line), "CLOCK RATE %.12F", rate);
    text(h, x + 16, 280, 1, muted, line);
    snprintf(line, sizeof(line), "CLOCK LAG %.6F MS", deficit * 1000);
    text(h, x + 16, 298, 1, muted, line);
    text(h, x + 16, 316, 1, accent, "SR + 1PN / F9 SPEED / F12 PAUSE");
}
void hud_build_solar(Hud *h, const SolarSystem *s, int focus, int width, int height, float fps, double scale,
                     double speed, double rate, double deficit) {
    h->count = 0;
    h->overflow = false;
    Vec3 panel = v3(.018f, .03f, .042f), white = v3(.88f, .93f, .94f), accent = v3(.37f, .79f, .59f),
         muted = v3(.48f, .58f, .62f);
    char line[160];
    rect(h, 18, 18, 438, 240, panel);
    text(h, 34, 34, 2, white, "SOLAR SYSTEM / 1:1 SCALE");
    const SolarBody *body = &s->bodies[focus];
    snprintf(line, sizeof(line), "FOCUS %s / LEFT RIGHT TO SELECT", body->name);
    text(h, 34, 60, 1, accent, line);
    snprintf(line, sizeof(line), "%.0F FPS / RADIUS %.1F KM", fps, body->radius_m / 1000);
    text(h, 34, 82, 1, muted, line);
    snprintf(line, sizeof(line), "SUN DISTANCE %.5F AU",
             pd_length(pd_sub(body->position_m, s->bodies[0].position_m)) / ASTRONOMICAL_UNIT_M);
    text(h, 34, 100, 1, muted, line);
    snprintf(line, sizeof(line), "MASS %.5E KG / ORBIT SPEED %.1F M/S", body->mass_kg,
             pd_length(body->velocity_mps));
    text(h, 34, 118, 1, muted, line);
    snprintf(line, sizeof(line), "CAMERA %.5F C / GAMMA %.9F", speed / LIGHT_SPEED_MPS,
             relativity_gamma(speed));
    text(h, 34, 136, 1, white, line);
    text(h, 34, 158, 1, accent, "F11 EARTH / F7 REFOCUS / F2 SUN");
    text(h, 34, 178, 1, muted, "WASD / SPACE CTRL / SHIFT FAST / ALT SLOW");
    text(h, 34, 198, 1, muted, "PHYSICAL ORBITS / PROCEDURAL PLANET APPEARANCE");
    text(h, 34, 218, 1, accent, "EDITABLE TERRAIN IS ON EARTH");
    hud_relativity(h, width, height, s->time_s, scale, rate, deficit);
    /* A logarithmic overview makes true-size planets discoverable without inflating them. */
    int y = height - 160;
    rect(h, 18, (float)y, (float)(width - 36), 136, panel);
    text(h, 34, y + 12, 1, muted, "ORBIT OVERVIEW / LOG DISTANCE / DIAGRAM MARKERS ARE ENLARGED");
    float span = (float)(width - 130);
    for (int i = 0; i < 9; i++) {
        double au = pd_length(pd_sub(s->bodies[i].position_m, s->bodies[0].position_m)) / ASTRONOMICAL_UNIT_M;
        float x = 60 + span * (float)(log1p(au) / log(32.0));
        rect(h, x - 3, (float)y + 50, 6, 6, i == focus ? white : s->bodies[i].color);
        text(h, (int)x - 15, y + 68 + (i % 2) * 16, 1, i == focus ? accent : muted, s->bodies[i].name);
    }
}
