#ifndef MATERIAL_STATE_H
#define MATERIAL_STATE_H
#include "material.h"
#define STATE_REFERENCE_PRESSURE_PA 101325.0
#define WATER_TRIPLE_T_K 273.16
#define WATER_TRIPLE_P_PA 611.657
#define WATER_CRITICAL_T_K 647.096
#define WATER_CRITICAL_P_PA 22064000.0
typedef enum {
    STATE_INVALID,
    STATE_UNSUPPORTED,
    STATE_OUTSIDE_MODEL,
    STATE_REFERENCE_ESTIMATE,
    STATE_EQUILIBRIUM
} StateStatus;
typedef enum { STATE_MODEL_NONE, STATE_MODEL_ELEMENT_REFERENCE, STATE_MODEL_WATER_IAPWS } StateModel;
typedef enum { ICE_IH, ICE_III, ICE_V, ICE_VI } WaterIce;
typedef struct {
    PhysicalPhase phase;
    uint16_t phases; /* Bits (1u << PhysicalPhase); no invented coexistence fractions. */
    StateStatus status;
    StateModel model;
    double temperature_k, pressure_pa;
    double melting_k, boiling_k, vapor_pressure_pa; /* NAN when unavailable. */
} MaterialState;
/* Pure H2O correlations, attributed to IAPWS SR1-86(1992) and R14-08(2011).
   Helpers return NAN outside their ranges; see docs/material-states.md. */
double water_saturation_pressure_pa(double temperature_k);
double water_sublimation_pressure_pa(double temperature_k);
double water_melting_pressure_pa(double temperature_k, WaterIce ice);
double water_melting_temperature_k(double pressure_pa);
double water_boiling_temperature_k(double pressure_pa);
/* Read-only query: stored material, mass, temperature, collision and rendering
   stay authoritative. A future energy solver owns transitions. */
MaterialState material_state_query(uint16_t material, double temperature_k, double pressure_pa);
const char *state_status_name(StateStatus status);
const char *state_model_name(StateModel model);
#endif
