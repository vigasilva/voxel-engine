#include "material.h"
#include <math.h>
/* Approximate illustrative recipes, not measured geological/biological samples. */
static const Material materials[MAT_COUNT] = {
    [MAT_AIR] = {"Air", {0.55f, 0.72f, 0.85f}, 1.225f, 1005, 0.026f, false, 0, {{0, 0}}},
    [MAT_SOIL] = {"Soil",
                  {0.39f, 0.27f, 0.16f},
                  1400,
                  900,
                  0.5f,
                  true,
                  4,
                  {{E_O, .50f}, {E_SI, .30f}, {E_AL, .10f}, {E_C, .10f}}},
    [MAT_GRASS] = {"Grass soil",
                   {0.29f, 0.53f, 0.20f},
                   1350,
                   1000,
                   0.45f,
                   true,
                   4,
                   {{E_O, .50f}, {E_SI, .28f}, {E_C, .15f}, {E_H, .07f}}},
    [MAT_STONE] = {"Stone",
                   {0.48f, 0.51f, 0.54f},
                   2700,
                   790,
                   2.5f,
                   true,
                   4,
                   {{E_O, .48f}, {E_SI, .32f}, {E_AL, .12f}, {E_CA, .08f}}},
    [MAT_SAND] = {"Sand", {0.79f, 0.69f, 0.43f}, 1600, 830, 0.27f, true, 2, {{E_SI, .4674f}, {E_O, .5326f}}},
    [MAT_IRON] = {"Iron", {0.43f, 0.47f, 0.52f}, 7874, 449, 80.4f, true, 1, {{E_FE, 1}}},
    [MAT_COPPER] = {"Copper", {0.78f, 0.40f, 0.21f}, 8960, 385, 401, true, 1, {{E_CU, 1}}},
    [MAT_WOOD] =
        {"Wood", {0.50f, 0.33f, 0.16f}, 650, 1700, 0.12f, true, 3, {{E_C, .50f}, {E_O, .44f}, {E_H, .06f}}},
    [MAT_CLAY] = {"Clay",
                  {0.67f, 0.37f, 0.26f},
                  1700,
                  880,
                  0.8f,
                  true,
                  4,
                  {{E_O, .56f}, {E_SI, .22f}, {E_AL, .20f}, {E_H, .02f}}},
    [MAT_WATER] = {"Water", {.04f, .22f, .48f}, 1000, 4184, .6f, false, 2, {{E_H, .1119f}, {E_O, .8881f}}},
    [MAT_ICE] = {"Ice", {.82f, .9f, .96f}, 917, 2108, 2.2f, true, 2, {{E_H, .1119f}, {E_O, .8881f}}}};
const Material *material_get(uint16_t id) {
    return &materials[id < MAT_COUNT ? id : MAT_AIR];
}
bool materials_validate(void) {
    if (!elements_validate())
        return false;
    for (int i = 0; i < MAT_COUNT; i++) {
        const Material *m = &materials[i];
        float sum = 0;
        if (m->constituent_count > COMPOSITION_CAPACITY || m->density_kg_m3 <= 0 ||
            m->specific_heat_j_kg_k <= 0)
            return false;
        for (int j = 0; j < m->constituent_count; j++) {
            Constituent c = m->composition[j];
            if (c.element < 0 || c.element >= E_COUNT || !isfinite(c.mass_fraction) || c.mass_fraction < 0)
                return false;
            sum += c.mass_fraction;
        }
        if (m->solid && fabsf(sum - 1) > 0.0001f)
            return false;
    }
    return true;
}
