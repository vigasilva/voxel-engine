# Millimeter terrain and editing

Run `./build/voxel_engine --full-detail` for the eager detailed **16 × 8 × 16 m** map. All **256 chunks** support authoritative integer-millimeter region storage, including air and underground chunks. The default application now generates fine data lazily and uses camera-dependent meshes; see [dynamic generation and LOD](streaming.md). Use `--coarse` for the earlier 125 mm generator. Loading a save preserves its existing contents and resolution; it does not regenerate terrain or erase edits.

Left click removes material, right click fills, middle click picks material, and [ / ] selects **1, 5, 10, 25, or 125 mm** brushes. The default is a 1 mm cube everywhere in the detailed map. F3 cycles solid, triangle overlay, and wireframe. The inspector reports the hit chunk, local integer-mm cell, material composition, temperature, and mass in milligrams.

## Detailed generation

`src/terrain.c` generates smooth seeded hills, a sandy strip, grass/soil layers, clay pockets, stone, and iron/copper deposits. Each 2 m chunk has a 32 × 32 horizontal sampling grid. Its integer column footprints alternate **62 and 63 mm**; terrain heights are rounded to **1 mm**, with small procedural surface relief. Sparse **1–4 mm** stone/copper grains refine the surface further. The same global map-local column positions are used in vertically adjacent chunks.

This is adaptive millimeter storage and editing, with finer features in the generated terrain. The height field is horizontally sampled at 62/63 mm; the generator does not calculate an independent terrain value at every possible 1 mm location. Changing that sampling density would increase geometry and memory substantially.

The generator builds balanced region trees directly, collapses equal leaves, and adds surface grains to unpublished trees. It commits the whole map only after successful generation. It neither allocates nor sweeps a dense millimeter array. Seeds are deterministic on this implementation, and rebasing the world preserves the generated local terrain.

Earlier eager Release seed-42 results on the development machine (retained for comparison; see streaming.md for current dynamic behavior):

| Measurement | Result |
| --- | --- |
| Logical 1 mm cells | 2,048,000,000,000 |
| Requested voxel/world storage | 21,076,792 bytes / 20.1 MiB |
| Region nodes | 658,320 |
| V4 save | 3,954,104 bytes / 3.8 MiB |
| Surface quads | 260,938 |
| Surface vertices | 1,565,628 |
| CPU generation | Approximately 50–60 ms |
| CPU meshing of all chunks | Approximately 260–340 ms |

Storage excludes allocator bookkeeping, meshes, Vulkan allocations, UI, and save buffers. The vertex payload alone is about 53.8 MiB. Timings are CPU measurements, not an FPS benchmark. A dense eight-byte value per logical millimeter cell would require 16.384 TB. Uniform storage is cheap; arbitrary distinct material/temperature data is still expensive.

## Integer boundaries and grouped regions

Global coordinates remain signed 64-bit **125 mm base-cell addresses** with small local offsets. Chunk detail coordinates are integers in `[0, 2000)` mm. Huge absolute global addresses are never multiplied into millimeter floats; the engine first subtracts nearby integer addresses.

Each chunk can own an axis-aligned binary spatial partition. A leaf represents every logical 1 mm cell in its cuboid with one complete eight-byte voxel value. Branches split at exact integer-mm planes. Equality includes material, flags, and temperature bits. Painting refines only overlapping volumes and merges equal siblings. A uniform 2 m cube represents **8 billion logical cells** with one 32-byte node and a 16-byte descriptor on this platform.

Refined chunks have uniform-air coarse placeholders. `world_get` remains a compatibility accessor that samples the center of a 125 mm cell; exact inspection uses the ray hit's chunk and millimeter address. `world_set` paints the entire 125 mm base cell. `world_paint_chunk_mm` paints a box relative to a specified chunk and supports crossing into neighbors. The legacy `world_paint_mm` wrapper addresses the active workshop/legacy chunk.

## Surface meshing and seams

The mesher visits solid region faces and queries neighboring region planes for exposed rectangles. Solid neighbors hide internal faces regardless of their material or temperature. At chunk boundaries, fine neighbors receive exact mm plane queries; coarse neighbors receive clipped 125 mm queries. Coarse meshes beside fine chunks also use exact region queries, so a one-millimeter hole is visible even when the coarse cell's center is solid.

Exposed patches are grouped by plane, orientation, and material. Sorting and repeated horizontal/vertical passes merge compatible adjacent rectangles before emitting two triangles per quad. This reduced the initial full-map output from about 1.94 million to 1.57 million vertices. Merging does not alter voxel properties. There is no dense 1 mm face-mask sweep. A uniform 2 m cube surrounded by air produces six quads / 36 vertices.

The rectangle partition is not guaranteed optimal. T-junctions remain possible; global edge stitching is not implemented. Unit tests independently check fine surface coverage, outward winding, mixed-resolution cavities, and fine/fine seams.

## Picking and brushes

World ray traversal visits only chunks crossed by the ray, queries each encountered refined tree once, and returns the nearest coarse or fine surface. A hit includes the chunk index and exact adjacent 1 mm cell. Camera-relative rendering, fine highlighting, and reversed-Z depth with a 0.1 mm near plane support close viewing, including at global addresses beyond 2⁶⁰.

Removal extends inward; filling extends outward. Tangential dimensions center on the selected cell. A brush can cross faces, edges, and corners into adjacent chunks. Coarse neighbors are promoted without losing their values when needed. All affected trees are built in temporary copies and committed together; allocation, node-budget, or out-of-map failures leave them unchanged. Interior edits dirty only their own mesh; touching a chunk face also dirties the neighbor. Ordinary 125 mm-or-smaller brushes affect at most eight chunks. World bounds reject brushes extending beyond the resident map. Placement avoids filling the camera's position.

Edits are creative mode: removal deletes matter, and placement creates matter at 293.15 K. Tools, inventory, conservation, chemistry, and evolving temperature remain future work. A 1 mm iron cell has volume `1e-9 m³` and mass **7.874 mg**. Integer volume accounting checks exact one-cell changes, including independently heated grains.

## Workshop

`--millimeter` or F4 focuses a 125 mm iron workpiece with a 10 mm copper inlay, 1 mm stripe, and 1 mm groove. It occupies an otherwise wholly empty chunk, preferred `(4, 2, 4)`. F4 returns to the terrain view. Existing workshop data is preserved across focusing and saves. If no empty chunk is available, creation fails without replacing terrain.

Workshop navigation is 60 mm/s, Shift 300 mm/s, Alt 5 mm/s. Outside the workshop, normal flight speeds remain. F5/F9 save/load all chunks and the workshop marker. Loading a coarse-only save exits workshop navigation.

## Saving and limits

Worlds with multiple refined chunks use **V4**. Its 80-byte header and coarse cube records retain the V2/V3 layouts. The payload then contains a uint32 refined-chunk count, uint32 workshop index (`0xFFFFFFFF` for none), and repeated uint32 chunk indices followed by region trees. Chunk records must be unique, in range, and have uniform-air coarse placeholders. The workshop must reference a present refined chunk.

A region leaf is tag 0 plus an eight-byte voxel value. Branch tags 1/2/3 select X/Y/Z, followed by a little-endian uint16 split and low/high children. Bounds reconstruct from the 2000 mm root. The checksum covers metadata and payload. Loads validate records and build a temporary world before committing. Zero/one-refined-chunk worlds retain V3 output for compatibility; both V1 resolutions, V2, and V3 still load.

Each refined chunk is limited to **65,535 nodes** and **128 branch levels**. Editing clones only affected chunk trees, temporarily increasing memory. The default now adds lazy chunk generation, preview streaming, distance-based geometry LOD, and frustum culling. Worker meshing and device-local staged uploads remain future work. V5 procedural saves persist only the seed and edited chunks; V3/V4 retain stored-world compatibility. See [streaming details](streaming.md).

## Verification

```sh
ctest --test-dir build --output-on-failure
./build/full_map_tests
python3 tests/window_smoke.py build/voxel_engine
python3 tests/window_smoke.py build/voxel_engine --millimeter
python3 tests/window_smoke.py build/voxel_engine --coarse
python3 tests/window_smoke.py build/voxel_engine \
  --origin-cell 1152921504606846976 -1152921504606846976 18014398509481984
```

The full-map tests check all chunks, total logical volume, generated 1 mm grains, seed determinism, exact seam geometry, brush transactions across eight chunks, downward picking throughout the map, V4 byte-identical round trips, workshop preservation, and duplicate-record rejection. The independent input test decodes saved region volumes, verifies exactly 1 mm³ of copper was added, removes it to restore the save, and exercises geometry modes and persistence. ASan/UBSan tests and GPU validation runs are also supported.
