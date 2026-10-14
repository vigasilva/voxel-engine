#include "composition.h"
#include "detail.h"
#include "material_picker.h"
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
    CHECK(MAT_AIR == 0 && MAT_COPPER == 6 && MAT_WATER == 9 && MAT_ICE == 10);
    CHECK(MAT_COUNT >= MAT_ELEMENT_FIRST + E_COUNT);
    for (int id = 0; id < MAT_COUNT; id++) {
        CHECK(material_composition((uint16_t)id, &mixed) && composition_valid(&mixed));
        Voxel value = {(uint16_t)id, 0, 137.25f};
        DetailChunk *d = detail_create(value);
        CHECK(d);
        size_t bytes = detail_encoded_size(d), at = 0;
        unsigned char *data = malloc(bytes);
        CHECK(data);
        detail_encode(d, data, &at);
        CHECK(at == bytes);
        at = 0;
        DetailChunk *decoded = detail_decode(data, bytes, &at);
        CHECK(decoded && at == bytes && voxel_equal(detail_get(decoded, 100, 101, 102), value));
        detail_destroy(decoded);
        detail_destroy(d);
        free(data);
    }
    for (int element = 0; element < E_COUNT; element++) {
        uint16_t id = material_element_id((ElementId)element);
        const Material *m = material_get(id);
        CHECK(m->pure_element == element && m->constituent_count == 1 &&
              m->composition[0].mass_fraction == 1);
    }
    CHECK(material_element_id((ElementId)-1) == MAT_AIR);
    CHECK(material_find("bAsAlT") == material_get(MAT_BASALT) && !material_find("missing"));
    CHECK(isnan(material_get(MAT_ELEMENT_OG)->density_kg_m3));
    CHECK(material_get(MAT_GRANITE)->natural_surface && !material_get(MAT_COPPER)->natural_surface);
    Composition calcite;
    CHECK(material_composition(MAT_CALCITE, &calcite));
    CHECK(fabs(composition_fraction(&calcite, E_CA) - .4004) < .001);
    MaterialPicker picker = {0};
    material_picker_open(&picker, MAT_GRANITE);
    CHECK(picker.active && picker.count == MAT_COUNT - 1 && material_picker_selected(&picker) == MAT_GRANITE);
    material_picker_filter(&picker, "basalt");
    CHECK(picker.count == 1 && material_picker_selected(&picker) == MAT_BASALT);
    material_picker_filter(&picker, "element og");
    CHECK(picker.count == 1 && material_picker_selected(&picker) == MAT_ELEMENT_OG);
    material_picker_filter(&picker, "not-a-real-material");
    CHECK(!picker.count && material_picker_selected(&picker) == MAT_AIR);
    material_picker_filter(&picker, "sand");
    CHECK(picker.count >= 5);
    material_picker_step(&picker, 1000000);
    CHECK(picker.cursor == picker.count - 1);
    material_picker_step(&picker, -1000000);
    CHECK(picker.cursor == 0);
    material_picker_filter(&picker, "basal");
    material_picker_append(&picker, "t");
    CHECK(!strcmp(picker.query, "basalt") && material_picker_selected(&picker) == MAT_BASALT);
    material_picker_backspace(&picker);
    CHECK(!strcmp(picker.query, "basal"));
    puts("PASS: all 118 elements, missing/predicted properties, stoichiometry, mass mixtures, failure "
         "atomicity");
    return 0;
}
