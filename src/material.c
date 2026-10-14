#include "material.h"
#include <math.h>
#include <ctype.h>
#include <string.h>
static const Material materials[MAT_COUNT] = {
#include "materials.inc"
};
_Static_assert(MAT_COUNT <= 256, "Expand PTEX material encoding before adding more materials");
const Material *material_get(uint16_t id) {
    return &materials[id < MAT_COUNT ? id : MAT_AIR];
}
static bool equal_name(const char *a, const char *b) {
    while (*a && *b)
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
            return false;
    return !*a && !*b;
}
const Material *material_find(const char *name) {
    if (!name)
        return NULL;
    for (int i = 0; i < MAT_COUNT; i++)
        if (equal_name(name, materials[i].name))
            return &materials[i];
    return NULL;
}
const char *material_family_name(MaterialFamily family) {
    static const char *names[] = {"generic", "soil",    "rock",  "mineral",  "sand",
                                  "metal",   "organic", "water", "volatile", "element"};
    return family >= FAMILY_GENERIC && family <= FAMILY_ELEMENT ? names[family] : "unknown";
}
uint16_t material_element_id(ElementId element) {
    return element_get(element) ? (uint16_t)(MAT_ELEMENT_FIRST + element) : MAT_AIR;
}
bool materials_validate(void) {
    if (!elements_validate())
        return false;
    for (int i = 0; i < MAT_COUNT; i++) {
        const Material *m = &materials[i];
        if (!m->name || m->constituent_count > COMPOSITION_CAPACITY || !m->constituent_count ||
            m->family < FAMILY_GENERIC || m->family > FAMILY_ELEMENT || m->reference_phase < PHASE_SOLID ||
            m->reference_phase > PHASE_GAS || m->solid != (m->reference_phase == PHASE_SOLID))
            return false;
        const float properties[] = {m->density_kg_m3, m->specific_heat_j_kg_k, m->conductivity_w_m_k};
        for (int j = 0; j < 3; j++)
            if ((!isnan(properties[j]) && (!isfinite(properties[j]) || properties[j] <= 0)) ||
                (isnan(properties[j]) && m->pure_element < 0))
                return false;
        if (m->pure_element >= 0 && !element_get((ElementId)m->pure_element))
            return false;
        double sum = 0;
        for (int j = 0; j < m->constituent_count; j++) {
            Constituent c = m->composition[j];
            if (!element_get(c.element) || !isfinite(c.mass_fraction) || c.mass_fraction <= 0)
                return false;
            for (int k = 0; k < j; k++)
                if (m->composition[k].element == c.element)
                    return false;
            sum += c.mass_fraction;
        }
        if (fabs(sum - 1) > .000001)
            return false;
        if (m->pure_element >= 0 &&
            (m->constituent_count != 1 || m->composition[0].element != (ElementId)m->pure_element))
            return false;
        for (int c = 0; c < 3; c++)
            if (!isfinite(m->color[c]) || m->color[c] < 0 || m->color[c] > 1)
                return false;
    }
    return true;
}
