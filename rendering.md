# Rendering data flow and ray traversal

## CPU-to-GPU region data

The server generates a region SVO and sends one complete `RegionSnapshot` in
one length-prefixed TCP frame. The client does not retain the SVO. It keeps
only the coordinate and revision map used to avoid requesting unchanged
regions.

For a newly received or revised region, the client creates a transient packed
upload record:

```text
RenderState::Region
  coordinate: uint16 x, y, z
  revision: uint32
  packedNodes: [childMask, firstChild, materialAndFlags] * nodeCount
```

`packedNodes` exists only until the next renderer upload. It is not a CPU-side
world representation. After `VulkanContext::uploadWorld` runs,
`RenderState::regions` is cleared.

The persistent representation is the mapped Vulkan storage buffer:

```text
word 0                         resident region count
word 1 .. 512                  64 region headers, 8 words each
remaining words                64 fixed-size SVO node slots
```

Each region header contains:

```text
header + 0: region X coordinate
header + 1: region Y coordinate
header + 2: region Z coordinate
header + 3: absolute node-base word offset
header + 4: node count
header + 5: region revision
header + 6..7: reserved
```

Each node occupies three words. `firstChild` is relative to the region's node
array, so the shader adds `nodeBase` exactly once. Child blocks contain eight
contiguous nodes. The renderer assigns a stable slot to each resident region,
updates only changed slots, and removes slots when the client emits an eviction
record. The arena is fixed-size and never grows during rendering.

## Ray traversal

For every pixel, the compute shader constructs a camera ray from the player
eye position and view direction. It advances through the resident region
volumes, not through every voxel:

1. Convert the current ray point to a cell coordinate.
2. Reject coordinates outside the finite world
   `[0, worldSize) × [0, worldSize) × [0, worldDepth)`.
3. Compute the region coordinate `(cell / 128)`.
4. Search the bounded resident region header table.
5. Intersect the ray with that region's 128-cell world-space box. Coordinates
   outside the finite world `[0, 32768) × [0, 32768) × [0, 8192)` are rejected.
6. Traverse the region SVO with a fixed 64-entry near-ordered stack.
7. Use child masks to skip empty children.
8. Stop at terminal nodes, or at conservative summary nodes when their
   projected size is below the LOD threshold.
9. Return the nearest hit normal and material.
10. If no resident region is hit, advance to the exact far face of the current
    region and continue up to the bounded ray distance. Missing regions are
    treated as unknown space rather than as solid or as a fixed-size distance
    skip, preventing geometry from being skipped at region boundaries.

Every node index is checked against the region node count before dereferencing.
The traversal has a fixed visit budget and the ray has a fixed segment budget,
which prevents malformed data or a sparse scene from creating an unbounded
Metal/Vulkan command.

## Important invariants

- Region headers use absolute GPU word offsets.
- SVO child indices remain region-local until `nodeBase` is added.
- Region coordinates are indices, while ray boxes use world-cell coordinates.
- X, Y, and Z are all bounded; no axis wraps.
- Missing regions are unknown and produce no hit; they are not fabricated as
  solid terrain.
- CPU staging payloads are transient; GPU slots are the authoritative render
  state.
