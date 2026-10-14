This document describes laboratory mode (`--lab`). See [planetary architecture](planet.md) for the current default.

# Foundations and simulation direction

## Cell state and grouped storage

The resident world is 128 × 64 × 128 cells, partitioned into 256 independently stored chunks. Each chunk covers 16³ cells. `VOXEL_SIZE_MM = 125` is the authoritative integer physical scale. The physical map is still 16 × 8 × 16 meters. All 256 editable chunks support integer-millimeter region trees; the default builds them lazily, with detailed generation and cross-chunk edits described in [millimeter.md](millimeter.md).

Logical voxel values remain eight bytes:

```c
typedef struct {
    uint16_t material;      // Stable registry ID; air is 0.
    uint16_t flags;         // Reserved; current formats require zero.
    float temperature_k;   // Independent cell state.
} Voxel;
```

In the optional `--coarse` mode, a chunk root represents its entire 2-meter cube with one value when all cells match. Mixed regions split into eight children, recursively covering 8³, 4³, and 2³ cells. At size 2, a small brick stores eight voxel values directly, avoiding a node allocation per individual cell. Uniform parent widths are 2000, 1000, 500, 250, and 125 mm.

Values match only when material, flags, and temperature bits are equal. Matching material alone is insufficient for volume compression: that would destroy distinct temperatures. An edit expands only the path to its cell. Matching children coalesce after edits and import. This is lossless grouping of fixed-resolution cells, not distance-based level of detail. Different cells retain independent state through the accessors.

`world_storage_bytes` reports the `World` structure and its requested tree allocations, excluding allocator bookkeeping and mesh/UI/save buffers. An empty/uniform world needs only the inline chunk roots and metadata (about 6.8 KiB on this platform). A single different cell adds 448 bytes. The coarse seed-42 terrain uses approximately 0.95 MiB versus the former 8,388,608-byte dense array. In the worst case of entirely different cells, tree/brick metadata can cost more than dense storage; compression is content-dependent.

The coarse generator builds one dense chunk scratch buffer at a time and compresses it directly. The fine generator builds balanced binary region trees on demand (or eagerly with `--full-detail`), sampling heights on 62/63 mm footprints and adding 1–4 mm grains. Its eager seed-42 map uses 20.1 MiB of requested voxel/world storage; the default lazy working set is smaller. There is no retained full-world dense copy. V1 imports use a temporary old-format payload, then build grouped chunks. Coarse temperature-only edits affect storage but do not mark visual meshes dirty because the shaders currently do not display temperature.

The immutable material registry supplies elemental mass fractions, density, heat capacity, and conductivity. Individual composition overrides are not implemented. A full cell has volume `s³` and estimated mass `density * s³`, with `s = 0.125 m`. Porosity and partial fill still need separate state. Creative placement/removal creates/deletes matter. Air is an empty-space sentinel, not simulated gas.

## Coordinates and millimeter precision

Global cell addresses, map origins, and ray-hit addresses use `int64_t`. Small chunk indices and local tree indices use ordinary integers. A position never becomes a large absolute float:

```c
typedef struct {
    CellCoord cell;       // Three signed 64-bit global cell addresses.
    DVec3 offset_mm;      // Double-precision local offsets, normalized into [0, 125).
} WorldPosition;
```

Movement adds millimeters to the local offset and carries/borrows whole cells with checked integer arithmetic. Taking the difference between nearby global addresses happens in integer space first; only the small relative result converts to floating point. This preserves 1 mm and sub-millimeter positioning at large addresses where even a double cannot distinguish adjacent global cells. Refined chunks use those positions to pick and edit actual 1 mm cells.

Meshes store floats local to their own 16-cell chunk. For each draw, the CPU subtracts the camera's integer cell address from the chunk origin, then includes the camera's millimeter offset. The resulting small camera-relative offset goes to the shader. The view matrix contains orientation only. Target cells are converted to world-local and then chunk-local addresses before reaching GPU floats. The near/far clip planes and fog remain physically scaled.

The resident map can be translated with `--origin-cell X Y Z`; bounds checks and saved origins retain all 64 bits. Invalid origins near signed integer endpoints are rejected to leave room for the map and a one-cell neighbor halo. F9 preserves the camera's position relative to the newly loaded map.

## Greedy coarse surface meshing

For each dirty coarse chunk, the mesher reads material IDs into an 18³ cache: 16³ cells plus a one-cell halo. Uniform empty chunks return immediately. Each axis, orientation, and slice builds a 16² mask of exposed faces. A face is present when its cell is solid and its direct neighbor is empty.

The mesher takes the next nonempty mask entry, extends a run across equal materials, then extends its height while every entry matches. It emits one rectangle and clears the covered mask entries. Faces merge only when they are coplanar, face the same direction, and share a material. Merging stops at chunk boundaries. Neighboring chunk edits still invalidate seam meshes.

Each rectangle emits six non-indexed vertices with chunk-local positions, a normal, and material color. Per-cell color variation and voxel grid lines are procedural shading, so color noise does not prevent rectangles from merging. Grass uses its appropriate top/side color. Picking, removal, placement, and target highlighting still address individual cells inside a merged face. Temperature stays independent in storage.

For seed 42, exposed surface coverage is unchanged at 51,800 unit faces, but they merge into 8,810 rectangles: 52,860 vertices versus 310,800 before. Coverage tests check every unit face, absence of duplicate/missing surfaces, material boundaries, and outward winding. This is a greedy partition, not a guaranteed minimum-polygon solution. Greedy rectangles can produce T-junctions; no explicit T-junction repair is implemented.

F3 cycles solid, triangle overlay, and wireframe. Barycentric coordinates from `gl_VertexIndex % 3` reveal the triangles of the merged rectangles. Wireframe discards triangle interiors; overlay keeps fill. The HUD stays filled. The voxel grid in solid mode is a shader effect, not the mesh edge pattern. Buried faces are not generated.

The algorithm background is described in the original [greedy meshing article](https://0fps.net/2012/06/30/meshing-in-a-minecraft-game/). Hierarchical volume storage is a separate concept; see [NVIDIA's sparse voxel octree research](https://research.nvidia.com/publication/2010-02_efficient-sparse-voxel-octrees). This engine uses its own small CPU chunk trees, not that paper's full rendering architecture.

Refined chunks walk stored solid regions and query their neighboring planes for empty patches, including exact fine/fine seams. Sparse compatible rectangles then merge by material, plane, and orientation. Coarse boundaries adjoining it bypass center-sampled face masks and use the same exact plane queries. No finest-cell sweep is used in the refined chunk. See [the millimeter mesher](millimeter.md#surface-meshing-and-seams).

## Vulkan rendering

The renderer uses one graphics/presentation queue and one frame in flight. It waits for the previous render fence before changing/freeing mapped mesh and HUD buffers. Acquire semaphores are reused after their submissions finish; presentation semaphores are indexed by swapchain image. Swapchain recreation waits for device idle and rebuilds color/depth resources, pipelines, and framebuffers.

Push constants occupy 128 bytes: view-projection matrix, coarse and 1 mm chunk-local targets, camera-relative chunk offset, and viewport/geometry/fog settings. Mesh buffers use host-visible coherent memory. Camera visibility, distance-based LOD, and bounded procedural preview streaming are now implemented; see [streaming.md](streaming.md). There are no staged device-local uploads, index buffers, or worker meshing yet. The core library does not depend on SDL/Vulkan.

Presentation is uncapped by default: `IMMEDIATE`, then `MAILBOX`, otherwise `FIFO`. `--vsync` explicitly requests `FIFO`. Only a minimized window sleeps; fence waits remain necessary for resource safety. The build/run instructions use Release compilation; sanitizer checks use Debug.

Depth uses a reversed-Z projection (near maps to 1, far to 0), clear depth 0, and GREATER testing. A 0.1 mm near plane permits close-up inspection; the far plane follows the configured view distance with a 128 m tile margin. D32 floating-point depth is preferred. Fog depends on camera-relative view distance rather than the depth encoding. See [NVIDIA’s depth-precision explanation](https://developer.nvidia.com/blog/visualizing-depth-precision/).

## Persistence versions 3 and 4

New saves encode coarse cube trees and indexed millimeter region trees, with an 80-byte header and depth-first records. Multiple refined chunks use V4; zero/one refined chunk uses V3. The header retains the V2 layout. All integer fields and binary32 temperature bits are little-endian. No C struct padding is written.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | `VXW3` or `VXW4` magic |
| 4 | 4 | Version (3 or 4) |
| 8 / 16 / 24 | 8 each | Unsigned 64-bit resident dimensions (128 / 64 / 128) |
| 32 | 4 | Generation seed |
| 36 | 4 | Cell width in millimeters (125) |
| 40 / 48 / 56 | 8 each | Signed 64-bit global origin X / Y / Z |
| 64 | 8 | Payload byte length |
| 72 | 4 | FNV-1a checksum |
| 76 | 4 | Reserved; must be zero |
| 80 onward | Variable | Coarse cube records followed by indexed millimeter region records |

Chunk order is X fastest, then Z, then Y. A uniform cube is tag byte `0` plus an eight-byte voxel value: 16-bit material, 16-bit flags, binary32 temperature. A split cube is tag `1` followed by eight children. Child index bits are X (bit 0), Y (bit 1), Z (bit 2); each bit selects the low/high half of its axis. Size-1 cubes cannot split. The checksum covers header bytes 0–71, header bytes 76–79, and the payload in that order, excluding its own four-byte field.

After the coarse records, V3 appends a little-endian 32-bit detail chunk index (`0xFFFFFFFF` for none). If present, the chunk’s coarse record must be a uniform air placeholder and the following region tree is authoritative. Region leaf tag 0 carries the same eight-byte voxel value. Branch tags 1/2/3 select X/Y/Z and carry a 16-bit integer-mm split coordinate, followed by low/high children. Root bounds are `[0, 2000)` on each axis. Bounds are reconstructed from validated split planes; see [refined persistence](millimeter.md#saving-and-limits).

V4 replaces the single detail index with a uint32 count and workshop index, then repeats uint32 chunk index + region tree for each refined chunk. Duplicate/out-of-range indices, invalid coarse placeholders, and missing workshop references are rejected.

The loader bounds payload size, limits recursion to the chunk depth, verifies tags/length/checksum, validates values and origin arithmetic, and builds a temporary world before committing. Redundant equal children coalesce. Failed loads leave the current world intact. The eager detailed seed-42 save is approximately 3.8 MiB; the earlier coarse save is about 0.72 MiB.

Both V1 grids remain supported: 128 × 64 × 128 at 125 mm, and 64 × 32 × 64 at 250 mm. V1 has a zero global origin. Old 250 mm cells map to 2 × 2 × 2 children with the same material/temperature, preserving mass. Import compresses their data; saving uses V3 or V4 according to the number of refined chunks. V2 cube-tree saves also load and have no refined chunk. The original V1 payload-only checksum and 32-byte header remain honored for backward compatibility.

Saving writes a sibling `.tmp` and renames it after a successful close. This is replacement safety on Linux, not guaranteed power-loss durability. Camera position/selection are not saved. Checksums detect accidental corruption and are not a security mechanism. Existing material IDs remain stable.

## Coordinate and map limits

The resident region is still exactly **128 × 64 × 128 cells**, or **16 × 8 × 16 meters**. Large global addresses do not allocate or generate a larger map. Procedural surface preview tiles now stream around the camera; arbitrary streamed editable chunks remain future work.

Signed 64-bit cell addresses span −2⁶³ through 2⁶³−1. Actual accepted map origins leave room for resident extents and neighbors. The important rendering improvement is relative arithmetic, rather than merely increasing the integer range. Nearby positions remain precise independent of their global address; far-away differences converted to double/float still have the usual floating-point limits, so those differences must remain local.

A uniform region can be cheap, but worst-case detail still costs memory. A dense 1 km cube at 125 mm would contain 512 billion cells and require about 4.1 TB just for eight-byte values. Hierarchical grouping avoids that cost for uniform regions, but cannot make arbitrary independent data free. The full resident map now uses adaptive 1 mm regions and boundary traversal. The default adds lazy generation, preview streaming, and distance-based render LOD; iterating every finest-resolution cell would defeat the compression benefit.

## Next simulation steps

1. Add collision, gravity, inventory, and tools that transfer matter instead of erasing it.
2. Introduce a fixed-step, energy-conserving thermal solver. Keep simulation activity separate from mesh dirty flags. Temperature differences must remain independently stored even if surfaces merge.
3. Add mass/fill fraction, chemical species, phase and latent heat, then conservative fluid/granular transport.
4. Implement containers, furnaces, molds, and work surfaces so heating/pouring/shaping happens in the world.
5. Extend streaming to authoritative editable chunks, mesh on workers, add staged uploads and GPU occlusion culling, and reduce edit-copy allocation without dense finest-cell iteration.

Default lazy-world persistence uses V5 generator metadata and modified region records; the details and 64-bit physical span are documented in [streaming.md](streaming.md).
