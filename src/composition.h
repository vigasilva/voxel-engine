#ifndef COMPOSITION_H
#define COMPOSITION_H
#include "material.h"
#include <stddef.h>
typedef struct {
    uint8_t count;
    Constituent constituents[COMPOSITION_CAPACITY];
} Composition;
typedef struct {
    ElementId element;
    double atoms;
} AtomRatio;
/* All builders publish only on success. Fractions are elemental mass fractions,
   not molecule IDs, bonds, mineral phases, or a reaction/equilibrium solver. */
bool composition_from_atoms(Composition *out, const AtomRatio *ratios, size_t count);
bool composition_from_fractions(Composition *out, const Constituent *parts, size_t count);
bool composition_mix(Composition *out, const Composition *parts, const double *mass_weights, size_t count);
bool composition_valid(const Composition *composition);
double composition_fraction(const Composition *composition, ElementId element);
bool material_composition(uint16_t material, Composition *out);
#endif
