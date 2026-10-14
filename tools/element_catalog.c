#include "elements.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static void show(const Element *e) {
    printf("%3u %-2s %-15s mass %.9g u | %s%s | %s\n", e->atomic_number, e->symbol, e->name, e->atomic_mass_u,
           phase_name(e->standard_phase), e->phase_predicted ? " (predicted)" : "", e->group);
}
int main(int argc, char **argv) {
    if (!elements_validate()) {
        fputs("Invalid element catalogue\n", stderr);
        return 1;
    }
    if (argc == 1) {
        for (unsigned n = 1; n <= E_COUNT; n++)
            show(element_by_number(n));
        return 0;
    }
    const Element *e = argc == 2 ? element_find(argv[1]) : NULL;
    if (!e) {
        fputs("Usage: element_catalog [case-sensitive element symbol]\n", stderr);
        return 1;
    }
    show(e);
    const double values[] = {e->reference_density_kg_m3, e->melting_k, e->boiling_k};
    const char *names[] = {"Reference density (kg/m3)", "Source melting point (K)",
                           "Source boiling point (K)"};
    for (int i = 0; i < 3; i++) {
        printf("%s: ", names[i]);
        if (isfinite(values[i]))
            printf("%.9g\n", values[i]);
        else
            puts("unknown");
    }
    puts("PubChem reference values; density conditions vary, and phase boundaries need a pressure model.");
    return 0;
}
