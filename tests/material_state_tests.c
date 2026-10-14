#include "material_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(e)                                                                                             \
    do {                                                                                                     \
        if (!(e)) {                                                                                          \
            fprintf(stderr, "%s:%d %s\n", __FILE__, __LINE__, #e);                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static void close_value(double value, double expected, double tolerance) {
    if (!isfinite(value) || fabs(value - expected) > tolerance)
        fprintf(stderr, "Reference check: %.12g, expected %.12g +/- %.12g\n", value, expected, tolerance);
    CHECK(isfinite(value) && fabs(value - expected) <= tolerance);
}
static void water_phase(double t, double p, PhysicalPhase expected) {
    MaterialState s = material_state_query(MAT_WATER, t, p);
    CHECK(s.model == STATE_MODEL_WATER_IAPWS && s.status == STATE_EQUILIBRIUM && s.phase == expected);
    CHECK(s.phases == (1u << expected));
}
int main(void) {
    /* Published computer-verification values, not values calculated from the
       implementation under test. IAPWS SR1-86 table 1 and R14-08 table 3. */
    close_value(water_saturation_pressure_pa(273.16), 611.657, .0001);
    close_value(water_saturation_pressure_pa(373.1243), 101325, .2);
    close_value(water_saturation_pressure_pa(647.096), 22064000, .001);
    close_value(water_sublimation_pressure_pa(230), 8.94735, .00001);
    close_value(water_melting_pressure_pa(260, ICE_IH), 138268000, 500);
    close_value(water_melting_pressure_pa(254, ICE_III), 268685000, 500);
    close_value(water_melting_pressure_pa(265, ICE_V), 479640000, 500);
    close_value(water_melting_pressure_pa(320, ICE_VI), 1356760000, 5000);
    close_value(water_melting_temperature_k(138268000), 260, .0001);
    close_value(water_melting_temperature_k(268685000), 254, .0001);
    close_value(water_melting_temperature_k(479640000), 265, .0001);
    close_value(water_melting_temperature_k(1356760000), 320, .0002);
    close_value(water_boiling_temperature_k(101325), 373.1243, .001);
    CHECK(water_boiling_temperature_k(50000) < 355 && water_boiling_temperature_k(200000) > 393);
    CHECK(water_melting_temperature_k(100000000) < 266);
    CHECK(water_melting_temperature_k(1000000000) > 298);
    /* Pressure changes first depress freezing, then select the higher-ice
       melting curves. Check valid phases on both sides throughout the range. */
    for (int i = 0; i <= 80; i++) {
        double p = 1000 * pow(1e6, i / 80.0), tm = water_melting_temperature_k(p);
        CHECK(isfinite(tm));
        WaterIce ice = p < 208566000 ? ICE_IH : p < 350100000 ? ICE_III : p < 632400000 ? ICE_V : ICE_VI;
        close_value(water_melting_pressure_pa(tm, ice), p, fmax(.01, p * 2e-7));
        water_phase(tm - .1, p, PHASE_SOLID);
        water_phase(tm + .1, p, PHASE_LIQUID);
    }
    water_phase(250, 101325, PHASE_SOLID);
    water_phase(300, 101325, PHASE_LIQUID);
    water_phase(400, 101325, PHASE_GAS);
    water_phase(360, 50000, PHASE_GAS);
    water_phase(360, 200000, PHASE_LIQUID);
    water_phase(260, 200000000, PHASE_LIQUID);    /* Pressure-depressed freezing. */
    water_phase(298.15, 1000000000, PHASE_SOLID); /* High-pressure ice. */
    water_phase(350, 1000000000, PHASE_LIQUID);
    water_phase(700, 30000000, PHASE_SUPERCRITICAL);
    water_phase(647.096, 22064000, PHASE_CRITICAL);
    water_phase(647.096, 101325, PHASE_GAS);
    water_phase(700, 22064000, PHASE_FLUID);
    water_phase(230, 1, PHASE_GAS);
    water_phase(230, 100, PHASE_SOLID);
    water_phase(50, 0, PHASE_GAS);
    water_phase(50, 101325, PHASE_SOLID);
    const unsigned solid = 1u << PHASE_SOLID, liquid = 1u << PHASE_LIQUID, gas = 1u << PHASE_GAS;
    MaterialState s = material_state_query(MAT_ICE, 273.16, 611.657);
    CHECK(s.phase == PHASE_COEXISTING && s.phases == (solid | liquid | gas));
    s = material_state_query(MAT_ICE, water_melting_temperature_k(101325), 101325);
    CHECK(s.phase == PHASE_COEXISTING && s.phases == (solid | liquid));
    s = material_state_query(MAT_STEAM, 373.15, water_saturation_pressure_pa(373.15));
    CHECK(s.phase == PHASE_COEXISTING && s.phases == (liquid | gas));
    s = material_state_query(MAT_SNOW, 230, water_sublimation_pressure_pa(230));
    CHECK(s.phase == PHASE_COEXISTING && s.phases == (solid | gas));
    uint16_t water_ids[] = {MAT_WATER, MAT_ICE, MAT_SNOW, MAT_STEAM};
    for (unsigned i = 0; i < 4; i++) {
        s = material_state_query(water_ids[i], 300, 101325);
        CHECK(s.phase == PHASE_LIQUID && s.status == STATE_EQUILIBRIUM);
    }
    CHECK(material_get(MAT_ICE)->reference_phase == PHASE_SOLID);
    CHECK(material_get(MAT_STEAM)->reference_phase == PHASE_GAS);
    CHECK(material_state_query(MAT_SALT_WATER, 300, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_METHANE, 100, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_GRANITE, 2000, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_ELEMENT_HE, .5, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_ELEMENT_C, 4000, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_ELEMENT_AS, 1200, 101325).status == STATE_UNSUPPORTED);
    CHECK(material_state_query(MAT_ELEMENT_OG, 300, 101325).status == STATE_UNSUPPORTED);
    s = material_state_query(MAT_COPPER, 1400, 101325);
    CHECK(s.phase == PHASE_LIQUID && s.status == STATE_REFERENCE_ESTIMATE);
    CHECK(material_state_query(MAT_ELEMENT_CU, 3000, 101325).phase == PHASE_GAS);
    CHECK(material_state_query(MAT_IRON, 1000, 101325).phase == PHASE_SOLID);
    CHECK(material_state_query(MAT_IRON, 1900, 101325).phase == PHASE_LIQUID);
    CHECK(material_state_query(MAT_COPPER, 300, 100000).status == STATE_OUTSIDE_MODEL);
    CHECK(material_state_query(MAT_COPPER, 10000, 101325).phase == PHASE_UNKNOWN);
    CHECK(material_state_query(MAT_WATER, 49, 101325).status == STATE_OUTSIDE_MODEL);
    CHECK(material_state_query(MAT_WATER, 1274, 101325).status == STATE_OUTSIDE_MODEL);
    CHECK(material_state_query(MAT_WATER, 300, 1000000001).status == STATE_OUTSIDE_MODEL);
    CHECK(material_state_query(MAT_COUNT, 300, 101325).status == STATE_INVALID);
    CHECK(material_state_query(MAT_WATER, 0, 101325).status == STATE_INVALID);
    CHECK(material_state_query(MAT_WATER, 300, -1).status == STATE_INVALID);
    CHECK(material_state_query(MAT_WATER, NAN, 101325).status == STATE_INVALID);
    CHECK(material_state_query(MAT_WATER, 300, INFINITY).status == STATE_INVALID);
    CHECK(isnan(water_saturation_pressure_pa(273)) && isnan(water_saturation_pressure_pa(648)));
    CHECK(isnan(water_sublimation_pressure_pa(49)) && isnan(water_sublimation_pressure_pa(274)));
    CHECK(isnan(water_melting_pressure_pa(300, ICE_IH)) &&
          isnan(water_melting_pressure_pa(260, (WaterIce)99)));
    CHECK(isnan(water_melting_temperature_k(500)) && isnan(water_boiling_temperature_k(30000000)));
    puts("PASS: published IAPWS values, pressure-dependent water phases, coexistence/critical states, "
         "element estimates and explicit unsupported/invalid conditions");
}
