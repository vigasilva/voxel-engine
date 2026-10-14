#include "composition.h"
#include "material_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static void property(const char *name, double value) {
    printf("%s: ", name);
    if (isfinite(value))
        printf("%.9g\n", value);
    else
        puts("unknown");
}
int main(int argc, char **argv) {
    if (!materials_validate()) {
        fputs("Invalid material catalogue\n", stderr);
        return 1;
    }
    if (argc == 1) {
        for (uint16_t id = 0; id < MAT_COUNT; id++) {
            const Material *m = material_get(id);
            printf("%3u %-27s %-8s %s\n", id, m->name, material_family_name(m->family),
                   phase_name(m->reference_phase));
        }
        return 0;
    }
    const Material *m = argc == 2 || argc == 4 ? material_find(argv[1]) : NULL;
    if (!m) {
        fputs("Usage: material_catalog [quoted material name [temperature_K pressure_Pa]]\n", stderr);
        return 1;
    }
    printf("%s / %s / reference %s\n", m->name, material_family_name(m->family),
           phase_name(m->reference_phase));
    property("Reference bulk density (kg/m3)", m->density_kg_m3);
    property("Specific heat (J/kg/K)", m->specific_heat_j_kg_k);
    property("Conductivity (W/m/K)", m->conductivity_w_m_k);
    for (unsigned i = 0; i < m->constituent_count; i++)
        printf("%s %.6f%% by mass\n", element_symbol(m->composition[i].element),
               100 * m->composition[i].mass_fraction);
    puts(m->properties_estimated ? "Representative preset; bulk properties/composition vary in nature."
                                 : "Element reference data; unknown transport properties remain unknown.");
    if (argc == 4) {
        char *end;
        double t = strtod(argv[2], &end);
        if (end == argv[2] || *end || !isfinite(t) || t <= 0) {
            fputs("Temperature must be a finite positive value in kelvin\n", stderr);
            return 1;
        }
        double p = strtod(argv[3], &end);
        if (end == argv[3] || *end || !isfinite(p) || p < 0) {
            fputs("Absolute pressure must be a finite nonnegative value in pascals\n", stderr);
            return 1;
        }
        uint16_t id = 0;
        while (id < MAT_COUNT && material_get(id) != m)
            id++;
        MaterialState state = material_state_query(id, t, p);
        printf("State at %.9g K / %.9g Pa: %s (%s; %s)\n", t, p, phase_name(state.phase),
               state_status_name(state.status), state_model_name(state.model));
        printf("Possible phases:");
        for (int phase = PHASE_SOLID; phase <= PHASE_FLUID; phase++)
            if (state.phases & (1u << phase))
                printf(" %s", phase_name((PhysicalPhase)phase));
        puts(state.phases ? "" : " unknown");
        property("Melting boundary (K)", state.melting_k);
        property("Boiling boundary (K)", state.boiling_k);
        property("Vapor equilibrium pressure (Pa)", state.vapor_pressure_pa);
        puts("State query only; stored matter and reference properties are not changed.");
    }
    return 0;
}
