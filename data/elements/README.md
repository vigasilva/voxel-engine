# Element reference data

`pubchem.json` is an offline snapshot of the [PubChem periodic-table JSON endpoint](https://pubchem.ncbi.nlm.nih.gov/rest/pug/periodictable/JSON), verified against the source on 2026-10-14. It includes all 118 elements. [IUPAC's periodic-table page](https://iupac.org/what-we-do/periodic-table-of-elements/) is the identity/atomic-weight reference; this imported numeric snapshot is PubChem's data and is not advertised as a replacement for current IUPAC standard atomic-weight intervals.

SHA256: `ff0f75976583b8a6493b18d0b42e83e1b68bd6e112b45d33578d98494ed5d321`.

`tools/import_elements.py` generates `src/element_ids.h` and `src/elements.inc`. A normal build needs no network. `python3 tools/import_elements.py --check` verifies the generated files; `--download` explicitly refreshes the snapshot before regeneration.

The engine copies source atomic masses in atomic mass units, melting/boiling entries in kelvin, and converts source density from g/cm³ to kg/m³. Missing values become `NAN`; unknown properties are not invented. Source standard states marked “Expected” retain a predicted flag, including superheavy elements. Reference density conditions and phase-point conditions vary across the source entries. These reference fields are not an equation of state or a guarantee of properties at any requested temperature/pressure.

Atomic IDs equal atomic number minus one. Saved voxels store stable material IDs, not these element enum values. The pre-existing material IDs 0–10 and voxel/save payloads remain unchanged. Elemental mass fractions do not describe molecular bonds, isotope inventories, crystal structures, allotropes, or reaction products.
