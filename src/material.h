#ifndef MATERIAL_H
#define MATERIAL_H
#include "elements.h"
#include <stdbool.h>
#include <stdint.h>
#define COMPOSITION_CAPACITY 16
/* Stable IDs are part of the save format. Never reorder existing materials. */
typedef enum {
    MAT_AIR,
    MAT_SOIL,
    MAT_GRASS,
    MAT_STONE,
    MAT_SAND,
    MAT_IRON,
    MAT_COPPER,
    MAT_WOOD,
    MAT_CLAY,
    MAT_WATER,
    MAT_ICE,
    MAT_COUNT
} MaterialId;
typedef struct {
    ElementId element;
    float mass_fraction;
} Constituent;
typedef struct {
    const char *name;
    float color[3];
    float density_kg_m3;
    float specific_heat_j_kg_k;
    float conductivity_w_m_k;
    bool solid;
    uint8_t constituent_count;
    Constituent composition[COMPOSITION_CAPACITY];
} Material;
const Material *material_get(uint16_t id);
bool materials_validate(void);
#endif
