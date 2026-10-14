#include "composition.h"
#include <math.h>
static bool publish(Composition *out, const double masses[E_COUNT]) {
    double total = 0;
    unsigned count = 0;
    for (int i = 0; i < E_COUNT; i++) {
        if (!isfinite(masses[i]) || masses[i] < 0)
            return false;
        total += masses[i];
        count += masses[i] > 0;
    }
    if (!out || !count || count > COMPOSITION_CAPACITY || !isfinite(total) || total <= 0)
        return false;
    Composition result = {0};
    for (int i = 0; i < E_COUNT; i++)
        if (masses[i] > 0) {
            float fraction = (float)(masses[i] / total);
            if (fraction == 0)
                return false;
            result.constituents[result.count++] = (Constituent){(ElementId)i, fraction};
        }
    *out = result;
    return true;
}
bool composition_from_atoms(Composition *out, const AtomRatio *ratios, size_t count) {
    if (!out || !ratios || !count)
        return false;
    double masses[E_COUNT] = {0};
    for (size_t i = 0; i < count; i++) {
        const Element *e = element_get(ratios[i].element);
        if (!e || !isfinite(ratios[i].atoms) || ratios[i].atoms < 0)
            return false;
        masses[ratios[i].element] += ratios[i].atoms * e->atomic_mass_u;
    }
    return publish(out, masses);
}
bool composition_from_fractions(Composition *out, const Constituent *parts, size_t count) {
    if (!out || !parts || !count)
        return false;
    double masses[E_COUNT] = {0};
    for (size_t i = 0; i < count; i++) {
        if (!element_get(parts[i].element) || !isfinite(parts[i].mass_fraction) || parts[i].mass_fraction < 0)
            return false;
        masses[parts[i].element] += parts[i].mass_fraction;
    }
    return publish(out, masses);
}
bool composition_valid(const Composition *c) {
    if (!c || !c->count || c->count > COMPOSITION_CAPACITY)
        return false;
    double sum = 0;
    for (unsigned i = 0; i < c->count; i++) {
        Constituent part = c->constituents[i];
        if (!element_get(part.element) || !isfinite(part.mass_fraction) || part.mass_fraction <= 0)
            return false;
        for (unsigned j = 0; j < i; j++)
            if (c->constituents[j].element == part.element)
                return false;
        sum += part.mass_fraction;
    }
    return fabs(sum - 1) < 1e-6;
}
bool composition_mix(Composition *out, const Composition *parts, const double *weights, size_t count) {
    if (!out || !parts || !weights || !count)
        return false;
    double masses[E_COUNT] = {0};
    for (size_t i = 0; i < count; i++) {
        if (!composition_valid(&parts[i]) || !isfinite(weights[i]) || weights[i] < 0)
            return false;
        for (unsigned j = 0; j < parts[i].count; j++) {
            Constituent part = parts[i].constituents[j];
            masses[part.element] += part.mass_fraction * weights[i];
        }
    }
    return publish(out, masses);
}
double composition_fraction(const Composition *c, ElementId element) {
    if (!composition_valid(c) || !element_get(element))
        return NAN;
    for (unsigned i = 0; i < c->count; i++)
        if (c->constituents[i].element == element)
            return c->constituents[i].mass_fraction;
    return 0;
}
bool material_composition(uint16_t id, Composition *out) {
    if (id >= MAT_COUNT)
        return false;
    const Material *material = material_get(id);
    return composition_from_fractions(out, material->composition, material->constituent_count);
}
