# Temperature and pressure state queries

`material_state.h` adds a read-only physical-state query with temperature in kelvin and **absolute pressure in pascals**. It distinguishes supported equilibrium correlations, reference estimates, missing models, conditions outside a model, and invalid inputs. Unknown results have no phase mask; unavailable boundary values are `NAN`.

```c
MaterialState state = material_state_query(MAT_WATER, 360, 50000);
/* Gas: water boils at a lower temperature at this pressure. */
MaterialState compressed = material_state_query(MAT_WATER, 298.15, 1e9);
/* Solid: the high-pressure melting curve matters, even at room temperature. */
```

The laboratory and planet inspectors query the hit voxel's stored temperature at an explicitly **assumed 1 atm**, displaying the state and its model/status. This is a diagnostic, not a measurement of local pressure. The application has no coupled atmosphere/hydrostatic pressure field yet. Querying a state does not allocate meshes or run over every terrain cell.

## Pure water

Water, ice, snow and steam presets share the pure-H2O model. Salt water, dissolved mixtures and other species require their own models. Correlations are attributed to the International Association for the Properties of Water and Steam:

- [IAPWS SR1-86(1992)](https://iapws.org/technical-guidance/release/Supp-sat): vapor–liquid saturation pressure.
- [IAPWS R14-08(2011)](https://iapws.org/documents/release/MeltSub): sublimation and melting boundaries.

The aggregate-state query supports 50–1273 K and 0–1 GPa. Zero pressure uses the vapor equilibrium limit. Below the triple point, sublimation replaces boiling where liquid is unstable. Pressure-dependent melting uses the ice Ih, III, V and VI curves. The query reports solid, liquid, gas, coexistence, supercritical or critical fluid; exactly on a critical isobar/isotherm it uses the generic fluid label. It does not resolve solid polymorph identity, metastability, nucleation, dissociation or plasma.

At a coexistence boundary, `state.phases` contains multiple bits and `state.phase` is `PHASE_COEXISTING`. Temperature and pressure alone do not determine the amounts of each phase. A relative tolerance of 1e-8 identifies numerical boundary equality; that tolerance is not a claim of physical accuracy.

The helper functions expose saturation, sublimation, melting and inverse melting/boiling boundaries. Saturation is limited to 273.16–647.096 K, sublimation to 50–273.16 K; the four melting helpers enforce their individual ranges. The inverse melting helper extends through the ice VI curve to 2.216 GPa, while the aggregate-state query deliberately stops at 1 GPa. Values outside a helper's range stay unknown. Near a rounded junction, the published coefficients and endpoint pressures can differ slightly; source uncertainty is much larger than numerical precision.

The CPU regression checks published verification values, pressure sweeps, atmospheric boiling, pressure-depressed freezing, high-pressure ice, coexistence, critical states and invalid/unsupported conditions. The equations predict equilibrium boundaries, not density, heat capacity or latent heat at arbitrary conditions.

## Elements and mixtures

Single-element presets with usable melting/boiling reference values have a **reference estimate** near 101325 Pa (within 1 Pa), up to 5000 K. This uses the existing [PubChem snapshot](../data/elements/README.md); it is not a pressure-dependent equation of state. Missing or predicted states, helium, carbon and arsenic are excluded from this shortcut. Their special-pressure melting or sublimation cannot be interpreted as an ordinary atmospheric melting point. Unsupported pressures/temperatures remain unknown instead of extrapolating these thresholds.

Elemental mass fractions do not identify chemical species, mineral phases or mixtures' equilibrium boundaries. Rock, soil, air, salts and the other molecular volatile presets therefore keep explicit unsupported results until suitable models are supplied.

## Inspect and extend

```sh
./build/material_catalog "Water" 360 50000
./build/material_catalog "Water" 298.15 1000000000
./build/material_catalog "Copper" 1400 101325
./build/material_catalog "Granite" 2000 101325
ctest --test-dir build -R material_equilibrium_states --output-on-failure
python3 tests/planet_window.py build/voxel_engine --state-only
```

The focused graphics check requires Tesseract alongside the existing Xvfb/xdotool/ImageMagick tools. It reads the rendered inspector, places/removes exact millimeter copper and water, independently decodes the saved matter, and checks Vulkan validation in an isolated temporary world.

State queries keep stored material IDs, temperature, mass recipes and collision authoritative. They do not transform a voxel merely because an equilibrium state differs from its preset. A physical transition must conserve elements and energy, account for latent heat and density/volume changes, and update collision, neighboring matter and rendering together. Heat transfer, chemistry and fluid flow remain separate unfinished simulation work; preset density/transport fields remain reference properties.
