#ifndef MATERIAL_H
#define MATERIAL_H
#include "elements.h"
#include <stdbool.h>
#include <stdint.h>
#define COMPOSITION_CAPACITY 16
#include "material_ids.h"
typedef enum {
    FAMILY_GENERIC,
    FAMILY_SOIL,
    FAMILY_ROCK,
    FAMILY_MINERAL,
    FAMILY_SAND,
    FAMILY_METAL,
    FAMILY_ORGANIC,
    FAMILY_WATER,
    FAMILY_VOLATILE,
    FAMILY_ELEMENT
} MaterialFamily;
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
    MaterialFamily family;
    PhysicalPhase reference_phase;
    bool natural_surface, properties_estimated;
    int16_t pure_element; /* Atomic-number-minus-one, or -1 for a compound/mixture. */
} Material;
const Material *material_get(uint16_t id);
const Material *material_find(const char *name);
const char *material_family_name(MaterialFamily family);
uint16_t material_element_id(ElementId element);
bool materials_validate(void);
#endif
