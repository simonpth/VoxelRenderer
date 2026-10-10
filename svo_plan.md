# Sparse Voxel World: Decisions and Implementation Plan

## Status and goal

This document is the implementation contract for replacing the current dense
`32^3` world. The goal is the highest practical efficiency while preserving
support for 2022-era desktop GPUs and macOS through MoltenVK.

The migration is deliberately staged. Each stage must build and be testable;
the old dense fixture may remain temporarily as a comparison oracle, but it
must not remain the production world representation after the GPU stage.

## Locked decisions

### World model

- One voxel is one world unit.
- World dimensions are X=`32768`, Y=`32768`, Z=`8192`.
- X, Y, and Z are finite and bounded. X/Y are `[0, 32768)` and Z is
  `[0, 8192)`.
- A lookup outside Z is empty for rendering and solid for authoritative
  collision at the lower boundary; the upper boundary is empty/outside.
- The server is the only world authority.
- Terrain is generated deterministically from a server seed, but the client
  receives streamed occupied SVO regions and does not regenerate authoritative
  terrain.
- There is one server process for now.
- Persistence is out of scope for this migration.

### SVO and regions

- The world is partitioned into `1024 x 1024 x 1024` voxel regions. There
  are `32 x 32 x 8 = 8192` addressable regions. A region is a depth-10 local
  octree and is addressed by bounded X/Y/Z coordinates.
- A region root is empty, homogeneous, or subdivided. Only occupied and
  explicitly resident nodes are serialized.
- CPU node storage uses packed child masks and contiguous eight-child blocks;
  no pointers are serialized or sent over the network.
- The payload is a material ID. Color and metadata come from a shared
  material table, so repeated voxels do not duplicate RGBA data.
- Edits support single voxels, box/fill/erase/brush operations, and procedural
  bulk generation. They are applied as bounded region transactions.
- Server collision descends to voxel leaves. A region needed for collision is
  regenerated on demand from the seed if it is not resident.

### Streaming and transport

- The default interest radius is five regions in X/Y and the minimum set is
  the current region plus a one-region safety ring. It is configurable.
- The client starts empty, displays a missing-data/coarse placeholder, and
  becomes interactive immediately. Missing client data is never interpreted as
  authoritative empty space.
- TCP is used for reliable region snapshots, fragmentation, acknowledgements,
  and resynchronization. UDP continues to carry input and player snapshots.
- UDP snapshots contain a world epoch and region revision acknowledgements,
  not voxel data.
- Region updates are revisioned and checksummed. Deltas are applied only when
  their base revision matches; otherwise the client requests a TCP snapshot.
- Initial target budgets are 8 MiB/s per client and 100 ms update latency.
  The server uses a token bucket and prioritizes spawn/current-camera regions.

### Rendering and LOD

- The camera can render the finite 32768-unit horizontal domain, but each ray
  has a finite configurable distance limit and is clipped at world bounds.
- LOD uses projected node size with hysteresis. The default target is one to
  four pixels per terminal node, with a configurable minimum node size.
- Coarse occupied nodes are conservative solid blocks and use average material
  color from the node summary. This prevents holes at the cost of possible
  over-occlusion, which is preferable for correctness.
- CPU and GPU rendering use camera-relative coordinates. Authoritative cell
  coordinates remain integers; float conversion occurs only after subtracting
  the camera origin.

### Hardware baseline

- Baseline is Vulkan 1.3 core functionality supported by MoltenVK on current
  macOS and typical 2022 GPUs.
- Do not require descriptor indexing, buffer-device-address, Vulkan ray
  tracing, mesh shaders, sparse residency, or vendor extensions.
- GPU data uses ordinary storage buffers, a bounded region table, and
  per-frame/fence-protected uploads. A fixed maximum resident-region count is
  negotiated from device memory and configured conservatively.
- The renderer remains a compute shader plus fullscreen pass. SVO traversal
  is implemented in Slang/SPIR-V, not hardware ray tracing.

## Why the dimensions are feasible

The dense volume contains `2^43 = 8,796,093,022,208` voxels. At one byte per
voxel this is about 8 TiB; the current two-word packed representation would be
about 64 TiB. This is impossible for a normal server, client, or GPU.

The requested dimensions are feasible because only occupied regions and the
client's interest set are resident. The region count is large but addressable:
32 X regions, 32 Y regions, and 8 Z regions. Empty regions occupy only
metadata. A terrain surface stores substantially fewer occupied nodes than a
dense volume, and region eviction bounds client/GPU memory.

The major feasibility risks are not coordinate width; they are worst-case
occupancy, edit fragmentation, bandwidth, and shader traversal work. Every
implementation phase therefore records node count, bytes, region count,
upload rate, and traversal visits.

## Component architecture

```text
                    +--------------------+
                    |  Server World       |
                    |  seed + region SVOs |
                    +----------+---------+
                               |
              reliable TCP     | snapshots/deltas
                               v
 +-------------+       +------+-------+       +----------------+
 | UDP input   +------>+ Client cache +------>+ GPU region SSBO |
 | and players |       | revisions    |       | node/payload    |
 +------+------+       +------+-------+       +--------+-------+
        ^                     |                         |
        | UDP snapshots       | immutable render view   | compute
        |                     v                         v
        |              +------+-------+       +----------------+
        +--------------+ Render state +------>+ SVO ray/LOD    |
                       +--------------+       +----------------+
```

The server's mutable SVO and client/GPU snapshots are separate ownership
domains. The render thread never reads a socket or mutable server structure.
GPU buffers are replaced or updated only after the frame fence that may read
them has completed.

## Data structures

### Shared coordinate and material types

Use fixed-width integer types:

```text
WorldDimensions { uint32 x=32768, y=32768, z=8192 }
VoxelCoord      { uint32 x, y, z }       // bounded cell
WorldAddress    { int64 x, int64 y, int32 z } // bounded API input
RegionCoord     { uint16 x, y, z }
MaterialId      uint16
```

All coordinates are validated against their axis bounds. Negative and
out-of-range X/Y addresses are rejected; no axis wraps.

### Packed node

The canonical node layout is:

```text
uint childMask       // bit i: child i exists
uint firstChild      // base index of an eight-node child block
uint payloadIndex    // material/summary index for terminal nodes
uint flags           // terminal, homogeneous, summary-valid, resident
```

Child blocks are contiguous. A missing child is represented by a clear bit,
so traversal uses `firstChild + childIndex`; no pointer chasing or GPU
allocation is required. A terminal homogeneous node carries one material.
Splitting copies its material to children, then edits the requested path.
After edits, equal terminal children are merged back into their parent.

Payloads are material IDs and optional node summaries. The material table is
small, immutable per world epoch, and uploaded once per client/device.

### Region ownership

`World` owns a map from `RegionCoord` to mutable `RegionSvo`. A region has:

- seed/generation identity;
- monotonic region revision;
- packed nodes and payloads;
- dirty bounding box and edit sequence;
- resident/loaded state;
- checksum of the serialized representation.

Regions are regenerated on demand from the seed, then retained according to
the server memory budget. The server may discard clean far regions; dirty
regions must be serialized or retained until their revision is durable in the
active client set.

## Server and simulation plan

1. Implement integer coordinate wrapping and `SvoRegion::get/set`.
2. Implement deterministic region generation from `(worldSeed, regionCoord)`.
3. Replace authority collision's linear array lookup with `World::isSolid`.
4. Use swept axis-separated collision as today, but query wrapped X/Y cells
   and exact SVO leaves. A missing region is synchronously regenerated.
5. Add edit transactions. Validate all affected Z cells and cap operation
   volume per tick. Apply the full transaction, then publish one region
   revision.
6. Add per-client interest sets and a token bucket. Region order is:
   current region, safety ring, near regions, then far regions.
7. Keep movement/player snapshots on UDP. Include world epoch and an
   acknowledgement watermark only.
8. Add TCP region snapshot, delta, resync, and disconnect handling. TCP
   framing uses a fixed header, payload length limit, sequence number,
   region coordinate, base revision, new revision, and checksum.

No client-provided position, voxel, revision, or region is trusted. All
operations are range checked, size capped, and rejected with an explicit
protocol error.

## Client and renderer plan

1. Add a background reliable-world connection separate from the UDP receive
   loop.
2. Store regions in a thread-safe cache with states:
   `Requested`, `Loading`, `Resident`, `Stale`, `Evicting`.
3. Publish immutable region snapshots to the render thread. Do not copy the
   complete world into every `RenderState`.
4. Maintain a byte and region budget. Evict far regions only after their GPU
   uploads are no longer referenced by in-flight frames.
5. Upload packed region nodes/payloads through staging buffers, with transfer
   to shader-read barriers and frame-fence ownership.
6. Use a bounded region table in a storage buffer. The table contains region
   coordinate, local root bounds, node/payload offsets/counts, revision, and
   residency flags.
7. Missing data renders a distinct coarse placeholder/debug color and is
   treated as unknown for interaction. It is never silently treated as empty.

## Shader traversal

The compute shader will:

1. Rebase the ray origin around the camera before converting to float.
2. Intersect the ray with the finite X/Y/Z world bounds.
3. Locate resident regions from the bounded region table.
4. Traverse the root using ray-box intersections and a small fixed traversal
   stack. Children are visited in increasing entry distance.
5. Stop at a terminal node, an LOD screen-space threshold, or a traversal
   budget. Coarse nodes use their summary material and conservative bounds.
6. Compute the hit normal from the entered node face.
7. Trace shadows with the same traversal and a shorter configurable budget.
8. Count invalid indices, stack overflow, missing regions, and visit budget
   exhaustion in a debug SSBO when enabled.

The shader never trusts node indices or counts supplied by the CPU. Every
access is bounds checked against the region table and buffer counts. It must
handle zero ray components without NaNs and use finite iteration limits.

## Vulkan implementation constraints

The descriptor set remains ordinary fixed bindings:

```text
0 storage image
1 sampled image
2 storage buffer: region table
3 storage buffer: packed nodes
4 storage buffer: material/payload table
5 storage buffer: optional debug counters
```

The implementation must work without descriptor indexing, so a region table
and arena are bounded. If the configured resident set exceeds the device
budget, the client evicts or reduces detail; it does not allocate unbounded
GPU memory. Device limits are queried at startup and logged.

Use host-visible staging for updates and fence-protected arena generations.
Avoid writing a mapped GPU buffer while a submitted frame can still read it.
Push constants contain only camera/LOD/ray parameters and buffer counts.

## Validation

### CPU correctness

- Compare random SVO lookup against a dense reference for small regions.
- Test split, clear, merge, fill, erase, brush, and procedural edits.
- Test negative and out-of-range X/Y addresses are rejected.
- Test Z `-1`, `0`, `8191`, and `8192`.
- Test region serialization, checksums, revision gaps, duplicates, and
  malformed lengths.
- Compare SVO collision and brute-force collision on representative scenes.

### GPU and network performance

Record:

- bytes and nodes per occupied voxel and per resident region;
- edit and regeneration latency;
- server/client memory and eviction rate;
- join time and bytes/sec under the 8 MiB/s budget;
- GPU upload time and resident-region count;
- ray node visits, missing-region count, stack overflow, and frame time;
- shadow traversal cost and LOD transitions.

Keep a small dense fixture specifically for regression comparison. Add debug
visualization for region residency, selected depth, unknown data, and wrapped
coordinates.

## Delivery phases

1. **CPU core:** coordinates, materials, packed SVO region, lookup/edit/merge,
   deterministic generation, and tests.
2. **Authority:** exact SVO collision and server-side region regeneration.
3. **Protocol:** TCP framing, snapshots, deltas, checksums, acknowledgements,
   resync, interest management, and bandwidth throttling.
4. **Client cache:** immutable snapshots, unknown-data behavior, budgets, and
   eviction.
5. **GPU storage:** bounded region table, SSBOs, staging, barriers, and
   material table.
6. **Ray traversal:** hierarchical primary and shadow traversal.
7. **LOD:** projected-size thresholds, hysteresis, conservative summaries,
   and visual/debug validation.
8. **Removal and scale test:** remove dense production paths, retain fixtures,
   stress target dimensions and adverse occupancy patterns.

Each phase must pass the existing build plus targeted CPU/network tests before
the next phase begins. The first implementation change starts phase 1 and
phase 2 while preserving a compatibility dense snapshot until phase 4.
