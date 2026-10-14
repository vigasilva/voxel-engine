#include "composition.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(v)                                                                                             \
    do {                                                                                                     \
        if (!(v)) {                                                                                          \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #v);                                          \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
int main(void) {
    CHECK(elements_validate() && materials_validate());
    for (unsigned n = 1; n <= 118; n++) {
        const Element *e = element_by_number(n);
        CHECK(e && e->atomic_number == n && element_find(e->symbol) == e &&
              element_get((ElementId)(n - 1)) == e);
    }
    CHECK(!element_by_number(0) && !element_by_number(119) && !element_get((ElementId)-1));
    CHECK(!element_get(E_COUNT) && !element_find("Invalid") && !element_find(NULL));
    CHECK(!strcmp(element_symbol(E_FE), "Fe") && !strcmp(element_symbol(E_OG), "Og"));
    CHECK(!strcmp(element_symbol((ElementId)-1), "?"));
    CHECK(element_get(E_FE)->reference_density_kg_m3 == 7874);
    CHECK(element_get(E_H)->standard_phase == PHASE_GAS);
    CHECK(element_get(E_HG)->standard_phase == PHASE_LIQUID);
    CHECK(element_get(E_OG)->phase_predicted && isnan(element_get(E_OG)->reference_density_kg_m3));
    Composition water = {0}, quartz = {0}, salt = {0};
    AtomRatio h2o[] = {{E_H, 2}, {E_O, 1}}, sio2[] = {{E_SI, 1}, {E_O, 2}}, nacl[] = {{E_NA, 1}, {E_CL, 1}};
    CHECK(composition_from_atoms(&water, h2o, 2) && composition_valid(&water));
    CHECK(composition_from_atoms(&quartz, sio2, 2));
    CHECK(composition_from_atoms(&salt, nacl, 2));
    CHECK(fabs(composition_fraction(&water, E_H) - .1119) < .0001);
    CHECK(fabs(composition_fraction(&quartz, E_SI) - .4674) < .0001);
    CHECK(fabs(composition_fraction(&salt, E_NA) - .3934) < .0001);
    CHECK(composition_fraction(&salt, E_FE) == 0);
    Composition parts[] = {water, salt}, mixed;
    double weights[] = {97, 3};
    CHECK(composition_mix(&mixed, parts, weights, 2) && composition_valid(&mixed));
    CHECK(fabs(composition_fraction(&mixed, E_NA) - composition_fraction(&salt, E_NA) * .03) < 1e-7);
    CHECK(fabs(composition_fraction(&mixed, E_H) - composition_fraction(&water, E_H) * .97) < 1e-7);
    Composition old = mixed;
    AtomRatio invalid[] = {{E_H, -1}};
    CHECK(!composition_from_atoms(&mixed, invalid, 1) && !memcmp(&old, &mixed, sizeof(old)));
    invalid[0].atoms = INFINITY;
    CHECK(!composition_from_atoms(&mixed, invalid, 1));
    weights[0] = NAN;
    CHECK(!composition_mix(&mixed, parts, weights, 2));
    Constituent repeated[] = {{E_FE, 1}, {E_FE, 2}, {E_O, 0}};
    CHECK(composition_from_fractions(&mixed, repeated, 3));
    CHECK(mixed.count == 1 && composition_fraction(&mixed, E_FE) == 1);
    Constituent many[COMPOSITION_CAPACITY + 1];
    for (int i = 0; i <= COMPOSITION_CAPACITY; i++)
        many[i] = (Constituent){(ElementId)i, 1};
    CHECK(!composition_from_fractions(&mixed, many, COMPOSITION_CAPACITY + 1));
    for (int id = 1; id < MAT_COUNT; id++)
        CHECK(material_composition((uint16_t)id, &mixed) && composition_valid(&mixed));
    CHECK(!material_composition(MAT_COUNT, &mixed));
    puts("PASS: all 118 elements, missing/predicted properties, stoichiometry, mass mixtures, failure "
         "atomicity");
    return 0;
}
