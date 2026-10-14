# Planetary mode

Planetary mode replaces the laboratory's fixed editable box with an implicit globe and globally addressed sparse overrides. It is the default application mode; `--lab` retains the previous sandbox and its `.vxw` saves.

## Physical scale and coordinates

The reference planet has equatorial radius 6,378,137 m, polar radius 6,356,752.314245 m, and mass 5.9722 × 10²⁴ kg. These use the [NASA Earth reference values](https://nssdc.gsfc.nasa.gov/planetary/factsheet/earthfact.html). Geographic coordinates convert to geocentric Cartesian coordinates: X at longitude 0°, Y at longitude 90° east, Z toward the north pole. Elevation samples provide altitude over the ellipsoid, with the approximation described in [data provenance](../data/README.md).

The existing `WorldPosition` remains a signed 64-bit address in 125 mm base cells and a normalized double local millimeter offset. Edit bounds are signed 64-bit integer millimeters. Conversion to double geocentric meters retains sub-millimeter precision at Earth distances. Rendering subtracts integer addresses before converting nearby offsets to floats; vertices stay local to their patch. Selection highlight uses an integer difference between the global millimeter target and mesh origin.

The planet subsystem deliberately accepts positions/edits within ±2 × 10⁹ m on each axis (±2,000,000 km). Spatial query bounds allow a larger halo for ray traversal. This is an implementation working range, not the limit of `int64_t`: the inherited 125 mm address representation theoretically spans about 2.306 × 10¹⁸ m per axis. Much larger planetary positions would require changing geographic queries and spatial indexing rather than merely using that theoretical address capacity.

## Implicit matter and global edits

`planet_base_voxel` returns material and temperature at any millimeter cell center without allocating that cell. The 2160 × 4320 NOAA global fallback, native regional maps, and cached Copernicus fine tiles supply the bed surface. The simplified generator places grass/soil/sand/ice near the surface, stone deeper inside, an iron core below approximately 3,485 km radius, and non-solid water between submarine bed and sea level. Mapped inland water fills an estimated 2 m channel below the local water surface, including locations well above sea level. Atmospheric space is AIR, not a simulated gas.

Every edited 2 m Cartesian chunk contains two existing binary cuboid trees:

1. An override mask distinguishes explicitly changed cells from implicit base terrain, including excavated AIR.
2. A value tree stores material, flags, and temperature for the modified volumes.

Editing splits only intersected cuboids; identical regions coalesce. A 1 mm cube needs a few splits, not an 8-billion-cell chunk allocation. Mask and value trees can have different partitions; their intersections produce visible edit regions. A single brush can cross eight chunk boundaries. Brushes range from 1 mm to 1 m; each brush transaction stages all affected chunks before publishing.

The implicit interior and every surface location use the same editable coordinates. There is no pre-generated dense Earth, no 16 m boundary, and no requirement to retain already visited terrain in RAM. Existing per-voxel composition is through material recipes; arbitrary mixtures and independently editable chemical composition are not implemented.

## Brush shapes and distant navigation

The default brush is a voxelized sphere; B toggles the original axis-aligned cube brush. Sphere diameter uses the existing 1/5/10/25/125/250/1000 mm sizes. A 1 mm sphere addresses one exact voxel. Larger placement positions the sphere along the actual surface normal, and excavation centers it at the contact point. Sphere shape is independent of the player's orientation and latitude; cuboid cells stay on the global integer grid.

Sphere generation classifies bounded regions by their nearest/farthest voxel-center distances to the sphere. Entire inside regions become uniform; outside regions inherit the old mask/value tree. Only partial regions split. Diameters through 25 mm use 1 mm boundary cells. Larger spheres stop partial refinement at `ceil(diameter / 32)` mm (4, 8, or 32 mm for 125/250/1000 mm strokes) to bound CPU and memory. These are voxelized approximate spheres, not perfectly smooth analytic surfaces; subsequent 1 mm strokes retain full local edit precision. The HUD reports sphere diameter and boundary detail. A tested 5 mm stroke contains 81 cells versus a 125-cell bounding cube. The tested 1 m stroke uses 1,693 grouped regions with 0.59% volume error.

F7 aims at the planet's center without teleporting or altering velocity. High-altitude spawns aim at Earth automatically. Space view distance always includes the far side of the planet, even with a small surface view override, and grows beyond the previous 200,000 km automatic cap when necessary. The projection retains its tiny near plane close to the surface and scales it to 0.1% of exterior clearance in space. This prevents distant geometry from quantizing to the cleared depth value on integer depth buffers. Exact nadir views use a stable alternative camera basis in both frustum culling and projection. CPU coverage includes 43 million m and 1 billion m altitudes; the real Vulkan test checks the Earth pixels at 43 million m and recovery with F7 after looking away.

Terrain clipping tests actual polygon/voxel intersection rather than only bounding-box overlap. This avoids fragmenting oblique terrain triangles for edited cells that lie away from the triangle's plane, which otherwise exhausted the clipping budget for round brushes. A temporary balanced bounding-volume index selects neighboring regions for face occlusion and terrain clipping, avoiding a full scan of every edit for every face. The collected regions, BVH candidates/nodes, polygon fragment lists, and mesh vertices grow dynamically without the previous 8,192-region, 128-fragment, or 500,000-vertex caps. Temporary buffers are freed after meshing; fragment buffers are reused between faces.

## Persistence

`earth.vxp` is SQLite, opened with WAL and FULL synchronous transactions. Tables store planet parameters, `(x,y,z)` chunk keys, and compressed payloads. An integer RTree accelerates local visibility/ray queries. A save requires the same reference mass, radii, dataset checksum, and version. Different worlds should use different `--planet-save` files.

Every brush commits automatically. All touched chunk payloads and spatial index entries commit together; a failed write rolls them all back and leaves the live cache unchanged. F5 checkpoints WAL into the main database; it is not required to retain an edit. SQLite can leave `.vxp-wal` and `.vxp-shm` sidecars while a world is open: copy a world after closing the engine, or use a proper SQLite backup. This avoids copying only part of an active save.

Each chunk BLOB begins with `PVED`, version 1, mask/value encoded byte lengths, and a payload checksum (20-byte little-endian header). Two validated `DetailChunk` encodings follow. The mask accepts only canonical inherited/override markers. Material values and temperatures use the existing strict detail decoder. Laboratory `.vxw` files do not import into this geocentric planet automatically.

## Surface meshing and LOD

Six projected cube faces form a spherical quadtree. Each requested surface patch samples a 16 × 16 grid, increased to 128 × 128 below 12 m patch span, 64 × 64 below 80 m span, and 64 × 64 for fine-data patches below 4 km span. Heights combine the improved global NOAA map, three native regional maps, streamed Copernicus 30 m tiles, and larger synthetic relief where fine measured data is absent and small relief over measured land described in [data provenance](../data/README.md). Surface normals interpolate gradients of the terrain samples; physics contact normals sample the same terrain gradient. The planner performs conservative frustum and horizon culling, refines near the camera through level 21, and retains at least level 3 to preserve recognizable coastlines in the whole-Earth view. Planet-level tessellation avoids visible kilometer-sized cubes. Fine terrain triangles interpolate the source height field; they do not add measured detail absent from NOAA data.

Radial skirts cover cracks between tessellation levels. Skirts are a visual approximation rather than watertight topology; triangles need not match across every LOD edge. Millimeter storage resolution and surface tessellation are independent. Near edited regions, base triangles are clipped against explicit volumes, and exposed cuboid/cavity faces are generated directly from the stored partitions. Coplanar region faces use the cuboid storage rather than iterating every millimeter. Unmodified terrain stays a sampled smooth surface; exact voxel cubes appear only where an edit creates such boundaries. Coarse distant rendering suppresses tiny edits while their stored values remain unchanged.

Greedy terrain rectangles merge equal-material cells only if all sampled positions stay within the LOD error: `max(0.25 mm, span × 0.00008)`, capped at 2 km; edited terrain uses 0.25 mm. Straight skirt runs merge similarly. An unmodified near sea-level patch reduces from 101,376 vertices to 30 in the flat fixture. Edit faces merge by plane, material color, and normal; independent temperatures and flags remain in storage. A 4 × 4 × 4 cube with independently varying temperatures renders as six quads (36 vertices), retaining all temperatures.

Edited geometry is visible within roughly 128 m of the camera, prioritizing up to 128 nearby edited chunks. Edit vertex clustering uses 1 mm below 12 m, 4 mm below 32 m, 16 mm below 64 m, and 32 mm farther away. The snapped grid is global, avoiding different snap origins at chunk boundaries. Small distant features may collapse; approaching rebuilds from the unchanged voxel source. Base terrain patches spanning less than 320 m are clipped and remeshed for edits. Nearby base patches are remeshed after edits; queues prioritize edited chunks and closest terrain. Distant mesh caches expire after 120 frames of invisibility. Temporary incomplete coverage during camera teleport/initial loading resolves as the bounded generation queue fills.

## Physics

`G = 6.67430 × 10⁻¹¹ m³ kg⁻¹ s⁻²`, from [NIST CODATA 2022](https://physics.nist.gov/cuu/pdf/wall_2022.pdf). Outside the directional reference ellipsoid radius, acceleration is `−GM r_vector / |r|³`. At equatorial sea level its magnitude is about 9.798337 m/s². Doubling distance divides gravity by four. Planet mass and radius are configurable rather than hidden fixed downward acceleration.

Inside the reference surface, a uniform-density approximation reduces acceleration linearly toward zero at the center. This is not a realistic layered Earth gravity model or the exact field of an oblate mass distribution. The simplified material interior does not determine the configured gravitational mass. Editing matter does not update the planetary mass or gravity field.

Player velocity/position use double precision and velocity Verlet integration at 120 Hz. Inputs request tangent velocity relative to the local ellipsoid normal. Grounded movement sets walking/running velocity; space motion retains inertia. Feet and torso sweeps plus five ground support samples provide collision and grounding against implicit terrain and explicit solid edits. A jump sets upward speed to 5 m/s, giving approximately 1.27 m height near the starting surface. The per-frame timestep is clamped, so overloaded rendering slows simulated time instead of doing an unbounded catch-up loop.

Water is non-solid and has no buoyancy or flow. Editor picking targets both solid and water cells, including the ocean surface; physics raycasts ignore non-solid water. Water edits have visible cuboid faces and retain temperature, but placed water stays where painted without fluid dynamics. Global river ribbons use source centerlines with estimated widths and are clipped to each cube-sphere patch. They are suppressed inside fine satellite-mask coverage. Their exposed triangles use the same edit clipping index as terrain. The static ocean mesh is a visual shell; submerged terrain and underwater edits are not yet a complete underwater renderer. Flight bypasses gravity/collision for inspection. Its camera captures the local orientation frame when enabled and keeps that frame fixed in geocentric space. Translation around the planet does not change the aim or horizon; Space/Ctrl use the captured flight up/down axis. Returning to walking restores local ground alignment and preserves the world-space viewing direction. F7/F2 can still aim at Earth/the Sun explicitly. Air control is simplified. The collision controller is not a full rigid-body/contact solver: high-speed impacts, ceilings, arbitrary structures, and crowded millimeter obstacles need further work. Rotation, centrifugal/Coriolis effects, J2, tides, atmospheric drag, heat, reactions, and falling detached voxels are not implemented.

## Sun and resource visualization

A fullscreen procedural sky draws a visible Sun disk and glow, with altitude-dependent atmosphere fading into space. The Sun direction and angular radius follow the nine-body Solar System state. F2 aims at the Sun. Earth keeps its fixed geocentric orientation; daily rotation, terrain shadow maps, atmospheric scattering and complete day/night simulation are future work.

F1 shows computer resources and a 120-frame timing graph. CPU usage uses process CPU time relative to elapsed time (100% per occupied core); Linux RAM metrics come from `/proc/self/statm` and `/proc/meminfo`. Update time includes simulation, scanning, mesh synchronization, and any fence wait, but excludes HUD construction and submission. Vulkan reports tracked mesh/HUD, uniform and occlusion-proxy buffers, depth and texture allocations, device-local heap capacity, and occluded patch counts. These allocations are not a measurement of total VRAM consumption or GPU utilization.

F4 displays unlit material colors plus a 25 × 25 tangent-plane slice, centered on the aim hit or ground below the player. Page Up/Down changes slice depth from 0 to 4,000 km; -/= zooms through 24 mm, 240 mm, 2.4 m, 24 m, and 240 m widths. Minimum spacing is 1 mm. Samples read `planet_voxel`, including saved modifications and implicit layers; legend percentages refer only to samples. The scan refreshes at 2 Hz and after control changes, with forward at the top. This is a local material inspection tool; ore deposit generation remains future work. Features between sample points can be missed.

## Working-set controls

| Component | Current bound |
| --- | --- |
| Base maps, river index, mapped fine cache | Grows with cached areas; mapping bytes differ from resident RAM |
| Resident edited/negative-query cache | 64 chunk entries |
| Requested resident edit-tree storage | 64 MiB, excluding elevation |
| SQLite page cache | 4 MiB requested; mmap disabled |
| Cached terrain and edit meshes | 768 slots total |
| Surface samples before greedy merging | 16 × 16, medium/fine 64 × 64, closest 128 × 128 |
| Mesh jobs | One background worker/job; synchronous fallback has six jobs / soft 4 ms |
| Planet vertex/HUD Vulkan buffer allocations | No configured allocation cap; available memory applies |
| Edited chunks collected in one query | Streamed without a row cap |
| Explicit regions and clipping fragments | Dynamic allocations; no configured count cap |
| Planet cuboid tree nodes / depth | No configured node cap / 128 structural depth guard |
| Temporary mesh vertices | Dynamic allocation; no configured count cap |
| Brush edge / sphere diameter | At most 1 m; at most eight touched chunks |

Wide or unusually dense visible terrain coarsens if planning exceeds its fixed patch cache. The 64-entry edit cache and requested 64 MiB storage target evict old chunks; a single large active chunk can exceed that target. These are streaming controls, not fatal complexity limits. Normal operation builds meshes on one background thread and uploads completed, current results on the render thread. The synchronous fallback retains a six-job / soft 4 ms allowance, which one complex job can exceed. Total saved edits can grow on disk. Finite allocator/device memory and SQLite's configured maximum record size still apply. Tree depth and input validation remain to protect recursion and save integrity. The laboratory retains its prior allocation limits.

A regression fixture now decodes a planet tree above 65,535 nodes, collects 32,768 regions, and builds 589,824 vertices with independently verified 98,304 mm² surface coverage. Another query streams 300 edited chunks through the 64-entry cache. Fine height fields and river ribbons increase mesh work compared with the old coarse-only view. The cache size changes as areas download. The background worker separately owns its global/regional height arrays and SQLite cache; fine file mappings share operating-system pages. The engine-derived texture allocated about 49 MiB on the tested GPU. These are observations, not guaranteed frame rate or whole-process/VRAM consumption. Rendering remains uncapped by default; `--vsync` lowers render load.

## Code and checks

| File | Responsibility |
| --- | --- |
| `src/planet.c` | Data loading, geography, ellipsoid, gravity, implicit material queries |
| `src/planet_store.c` | Sparse global edits, bounds, SQLite transactions/index/cache |
| `src/planet_physics.c` | Exact cell/region picking, brush bounds, player integration/collision |
| `src/planet_scene.c` | Cube-sphere quadtree, visibility, adaptive LOD, cache planning |
| `src/planet_mesh.c` | Greedy terrain rectangles, skirts, dynamic edit clipping/BVH |
| `src/planet_mesh_merge.c` | Greedy edit-face joining and distance clustering |
| `src/planet_height.c` | Height composition, inland water levels, relief, and normals |
| `src/planet_data.c` | Mapped fine height/water cells, spatial hash, indexed global rivers |
| `src/terrain_stream.c` | Nonblocking download worker, cache refresh, timeout/fallback |
| `src/resources.c` | Process statistics and material slice sampling |
| `src/planet_app.c` | Planet inputs, walking/flight/orbit, frame budgets, autosave |
| `src/renderer.c`, `src/hud.c` | Vulkan upload/draw and planet inspector |
| `tools/fetch_earth.py`, `tools/fetch_earth_regions.py` | Reproducible NOAA global/regional conversion |
| `tools/fetch_earth_detail.py`, `tools/fetch_rivers.py` | Reproducible Copernicus/WorldCover crops and Natural Earth rivers |
| `tests/terrain_stream_tests.c` | Async load/invalidation, cache hits, failed-download fallback, cancellation |
| `tests/planet_tests.c` | CPU geography/edits/persistence/physics/LOD checks |
| `tests/planet_window.py` | Isolated actual Vulkan/input/autosave test |

The CPU suite checks terrain and coordinate seams, millimeter picking on oblique surfaces, edit temperatures, eight-chunk geometry area, cache eviction, reopen, and rollback after an injected second-chunk database failure, mismatched planet parameters, and corrupt chunk rejection. Ocean picking and visible 1 mm water edits are checked independently from solid collision. Physics checks freefall, grounding stability, jump height, and orbital energy preservation over 2,000 fixed steps. LOD coverage exercises polar and longitude seams, ground/orbital altitudes, downward/horizon views, and aspect ratio 4 within the cache limit. All laboratory tests remain available, including old save formats and fine/coarse seams.

The window test uses Xvfb, the X11 SDL backend, a temporary world directory, synchronized presentation, and a shared GPU-test lock. It independently decodes the SQLite value trees to prove actual right/left clicks add/remove exactly 1 mm³ of copper, switches all geometry modes, travels surface → space → surface, restarts the app with its saved edit, launches a gravity-driven orbital trajectory, and independently checks Earth pixels at 43 million m altitude before/after F7 aim recovery. Validation warnings/errors fail the test when the Vulkan validation layer is available.

## Terrain streaming and data upgrades

See [data provenance, accuracy, and attribution](../data/README.md). Optional 0.1-degree fine cache cells combine nominal 30 m height data and 10 m permanent-water classification. Fine measured terrain controls the large-scale shape; small deterministic relief supplements its surface. Satellite water masks take precedence over the coarse river layer. Source heights determine static water levels; channel depth and coarse river widths remain approximations. New fine packs increment a separate surface revision so all affected terrain meshes rebuild, while edit revisions continue to track stored voxel modifications.

The Unix application enables online streaming initially, with F10/`--terrain-offline` controls. It launches one Python/GDAL worker after two seconds in a stable cell, at elevations below 20 km. Existing local coverage permits prefetching a point one kilometer ahead. Requests use cell centers to avoid rounding into an adjacent cell at geographic boundaries. Downloads write a temporary file and rename it atomically; cached CRCs are checked before reuse. The main render thread polls completion, registers mapped data, and invalidates terrain meshes. Failures retain cached/global terrain and delay retries by 120 seconds; a worker is cancelled after 180 seconds or on shutdown. The async regression uses a synthetic subprocess fixture, while real public-source import and inland-water pixels are verified separately.

Version-2 and version-3 saves migrate to terrain version 4 without changing the compressed voxel payloads. The base global/rivers recipe remains bound to its checksum; optional fine cells can be added independently. Refined implicit terrain can expose or bury existing absolute edits. A walking player embedded by a newly loaded height field is lifted to the new ground; flight preserves position and orientation.

## Background meshes, distant texture and occlusion

The SDL meshing worker owns an independent `Planet`, SQLite connection, mapped cache metadata, and edit cache. It accepts one job at a time, rebuilds its source after edit revisions, refreshes fine data for surface revisions, and publishes a moved `Mesh`. The renderer accepts the mesh only when the slot key, edit LOD, and both revisions still match. Mesh upload and all Vulkan operations remain on the render thread. Shutdown joins the worker before releasing terrain resources. A synchronous fallback remains for thread initialization failure and in-memory worlds.

`surface_texture.c` bakes the implicit height/material rules into a zlib-compressed PTEX version-1 atlas. Header fields are magic, version, width, height, payload CRC32, compressed length, terrain checksum and reserved zero (32 little-endian bytes). RG stores 16-bit elevation mapped over −12,000..+12,000 m, B stores a material ID, and A stores inland-water coverage. The shader interpolates height before applying an antialiased sea-level threshold. That separates sharp coastlines from distant mesh tessellation and material-cell boundaries. Above 50 km altitude, the sky pass also intersects the complete reference ellipsoid and shades it with the same engine atlas. This fills distant streaming gaps without waiting for hundreds of mesh uploads; detailed meshes overlay it. The backdrop uses the reference ellipsoid and approximates relief at the silhouette. Palette values come from `material_get`; no imagery is used. The base atlas does not encode individual millimeter edits. Rebuilding refreshes it after data/source changes.

Visible mesh triangles are partitioned into solid and water ranges once during upload. The passes draw near to far, reducing overdraw and pipeline changes. A Vulkan occlusion query pool tests conservative boxes derived from actual uploaded vertex bounds against scene depth, with no colour or depth writes. Previous completed results are read without a query wait. Zero-sample queries suppress the corresponding mesh draws only for unchanged camera coordinates, projection and contributing geometry. Camera-inside proxies stay visible. Edits, source changes, slot replacement, replacement meshes and disappearing occluders invalidate old results; newly added occluders can only hide more surfaces. Query boxes continue to draw for hidden patches so they can become visible again. F3 geometry modes disable occlusion; O toggles it explicitly. This intentionally forgoes stale-query culling during camera movement.

`solar.c` contains barycentric Newtonian interactions with a Sun-centered test-particle 1PN correction and RK4. `solar_tests.c` compares an integrated isolated Mercury orbit with the Newtonian control, obtaining the expected extra perihelion advance, and checks year-scale orbital stability and clock/velocity effects. This is not the full Einstein-Infeld-Hoffmann many-body model. `tests/solar_window.py` checks a known foreground/background edit scene, pixel equality with culling disabled, actual skipped patches, planet selection and time controls. `tests/planet_worker_tests.c` checks independent connection refresh, bounded queue and shutdown. See the [README](../README.md) for solar controls and model limits.

## River projection and planetary texture view

Each terrain request carries a quantized power-of-two meters-per-pixel river LOD derived from camera clearance, conservative patch distance, the vertical field of view and actual drawable height. The planner omits river ribbons below 1.5 pixels width. Maximum mapped width permits rejecting all river candidate collection before allocating or scanning the river index. Surviving segments use 50 m..5 km subdivisions scaled to pixel size. Their stored hydrology and editable water volume remain independent of this rendering decision. River LOD is part of worker result identity and cached mesh readiness, so changing distance rebuilds representation appropriately.

When all rivers become subpixel, old detailed terrain buffers are retired immediately. Results for invisible patches are discarded before any GPU allocation or upload. At large distances where the maximum 12 km reference relief is below one physical screen pixel, solid view switches to the analytic textured ellipsoid and requests no terrain patches at all. This avoids both river and terrain mesh generation/upload/drawing for the overview; F3 restores geometric inspection. Approaching restores terrain requests and reconstructs meshes from the retained height and voxel sources. The texture backdrop writes reverse-Z reference depth, preventing submerged terrain skirts from appearing over the planet during streaming.
