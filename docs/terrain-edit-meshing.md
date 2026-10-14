# Local terrain refinement around voxel edits

A collected edit used to force every cell of its nearby terrain patch to use a 0.25 mm merging error and bypass the normal terrain LOD. One millimeter edit could therefore add hundreds of thousands of unrelated triangles. The base terrain now keeps its ordinary greedy partition and refines the area around an edit instead.

The mesher projects edited boxes onto the patch's cube face using their geodetic positions on the reference ellipsoid. Removing height before that projection matters: at Earth elevation, projecting an elevated point directly can shift its terrain footprint by several meters. A small guard band retains detailed ground beside clipping edges.

Refinement uses a hierarchy of rectangular cells. Long greedy strips split along their longer axis first; nearly square cells split into four children. Height samples and the edit index decide which cells need more detail. Tiny edits retain a sub-millimeter sampled height tolerance; larger edits use a tolerance bounded by both the terrain LOD and the edited box size. Fully overridden fine surfaces are omitted. Rendering never rewrites the stored integer voxel addresses, materials or temperatures.

Dyadic coordinates identify shared edge points exactly. All leaf edges are collected and matched before triangulation, so a fine neighbor's boundary points also appear on its coarse neighbor. Skirts follow those same outer segments and are clipped against edits. Unrelated edits whose projected footprints lie outside the patch keep the normal terrain path. Coarse biome colour textures remain usable around edits; separately rendered voxel walls retain their actual materials.

## Verified geometry cost

These are complete CPU mesh fixtures with the same terrain data and camera, comparing the previous mesher with the new one. The complete-view tests build **every visible patch**, not just the first streaming jobs.

| Fixture | Previous vertices | Current vertices | Current triangles |
| --- | ---: | ---: | ---: |
| 65 mm cut in a 194.6 m patch | 26,127 | 2,535 | 845 |
| Complete ground view, 1 mm spherical cut, 528 patches | 2,042,001 | 165,423 | 55,141 |
| Complete ground view, 1 m spherical cut, 531 patches | 2,129,313 | 188,661 | 62,887 |

The latter two counts include both base terrain and edited-volume meshes. The regression retains a 300,000-vertex guard for these ground views. This is a test expectation, not a runtime complexity cap or a guarantee for arbitrary volumes of independently painted matter.

Independent triangle-ray checks locate the cut at its physical position and compare surviving nearby ground with the implicit height field. The native survey fixture's observed neighboring errors were below 0.005 mm; that does not imply the survey itself has millimeter accuracy. A floating edit also exercises mixed-resolution topology and matching skirts, with 698 paired edges and zero unpaired surface edges. Tests cover 1 mm, 65 mm and 1 m cuts, stored matter/temperature preservation, and multiple terrain spans. A cut on a patch boundary also verifies that no remaining terrain/skirt intersection lies inside the removed volume.

The release suite, focused AddressSanitizer/UndefinedBehaviorSanitizer checks and the NVIDIA Vulkan editing/input/persistence/water/orbit checks passed. Vulkan synchronization validation was enabled. These results measure geometry and correctness, not desktop FPS.

## Remaining work

Excavation walls still follow the stored cuboid regions. Smoothed normals and locally refined terrain do not implement a density-volume mesher or reconstruct an analytic sphere from the older adaptive cuboid boundary. A future volume mesher must improve that shape while keeping saved material data and physics consistent. Cold views with no loaded parent still need bootstrap coverage; more detailed terrain providers and planetary body frames remain separate development work.
