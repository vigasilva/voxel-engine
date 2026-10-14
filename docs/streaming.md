This document describes laboratory streaming (`--lab`). See [planetary mode](planet.md) for global editable Earth and spherical LOD.

# Dynamic generation, visibility, and LOD

The default application now starts with a seed and pending chunk descriptors instead of generating the entire millimeter map. It plans geometry around the camera, rejects chunks/tiles outside a conservative view frustum, and builds a bounded number of visible meshes per frame. The default view distance is **512 m**, configurable from **32 to 1024 m**:

```sh
./build/voxel_engine
./build/voxel_engine --view-distance 1024
./build/voxel_engine --full-detail   # Eager 16 x 8 x 16 m map, exact meshes, comparison mode
./build/voxel_engine --coarse        # Original 125 mm map, comparison mode
```

The **editable area remains 16 × 8 × 16 m**. All of its chunks can be refined and edited at 1 mm. Surrounding terrain streams as a **procedural surface preview**; it does not yet have voxel editing, caves, physics, or saved modifications outside that area. The camera can fly beyond the original map, and preview tiles follow it. Extending authoritative editable storage to arbitrary streamed chunks is a separate next step.

## Near geometry and preserved voxel state

Distances are measured to chunk bounding boxes in meters. The editable map uses these render LODs:

| Distance | Render representation |
| --- | --- |
| 0–2 m | Exact region faces, including 1 mm edits and generated 1–4 mm grains |
| 2–12 m | Greedy 125 mm cells |
| 12–32 m | Greedy 250 mm cells |
| 32–96 m | Greedy 500 mm cells |
| Beyond 96 m | Greedy 1 m cells |

The targeted chunk stays exact throughout the **10 m interaction reach**. Picking prepares only pending chunks crossed by the ray, in front-to-back order, and stops once a nearer exact hit occludes subsequent chunks. Switching render LOD does not coarsen stored temperatures or erase edits. Coarse meshes sample the authoritative data, while pending unedited chunks answer procedural samples without allocating fine trees.

A 15% hysteresis band avoids repeated LOD changes at distance thresholds. LOD changes currently switch meshes; there is no continuous geomorph or screen-space error calculation. Small features may disappear at coarser LODs and return when approaching or targeting the chunk.

Unedited procedural fine trees can be regenerated. They unload beyond 20 m or after 120 invisible planning frames. Edited/workshop trees are retained. Loaded V1–V4 worlds retain their stored contents and do not become regenerable by guessing their original terrain; lazy V5 worlds retain their procedural semantics.

## Distant generation and cache limits

Outside the editable footprint, a camera-centered quadtree selects **2–128 m surface tiles**. Each tile generates a fixed **16 × 16** height mesh: horizontal sampling ranges from **125 mm to 8 m** as tile size increases. Large distant tiles never construct millimeter region trees. They sample the same integer-height function as the near terrain; distant colors and geometry are approximations. Downward skirts hide sampling gaps at neighboring LODs and the editable-map boundary.

Tiles subdivide near the camera and wherever necessary to exclude the editable footprint. Desired tiles do not overlap. Global tile origins remain signed 64-bit base-cell addresses; vertex coordinates remain local to each tile and render relative to the camera.

The scheduler builds at most **six meshes per frame**, checking a **4 ms CPU scheduling budget** after each completed job. An individual meshing job can exceed 4 ms because jobs are not preempted. New terrain fills in gradually; old same-origin LOD meshes can remain until replacements finish. Edits receive priority. Generation/meshing is still on the main thread. A minimized window pauses terrain generation and mesh uploads.

The cache holds at most **1024 preview tiles**, plus the 256 editable-map slots. Invisible preview geometry expires after 120 planning frames; replacement uses invisible slots. GPU buffers shrink after large LOD reductions and release after invisibility. Actual Vulkan vertex allocations, including allocation padding and the HUD, are capped at **128 MiB**. Under pressure, invisible buffers are evicted first; exceeding the remaining budget reports failure instead of continuing to allocate. Depth, swapchain, driver, CPU trees, and scratch allocations are additional.

Frustum culling is conservative. Terrain behind hills may still be generated/drawn if its bounding volume intersects the view. GPU occlusion culling, worker meshing, staged device-local uploads, and transition geomorphs remain future work. The general LOD background is covered in NVIDIA's [terrain rendering chapter](https://developer.nvidia.com/gpugems/gpugems2/part-i-geometric-complexity/chapter-2-terrain-rendering-using-gpu-based-geometry); this engine uses CPU chunk/quadtree meshes rather than that chapter's GPU clipmap implementation.

## Saves and generator identity

Lazy worlds use **V5**. The 80-byte header and coarse records retain the earlier layout. Next come a uint32 procedural-generator marker (`1`), a 32-byte pending bitmap, a 32-byte modified bitmap, then the V4 count/workshop/indexed region records. Each chunk must be either regenerable or modified, and every modified chunk must have a valid authoritative region record. Invalid metadata leaves the current world unchanged on load.

Saves persist the seed and **modified chunks**, omitting unedited cache contents. A fresh unedited V5 world is **2,460 bytes**, independent of where the camera has looked. Restoring its seed recreates terrain as needed. Future changes to the procedural generator must introduce a new generator identity/format so old seeds do not silently change terrain. V1–V4 loading and V3/V4 output for nonprocedural worlds remain supported.

## Address range versus map size

`int64_t` counts **125 mm base cells**, not millimeters. Millimeter precision comes from the local chunk/position offsets.

- Cell-address range: **−2⁶³ through 2⁶³−1** on each axis.
- Approximate physical endpoints: **−1.153 × 10¹⁸ m to +1.153 × 10¹⁸ m**.
- Full theoretical span: **2⁶⁴ × 0.125 = 2.306 × 10¹⁸ m**, approximately **244 light-years per axis**.

Accepted map origins leave room for the resident extents and neighbor accesses. Preview tiles also reject origins that cannot fit their extent inside signed range. The address range is not a claim that the engine stores or simulates a universe of that size. The current authoritative editable volume is 16 × 8 × 16 m; preview generation uses a bounded camera working set. Its simple height function is periodic every **2²⁴ base cells = 2,097.152 km** horizontally, so world uniqueness is also distinct from coordinate capacity.

## Measurements and verification

Before dynamic LOD, eager seed-42 generation retained approximately **20.1 MiB** of requested voxel storage and **1.57 million** surface vertices. A settled default-camera 512 m streaming run retained approximately **1.2 MiB** of voxel storage and drew **416,000** vertices while including the surrounding preview. These are scene-dependent storage/geometry measurements, not FPS benchmarks, and exclude allocator/driver overhead. The HUD reports visible entries, queued work, fine chunks, and LOD counts; frame-limited runs print actual mesh allocation bytes.

```sh
ctest --test-dir build --output-on-failure
./build/streaming_tests
python3 tests/window_smoke.py build/voxel_engine
python3 tests/window_smoke.py build/voxel_engine --travel --view-distance 1024
python3 tests/window_smoke.py build/voxel_engine --millimeter \
  --origin-cell 1152921504606846976 -1152921504606846976 18014398509481984
```

CPU tests check lazy versus realized voxel values, procedural/stored seam coverage, detail restoration, edited-data retention, cache-independent V5 saves, corrupt metadata, culling, LOD hysteresis, disjoint tiles, camera travel, maximum view distances, and signed-coordinate endpoints. The travel input test flies beyond the original map at a 1024 m view distance and checks that persisted edits remain unchanged. GPU input checks run serially on isolated X displays with vsync and bounded shutdown; they independently verify exactly 1 mm³ of copper placement, removal, saves, and geometry modes. ASan/UBSan and Vulkan validation are supported.
