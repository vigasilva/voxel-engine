#include "elements.h"
#include <math.h>
#include <string.h>
static const Element elements[E_COUNT] = {
#include "elements.inc"
};
const Element *element_get(ElementId id) {
    return id >= 0 && id < E_COUNT ? &elements[id] : NULL;
}
const Element *element_by_number(unsigned number) {
    return number >= 1 && number <= E_COUNT ? &elements[number - 1] : NULL;
}
const Element *element_find(const char *symbol) {
    if (!symbol)
        return NULL;
    for (int i = 0; i < E_COUNT; i++)
        if (!strcmp(elements[i].symbol, symbol))
            return &elements[i];
    return NULL;
}
const char *element_symbol(ElementId id) {
    const Element *e = element_get(id);
    return e ? e->symbol : "?";
}
const char *phase_name(PhysicalPhase phase) {
    static const char *names[] = {"unknown",       "solid",      "liquid",   "gas",  "plasma",
                                  "supercritical", "coexisting", "critical", "fluid"};
    return phase >= PHASE_UNKNOWN && phase <= PHASE_FLUID ? names[phase] : "unknown";
}
bool elements_validate(void) {
    for (int i = 0; i < E_COUNT; i++) {
        const Element *e = &elements[i];
        if (e->atomic_number != i + 1 || !e->name || !e->symbol || !e->group || !isfinite(e->atomic_mass_u) ||
            e->atomic_mass_u <= 0 || e->standard_phase < PHASE_SOLID || e->standard_phase > PHASE_GAS)
            return false;
        for (int j = 0; j < i; j++)
            if (!strcmp(e->symbol, elements[j].symbol))
                return false;
        const double properties[] = {e->reference_density_kg_m3, e->melting_k, e->boiling_k};
        for (int j = 0; j < 3; j++)
            if (!isnan(properties[j]) && (!isfinite(properties[j]) || properties[j] <= 0))
                return false;
    }
    return true;
}
