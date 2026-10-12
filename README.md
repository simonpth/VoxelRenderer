# VoxelRenderer

A server-authoritative sparse voxel octree renderer. The server owns the world
and the tick timeline; the client forwards input, receives state, and renders.
Runs on one process with real loopback TCP/UDP sockets.

```text
                 +---------------------------+
                 |  Server                   |
                 |  world SVO + authorities  |
                 +------+-------------+------+
              UDP      |             |  TCP
          intent/snap  |             |  brick stream
                       v             v
                 +---------------------------+
                 |  Client                   |
                 |  uploads to GPU arena     |
                 |  interpolates, renders    |
                 +---------------------------+
```

## Building and running

```sh
cmake -S . -B build -DVOXEL_BUILD_TESTS=ON
cmake --build build -j8
ctest --test-dir build --output-on-failure
./build/VoxelRenderer
```

Shaders are compiled at build time by `slangc`; touching `src/shaders/*.slang`
and rebuilding recompiles them.

### Options

```text
--port   <n>      UDP gameplay port (world stream uses port + 1)
--host   <ip>     server address
--world  X Y Z    world size in cells (default 1024 1024 256)
--brick  <n>      power-of-two edge of one octree brick (default 128)
--seed   <n>      terrain seed
--radius <n>      brick streaming radius (default 6)
--pool   <n>      GPU node pool capacity in nodes (default 4M)
--ray    <n>      maximum primary ray distance in cells
```

## The authority boundary

Everything that decides *where a player is* happens on the server:

- `simulation/authority.*` is the only code that integrates velocity, gravity,
  jumps, and swept-AABB collision against the world.
- The client sends `InputIntent` over UDP and never a position, a velocity, or
  a collision result.
- The server never accepts a client transform. It keeps the newest valid
  intent per client, steps at a fixed 60 Hz, and publishes tick-numbered
  snapshots.
- The client interpolates between the two newest snapshots purely for display.

Mouse look is the one deliberate exception: it is applied locally so the
camera responds within a frame. The authoritative orientation still comes from
the server and is what other players see.

Collision is exact against the octree. The server never generates a whole
dense volume: it regenerates the one brick a query misses.

## World layout

The world is a finite, non-cubic box of cells `sizeX x sizeY x sizeZ`
(default `1024 x 1024 x 256`, Z up). It is addressed with integer cell
coordinates in `[0, sizeX) x [0, sizeY) x [0, sizeZ)`.

### The non-cubic root layer

An octree needs a power of two, but a 1024 x 1024 x 256 world is not a cube.
Two levels handle this:

- **Root layer**: an N-ary node whose dimensions are
  `rootDimX x rootDimY x rootDimZ = ceil(size / brick)`. For the default world
  that is `8 x 8 x 2 = 128` children. This layer is what makes a non-cubic
  world possible, and each dimension is adjustable independently.
- **Bricks**: below the root every child is a plain octree covering
  `brick^3` cells (a power-of-two cube), which is what the GPU expects.

A root lookup is one division per axis and no hashing: the root layer is a
dense array indexed directly.

### The node

Eight bytes, trivially copyable, identical on the CPU and on the GPU. The
layout is mirrored by `src/shaders/svo.slang`.

```text
word0  u32   bits  0..23  firstChild   index of the first stored child
                24..31  flags
word1  u16   bits  0..15  material     16-bit material id
        u8   bits 16..23  mask         bit i => child i is stored
        u8   bits 24..31  lodLevel     log2 of this node's edge in cells
```

```cpp
struct alignas(8) SvoNode {
    std::uint32_t word0 = 0;
    std::uint16_t material = 0;
    std::uint8_t mask = 0;
    std::uint8_t lodLevel = 0;
};
```

`firstChild` is an absolute index into the shared node pool, so a node's
children live at `firstChild + i`. Index 0 is reserved, which lets
`firstChild == 0` mean "no children" without spending a flag bit.

### Why children are contiguous

A node's children are stored contiguously and in mask order, so child `i`
lives at `firstChild + popcount(mask & ((1 << i) - 1))`. For surface-like data
most boxes have a single occupied child, so this costs `popcount(mask)` slots
instead of a fixed eight. On the default world that is the difference between
41 MiB and about 23 MiB of nodes.

## CPU-side storage

- `Brick` holds one flat `std::vector<SvoNode>`. A node with children owns a
  contiguous run of eight child slots, so GPU traversal is one indexed load
  and the CPU allocator is a free list of eight-node blocks. A reused block is
  zeroed on allocation, so a child's default state is "empty".
- `World` is a dense `std::vector<Brick>` over the whole root layer. The array
  costs one small struct per brick even when empty, which is the right
  trade-off for direct indexing; a sparse map would be the upgrade path for
  very large worlds.
- Generation splits top-down and uses a min/max **height mipmap** over the
  brick footprint, so its "is this box uniform" test is O(1) instead of a
  per-cell scan. The full 1024 x 1024 x 256 world generates in about 105 ms and
  compacts in about 11 ms, for 2.9M nodes (22 MiB, 178 KB per brick).

The merge step only collapses a node whose eight children are *identical
leaves*. Two subtrees that merely look identical in their summary fields are
never merged, because a node's summary does not describe its children.

### Materials

Every voxel stores a 16-bit material id. Colour and behaviour live in the
material table, which the **server** defines and streams once per connection
in the world descriptor. The client uploads it to a GPU buffer and the shader
indexes it.

| id | name   | colour        | flags             |
|----|--------|---------------|-------------------|
| 0  | air    | -             | -                 |
| 1  | stone  | grey          | solid             |
| 2  | dirt   | brown         | solid             |
| 3  | grass  | green         | solid             |
| 4  | sand   | tan           | solid             |
| 5  | lava   | orange        | solid, emissive   |

## GPU storage

The client uploads bricks and keeps **no CPU-side voxel world**.

- **Root table**: one `uint2` per root-layer brick, holding that brick's root
  node plus a `resident` flag. Direct index, no search.
- **Node pool**: every node of every resident brick in one storage buffer.
- **Material table**: `uint2` per material, `.x` packed RGBA8, `.y` flags.

Node layout and child addressing are identical to the CPU, so a brick upload
is a `memcpy` of the compacted array plus one `firstChild += poolBase` pass.

The pool is a **coalescing first-fit free list** over a fixed capacity. A
brick takes exactly the run it needs, so the arena never fragments into
slivers and eviction returns the run whole. Capacity is bounded at startup
(`--pool`, default 4M nodes = 32 MiB). If the resident set will not fit, the
brick is dropped and reported rather than growing the allocation.

Only the **dirty span** of each buffer is flushed, aligned out to the device's
`nonCoherentAtomSize`. Both in-flight frame fences are waited on before any
host write, so a frame still reading the pool is never overwritten.

### Traversal

`shaders/compute.slang` walks the root layer brick by brick and descends each
brick with a fixed 56-entry stack (eight children per level, seven levels
under a 128-cell brick). Children are pushed in descending entry order and
popped nearest-first, so the nearest hit wins.

A ray that stops exactly on a lattice plane with a tiny direction component
along that plane's normal produces a degenerate, zero-length slab overlap, and
a fixed distance epsilon is then below float32 precision at these
coordinates. The walk therefore advances the *cell coordinate* as well, which
always lands in a genuinely different voxel.

The shadow ray is a separate call for the same reason SPIR-V has no
recursion. It only needs "did anything block this", so it stops at the first
terminal solid node and does no shading.

Every pool load is bounds-checked against `nodeCount` and the iteration count
is capped, so malformed data cannot hang the GPU.

### Feature scope

MoltenVK-compatible on purpose: plain storage buffers with fixed bindings, no
buffer device address, no descriptor indexing, no ray tracing, no mesh
shaders. Core 1.3 dynamic rendering and synchronization2 only.

LOD is deliberately out of scope for now; the traversal is exact. The node's
`lodLevel` field is already there for when projected-size culling is added.

## Networking

| Channel | Direction | Payload |
|---------|-----------|---------|
| UDP     | C -> S     | registration, `InputIntent` |
| UDP     | S -> C     | `AuthoritativeState` snapshots |
| TCP     | C -> S     | `WorldInterest`, `WorldAck` |
| TCP     | S -> C     | `WorldDescriptor`, `BrickSnapshot` |

One TCP connection per client, full duplex, polled on both sides, each served
by its own thread. A handshake (`WorldInterest`) establishes the stream; the
server then replies with the world descriptor and one brick snapshot per
frame, nearest first, in bounded batches. The client acknowledges each brick so
the server stops resending it and skips revisions it already holds.

The listener reaps only stream threads whose connection has already finished.
Joining a *live* one there would make a second client wait for the first to
disconnect; `VoxelLifecycleTests` covers that case with two concurrent clients.

`BrickSnapshot` carries one contiguous node array where index 0 is the brick
root. Decoding validates the frame length, every child index against the
payload, the root-layer bounds, and the descriptor's consistency before any
data is used.

Missing bricks render as a distinct magenta rather than as air, so a
half-streamed world is visible instead of silently wrong.

## Tests

```text
VoxelSvoTests        brick ops, root-layer indexing, snapshot round-trip
                     against direct lookups, upper-brick emptiness,
                     material table, generation revision
VoxelProtocolTests   encode/decode round trips and rejection of malformed
                     and hostile payloads
VoxelLifecycleTests  real loopback UDP/TCP: connect, stream, stop and
                     destroy in both orders, plus two concurrent streams.
                     Covers the shutdown and thread-reaping paths.
```

The snapshot round-trip test walks the compacted array with the same
addressing rules the shader uses and compares against `World::get` over the
whole default world, so a CPU-side mismatch fails there instead of on screen.

## Source map

```text
src/simulation/svo_node.hpp   8-byte packed node, shared CPU/GPU layout
src/simulation/svo.hpp/.cpp   Brick octree, N-ary root layer, World, terrain
src/simulation/state.hpp      wire and render types, material table
src/simulation/authority.*    the only player physics and collision
src/network/protocol.*        encode/decode and payload validation
src/network/udp_socket.*      intent and snapshots
src/network/tcp_socket.*      length-framed world stream
src/network/server.*          world ownership, tick, per-client streams
src/network/client.*          intent out, bricks to GPU, interpolation
src/renderer/vulkancontext.*  arena, uploads, pipelines, compute dispatch
src/shaders/svo.slang         layout constants and traversal helpers
src/shaders/compute.slang     primary ray, shadow ray, sky
src/shaders/fullscreen.slang  compute image blit
src/shaders/overlay.slang     UI overlay
src/config.hpp/.cpp           defaults and command-line parsing
```

## Conventions

- Z is up. One voxel is one world unit.
- Little-endian, fixed-width, versioned wire format.
- Deterministic terrain: integer hashes only, no libm randomness, no
  `std::random`, so the same seed is bit-identical everywhere.
- Threads: one UDP receive+tick, one TCP listener, one thread per world
  stream. Shutdown always joins and is safe to call twice.
