#ifndef MATERIAL_H
#define MATERIAL_H
#include <stdbool.h>
#include <stdint.h>
#define COMPOSITION_CAPACITY 6
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
typedef enum { E_H, E_C, E_O, E_AL, E_SI, E_CA, E_FE, E_CU, E_COUNT } ElementId;
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
const char *element_symbol(ElementId id);
bool materials_validate(void);
#endif
