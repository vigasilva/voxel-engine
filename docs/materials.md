# Elements and composition foundations

The C core has the full 118-element catalogue, including identities, source atomic masses, reference densities, source melting/boiling points, standard states and group classifications. Data is imported from [PubChem](https://pubchem.ncbi.nlm.nih.gov/rest/pug/periodictable/JSON); see [snapshot provenance and limitations](../data/elements/README.md). Missing measurements stay unknown and predicted states stay marked. Element properties are reference metadata, not a thermodynamics simulation.

Inspect the catalogue offline:

```sh
./build/element_catalog
./build/element_catalog Fe
./build/element_catalog Og
python3 tools/import_elements.py --check
```

`element_get`, `element_by_number`, and `element_find` expose the table. Element enum IDs follow atomic number minus one. Save files continue to store the existing material IDs; the original eleven materials retain their IDs and recipes.

`composition.h` provides a practical basis for geological recipes. `composition_from_atoms` converts stoichiometric ratios into elemental mass fractions using the imported atomic masses. `composition_from_fractions` normalizes supplied fractions and merges duplicate elements. `composition_mix` combines valid compositions using mass weights. Recipes hold up to 16 distinct elements and reject invalid/overflowing values or excessive diversity without replacing the caller's previous result.

```c
AtomRatio quartz_atoms[] = {{E_SI, 1}, {E_O, 2}};
Composition quartz;
if (composition_from_atoms(&quartz, quartz_atoms, 2)) {
    double silica_silicon_fraction = composition_fraction(&quartz, E_SI);
    /* About 46.74% of this SiO2 composition is silicon by mass. */
}
```

`material_composition` extracts normalized fractions from a material recipe. An unmodeled empty recipe, currently air, fails extraction rather than reporting zero mass as a valid mixture. Geological mineral/rock families, elemental placement materials, actual temperature/pressure phase queries, and air chemistry remain work in progress. A physical-state enum includes unknown, solid, liquid, gas, and plasma, but changing a voxel's temperature does not yet melt, evaporate, react, or move it.

These helpers preserve mass accounting, but a mixture of H and O does not establish whether it is water, dissolved gas, or separate phases. Future simulation must retain species/mineral phase identity and track reaction energetics independently from elemental fractions.
