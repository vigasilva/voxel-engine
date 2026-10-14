#include "material_state.h"
#include <math.h>
double water_saturation_pressure_pa(double t) {
    if (!isfinite(t) || t < WATER_TRIPLE_T_K || t > WATER_CRITICAL_T_K)
        return NAN;
    if (t == WATER_TRIPLE_T_K)
        return WATER_TRIPLE_P_PA;
    double q = 1 - t / WATER_CRITICAL_T_K;
    double sum = -7.85951783 * q + 1.84408259 * pow(q, 1.5) - 11.7866497 * pow(q, 3) +
                 22.6807411 * pow(q, 3.5) - 15.9618719 * pow(q, 4) + 1.80122502 * pow(q, 7.5);
    return WATER_CRITICAL_P_PA * exp(WATER_CRITICAL_T_K / t * sum);
}
double water_sublimation_pressure_pa(double t) {
    if (!isfinite(t) || t < 50 || t > WATER_TRIPLE_T_K)
        return NAN;
    double q = t / WATER_TRIPLE_T_K;
    double sum = -21.2144006 * pow(q, .00333333333) + 27.3203819 * pow(q, 1.20666667) -
                 6.10598130 * pow(q, 1.70333333);
    return WATER_TRIPLE_P_PA * exp(sum / q);
}
double water_melting_pressure_pa(double t, WaterIce ice) {
    if (!isfinite(t))
        return NAN;
    if (ice == ICE_IH && t >= 251.165 && t <= WATER_TRIPLE_T_K) {
        double l = log(t / WATER_TRIPLE_T_K);
        /* expm1 avoids cancellation close to the normal triple point. */
        return WATER_TRIPLE_P_PA * (1 - 1195393.37 * expm1(3 * l) - 80818.3159 * expm1(25.75 * l) -
                                    3338.26860 * expm1(103.75 * l));
    }
    if (ice == ICE_III && t >= 251.165 && t <= 256.164)
        return 208566000 * (1 + .299948 * expm1(60 * log(t / 251.165)));
    if (ice == ICE_V && t >= 256.164 && t <= 273.31)
        return 350100000 * (1 + 1.18721 * expm1(8 * log(t / 256.164)));
    if (ice == ICE_VI && t >= 273.31 && t <= 355)
        return 632400000 * (1 + 1.07476 * expm1(4.6 * log(t / 273.31)));
    return NAN;
}
double water_melting_temperature_k(double p) {
    if (!isfinite(p) || p < WATER_TRIPLE_P_PA || p > 2216000000.0)
        return NAN;
    if (p == WATER_TRIPLE_P_PA)
        return WATER_TRIPLE_T_K;
    if (p >= 632400000)
        return 273.31 * pow(1 + (p / 632400000 - 1) / 1.07476, 1 / 4.6);
    if (p >= 350100000)
        return 256.164 * pow(1 + (p / 350100000 - 1) / 1.18721, 1. / 8);
    if (p >= 208566000)
        return 251.165 * pow(1 + (p / 208566000 - 1) / .299948, 1. / 60);
    double lo = 251.165, hi = WATER_TRIPLE_T_K;
    for (int i = 0; i < 48; i++) {
        double mid = (lo + hi) * .5;
        if (water_melting_pressure_pa(mid, ICE_IH) > p)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) * .5;
}
double water_boiling_temperature_k(double p) {
    if (!isfinite(p) || p < WATER_TRIPLE_P_PA || p > WATER_CRITICAL_P_PA)
        return NAN;
    if (p == WATER_TRIPLE_P_PA)
        return WATER_TRIPLE_T_K;
    if (p == WATER_CRITICAL_P_PA)
        return WATER_CRITICAL_T_K;
    double lo = WATER_TRIPLE_T_K, hi = WATER_CRITICAL_T_K;
    for (int i = 0; i < 48; i++) {
        double mid = (lo + hi) * .5;
        if (water_saturation_pressure_pa(mid) < p)
            lo = mid;
        else
            hi = mid;
    }
    return (lo + hi) * .5;
}
static bool near(double a, double b) {
    return fabs(a - b) <= fmax(fabs(a), fabs(b)) * 1e-8;
}
static MaterialState phase(MaterialState s, unsigned mask) {
    s.phases = (uint16_t)mask;
    s.phase = PHASE_COEXISTING;
    for (int i = PHASE_SOLID; i <= PHASE_FLUID; i++)
        if (mask == (1u << i))
            s.phase = (PhysicalPhase)i;
    return s;
}
#define SOLID (1u << PHASE_SOLID)
#define LIQUID (1u << PHASE_LIQUID)
#define GAS (1u << PHASE_GAS)
static MaterialState water_state(MaterialState s) {
    double t = s.temperature_k, p = s.pressure_pa;
    s.model = STATE_MODEL_WATER_IAPWS;
    /* Aggregate equilibrium phases, not ice polymorph identity, density,
       latent heat, reactions or metastable/supercooled states. */
    if (t < 50 || t > 1273 || p > 1e9) {
        s.status = STATE_OUTSIDE_MODEL;
        return s;
    }
    s.status = STATE_EQUILIBRIUM;
    s.melting_k = water_melting_temperature_k(p);
    s.boiling_k = water_boiling_temperature_k(p);
    s.vapor_pressure_pa =
        t < WATER_TRIPLE_T_K ? water_sublimation_pressure_pa(t) : water_saturation_pressure_pa(t);
    if (p == 0)
        return phase(s, GAS);
    if (near(t, WATER_TRIPLE_T_K) && near(p, WATER_TRIPLE_P_PA))
        return phase(s, SOLID | LIQUID | GAS);
    if (isfinite(s.melting_k)) {
        if (near(t, s.melting_k))
            return phase(s, SOLID | LIQUID);
        if (t < s.melting_k)
            return phase(s, SOLID);
    }
    if (t < WATER_TRIPLE_T_K) {
        if (near(p, s.vapor_pressure_pa))
            return phase(s, SOLID | GAS);
        return phase(s, p < s.vapor_pressure_pa ? GAS : isfinite(s.melting_k) ? LIQUID : SOLID);
    }
    if (t >= WATER_CRITICAL_T_K) {
        if (near(t, WATER_CRITICAL_T_K) && near(p, WATER_CRITICAL_P_PA))
            return phase(s, 1u << PHASE_CRITICAL);
        if (p < WATER_CRITICAL_P_PA)
            return phase(s, GAS);
        if (t > WATER_CRITICAL_T_K && p > WATER_CRITICAL_P_PA)
            return phase(s, 1u << PHASE_SUPERCRITICAL);
        return phase(s, 1u << PHASE_FLUID); /* Exactly on a critical isobar/isotherm. */
    }
    if (near(p, s.vapor_pressure_pa))
        return phase(s, LIQUID | GAS);
    return phase(s, p < s.vapor_pressure_pa ? GAS : LIQUID);
}
MaterialState material_state_query(uint16_t id, double t, double p) {
    MaterialState s = {.phase = PHASE_UNKNOWN,
                       .status = STATE_INVALID,
                       .model = STATE_MODEL_NONE,
                       .temperature_k = t,
                       .pressure_pa = p,
                       .melting_k = NAN,
                       .boiling_k = NAN,
                       .vapor_pressure_pa = NAN};
    if (id >= MAT_COUNT || !isfinite(t) || !isfinite(p) || t <= 0 || p < 0)
        return s;
    if (id == MAT_WATER || id == MAT_ICE || id == MAT_SNOW || id == MAT_STEAM)
        return water_state(s);
    s.status = STATE_UNSUPPORTED;
    const Material *m = material_get(id);
    if (m->constituent_count != 1)
        return s;
    const Element *e = element_get(m->composition[0].element);
    if (!e || e->phase_predicted || !isfinite(e->melting_k) || !isfinite(e->boiling_k) ||
        e->melting_k >= e->boiling_k || m->composition[0].element == E_HE ||
        m->composition[0].element == E_C || m->composition[0].element == E_AS)
        return s;
    s.model = STATE_MODEL_ELEMENT_REFERENCE;
    if (fabs(p - STATE_REFERENCE_PRESSURE_PA) > 1 || t > 5000) {
        s.status = STATE_OUTSIDE_MODEL;
        return s;
    }
    s.status = STATE_REFERENCE_ESTIMATE;
    s.melting_k = e->melting_k;
    s.boiling_k = e->boiling_k;
    if (near(t, e->melting_k))
        return phase(s, SOLID | LIQUID);
    if (near(t, e->boiling_k))
        return phase(s, LIQUID | GAS);
    return phase(s, t < e->melting_k ? SOLID : t < e->boiling_k ? LIQUID : GAS);
}
const char *state_status_name(StateStatus s) {
    static const char *names[] = {"invalid conditions", "no phase model", "outside model",
                                  "reference estimate", "equilibrium"};
    return s >= STATE_INVALID && s <= STATE_EQUILIBRIUM ? names[s] : "unknown";
}
const char *state_model_name(StateModel model) {
    static const char *names[] = {"none", "element reference", "IAPWS water"};
    return model >= STATE_MODEL_NONE && model <= STATE_MODEL_WATER_IAPWS ? names[model] : "unknown";
}
