#ifndef ELEMENTS_H
#define ELEMENTS_H
#include "element_ids.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum { PHASE_UNKNOWN, PHASE_SOLID, PHASE_LIQUID, PHASE_GAS, PHASE_PLASMA } PhysicalPhase;
typedef struct {
    uint8_t atomic_number;
    const char *symbol, *name;
    double atomic_mass_u;
    /* Reference-source values. Missing values are NAN; density conditions
       vary by entry. These are not properties at arbitrary T/P. */
    double reference_density_kg_m3, melting_k, boiling_k;
    PhysicalPhase standard_phase;
    bool phase_predicted;
    const char *group;
} Element;
const Element *element_get(ElementId id);
const Element *element_by_number(unsigned atomic_number);
const Element *element_find(const char *symbol);
const char *element_symbol(ElementId id);
const char *phase_name(PhysicalPhase phase);
bool elements_validate(void);
#endif
