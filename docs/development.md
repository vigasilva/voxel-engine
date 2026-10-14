# Active development and implementation audit

This tracks the reality-simulator proof of concept's next development pass. Completed changes are tested, committed, and pushed individually to the public repository. A feature being present does not establish complete physical accuracy.

## Audit of the previous implementation notes

| Claim | What the implementation currently does | Remaining work |
| --- | --- | --- |
| Mesh-backed terrain and 1 mm voxels | Unmodified terrain is an implicit height field rendered as meshes. Modified volumes are stored in sparse voxel trees and their boundaries are rendered as meshes. | Natural excavation still has cuboid boundaries; normal smoothing is not a density-volume mesher. Improve the visible edit surface while preserving material values. |
| Greedy meshing | Merges terrain rectangles within an error tolerance and equal-material edit faces. Shared terrain edges and skirts retain matching vertices. Terrain sampling/merging now use quantized screen-space error. | Further localize fine clipping around edits and control total rendering cost. |
| Dynamic LOD | A spherical quadtree selects visible patches; distant Earth uses an engine-generated texture and analytic ellipsoid. Invisible meshes now remain cached by slot/memory pressure, with reusable slots protected before new requests. | Keep coarse parent coverage until different-detail replacement meshes are ready; slot replacement can still cause gaps. |
| Occlusion culling | Vulkan bounding-box queries skip hidden patches when the camera and contributing geometry are unchanged. | Moving the camera invalidates results conservatively. Validate interactions with retained replacement coverage and expose its cost. |
| Detailed Earth data | NOAA global/regional relief, sparse Copernicus GLO-30 heights, WorldCover water masks, and Natural Earth river centerlines. | Coverage is sparse and source heights include surface objects. Add finer regional providers and retain source resolution/provenance. |
| Procedural terrain | Deterministic small-scale noise supplements measured heights; fallback areas receive larger noise octaves. | This is not yet a biome-aware landscape generator or a model of mountain erosion. |
| Distant textures | Bakes engine heights, material IDs, and inland-water classification; no photographic Earth imagery. | Current material classification is too simple for a realistic global biome appearance. |
| Water | Editable non-solid material and a dedicated optical surface shader. | Native water height follows the sampled DSM and can rise along slopes; beds use an abrupt estimated depth. Implement connected level surfaces and gradual banks/beds. No fluid solver exists. |
| Material composition | Eleven material IDs and illustrative elemental fractions; temperature is stored per voxel. | Only eight elements exist. Add all 118 elements, common geological recipes, and explicit physical states. Thermal transfer and chemistry remain future simulation work. |
| Solar System and relativity | Nine mutually gravitating bodies, approximate J2000 initial conditions, special-relativistic helpers, and leading weak-field Schwarzschild corrections. | Not a full Einstein field solver. Other planets are analytic renderings, not editable terrain worlds. |
| Resource visualizers | CPU/RAM/tracked Vulkan allocations, frame history, material slices, patch queues, and occlusion counts. | Material slices are samples, not an ore-deposit generator; driver-wide GPU usage is not measured. |

## Work checklist

- [x] Reproduce and fix sky cracks inside greedy terrain patches; add topology regression and matching skirt boundaries.
- [x] Make all graphics test suites share the same GPU lock.
- [x] Reduce unmodified terrain triangle cost using projected detail and measured error (default ground fixture: about 2.3 million to 51,000 triangles).
- [ ] Further localize fine terrain around edits and validate total cost with dense excavation.
- [ ] Keep coarse coverage until finer replacements finish; cache useful meshes through camera turns.
- [x] Retain hidden mesh buffers through camera turns, remove frame timeout eviction, and expose retained memory in F1.
- [x] Reuse completed stationary patch selections instead of repeatedly sampling/replanning the quadtree; invalidate on camera, projection, source, edit, or world-shape changes.
- [ ] Validate occlusion with the revised rendering/cache lifecycle.
- [ ] Flatten connected water surfaces and provide gradual riverbeds and shore transitions.
- [ ] Add an inspectable procedural biome/landscape generation tool and use its rules over measured terrain.
- [ ] Improve mountainous fallback terrain without claiming synthetic relief is surveyed geography.
- [ ] Represent world material/biome states in the engine-derived space view.
- [ ] Add the periodic table, extensible compositions, geological material families, and physical state queries.
- [ ] Improve the mesh appearance of natural voxel excavations and verify saved voxel values remain intact.
- [ ] Research and integrate finer public terrain providers with on-demand sampling, attribution, and explicit coverage.
- [ ] Research and import the best available terrain/shape data for other planets and major moons, with explicit coverage and accuracy.
- [ ] Add the major planetary moons and their orbital interactions.
- [ ] Implement axial rotation and separate body-local voxel grids that move and rotate with each celestial body while retaining edit coordinates.
- [ ] Complete CPU, sanitizer, and focused Vulkan checks for the final behavior and update the implementation notes.

## Earth data direction

The engine must select sources by coverage, sampling resolution, vertical datum, and whether they measure bare ground or a surface including vegetation/buildings. Fine data is fetched locally; a complete Earth raster at the highest regional resolution is not allocated in memory or bundled in Git.

| Source | Intended role | Integration status |
| --- | --- | --- |
| [NOAA ETOPO](https://www.ncei.noaa.gov/products/etopo-global-relief-model) | Global relief and bathymetry fallback | Included, with decimated global and native regional samples. |
| [Copernicus GLO-30](https://registry.opendata.aws/copernicus-dem/) | Broad 30 m surface-height coverage | Existing on-demand importer. |
| [ESA WorldCover](https://esa-worldcover.org/en/about/about) | Land-cover classes and permanent water | Existing importer retains only its water classification; full class support is pending. |
| [USGS 3DEP](https://www.usgs.gov/faqs/what-coverage-3d-elevation-program-3dep-dems) | Finer US terrain where products are available | Provider selection/import pending; coverage must be queried. |
| [PGC polar elevation products](https://www.pgc.umn.edu/guides/stereo-derived-elevation-models/pgc-dem-products-arcticdem-rema-and-earthdem/) | Finer Arctic/Antarctic surface models | Access, datum, coverage, and product-specific terms must be checked before enabling a provider. |
| [IUPAC periodic table](https://iupac.org/what-we-do/periodic-table-of-elements/) | Element identity and atomic-weight provenance | Material-system expansion pending. |

Additional regional surveys can be imported through a common georeferenced raster path. Providers requiring account access or restricted datasets must remain explicitly configured. There is no single uniformly detailed survey of all Earth features; the engine must show source quality honestly and distinguish procedural detail from measurements.
