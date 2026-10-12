# LOD and Persistent Saves — Implementation Plan

**Project:** `VoxelRenderer` (server-authoritative sparse voxel octree renderer)
**Scope:** Level-of-detail streaming, a provenance/delta SVO for saves, region-based
persistence, and a budgeted resident cache so big worlds fit in RAM.
**Status:** plan only — no code changes yet.

---

## 0. TL;DR of the design

Three layers, each with a different lifetime and a different cost:

```
  DISK (survives restart, scales with *player activity*)
  ┌──────────────────────────────────────────────────────────────────────┐
  │  world.meta  +  regions/r<X>_<Y>_<Z>.vxr                             │
  │  ┌────────────────────────────────────────────────────────────────┐  │
  │  │ Provenance SVO  (a.k.a. delta / edit octree)                    │  │
  │  │   path exists  => "this voxel was edited, here is the value"    │  │
  │  │   no path      => "pure procedural, regenerate on demand"       │  │
  │  └────────────────────────────────────────────────────────────────┘  │
  └──────────────────────────────────────────────────────────────────────┘
                                   │  generate(procedural) ⊕ delta
                                   ▼
  RAM (bounded by --cache, rebuilt from the two sources above)
  ┌──────────────────────────────────────────────────────────────────────┐
  │  Resident voxel SVO  — the *finest* LOD any player currently needs   │
  │  + derived reduced snapshots per (brick, lod), shared by all clients │
  │  LRU evicted; eviction never loses data (everything is re-derivable) │
  └──────────────────────────────────────────────────────────────────────┘
                                   │  reduce to the per-client ring
                                   ▼
  WIRE / GPU (per client, nearest = full detail, far = coarse)
  ┌──────────────────────────────────────────────────────────────────────┐
  │  BrickSnapshot{ rootIndex, revision, lodLevel, nodes[] }             │
  │  Client uploads into the existing node pool; no CPU voxel world.     │
  └──────────────────────────────────────────────────────────────────────┘
```

The key insight behind the "additional SVO": **the procedural generator is a
pure function of `(seed, x, y, z)`, so unedited voxels are not data — they are
code.** The second SVO therefore stores *provenance*, not voxels: one bit per
volume saying "regenerate this" versus "this was changed, here is the delta".
Storage then scales with the number of player edits instead of the size of the
world.

---

## 1. What exists today (the base we build on)

| Area | File(s) | Behaviour today | Relevance |
|---|---|---|---|
| World spec | `src/simulation/svo.hpp` | Non-cubic root layer `rootDimX×Y×Z = ceil(size/brick)`, brick = 128³ cells | Root index stays the global address everywhere |
| Brick | `src/simulation/svo.hpp/.cpp` | Flat `std::vector<SvoNode>`, 8-node block free list, `compact()` produces the GPU-ready BFS array | `lodLevel` already exists on every node; `compact()` is reused as-is |
| World | `src/simulation/svo.cpp` | Dense `std::vector<Brick>` over the root layer, lazy `generate()`, min/max **height mipmap** for O(1) box-uniformity tests, `collapseReachable()` | `generate()` is the hook for "generate at depth L" (LOD) and for ⊕ delta |
| Node | `src/simulation/svo_node.hpp` | 8 B: `firstChild(24) flags(8) material(16) mask(8) lodLevel(8)` | `lodLevel` = "log2 of this node's edge in cells" — already exactly the LOD field we need |
| Streaming | `src/network/server.cpp` | One TCP thread per client, interest radius in *bricks*, nearest-first, 16 frames/loop, `sent`/`acknowledged` maps keyed by `rootIndex → revision` | Needs to become `(rootIndex → {revision, lod})` |
| Wire | `src/network/protocol.hpp` | `WorldDescriptor`, `WorldInterest`, `WorldAck`, `BrickSnapshot` (already carries `lodLevel`), `kProtocol = 2` | Add LOD-aware fields, bump to 3 |
| Client | `src/network/client.cpp` | Keeps no voxels; `m_resident[rootIndex] = revision`; evicts by Chebyshev distance from interest centre | Needs LOD-aware replace + outer-radius eviction |
| GPU | `src/renderer/vulkancontext.cpp`, `shaders/compute.slang` | Root table + coalescing first-fit node pool (`--pool` 4 M nodes), missing brick = magenta | Reads `lodLevel` from the root node already; no format change needed |
| Threads | `src/network/server.cpp` | 1 UDP+tick thread, 1 TCP listener, 1 thread/client; `m_worldMutex` held for the whole tick *and* for every `snapshot()` | Must be split so generation/eviction cannot stall the tick |

Measured facts to design against (from `README.md`):

- Full default world `1024×1024×256` = 268 M cells → **2.9 M nodes = 23 MiB**, ~150 ms to build → **~181 KB and ~1.17 ms per 128³ brick**.
- `--pool` default = 4 M nodes = 32 MiB GPU.
- `--ray` default = 512 cells = **4 bricks of travel**. This is what bounds how much detail is ever visible.

**Problem this plan solves:** streaming radius 6 at full detail is
`13³ = 2197` bricks ≈ **240 MB of nodes (30 M nodes)**. That is ~30× over the
default GPU pool, ~30× over any sane server RAM budget, and ~30× over what a
client should download. A big world (e.g. `8192×8192×1024`, root `64×64×8` =
32 768 bricks) would need ~1.5 GB of resident nodes even if fully generated.
LOD is not an optimisation here; it is what makes the renderer work.

---

## 2. Layer A — the Provenance SVO (saves / deltas)

### 2.1 Data structure

One sparse octree per brick, byte-layout identical to `SvoNode` so it can reuse
the GPU-free parts of the existing code and be serialised raw. Leaf states:

| State | Meaning | Stored payload |
|---|---|---|
| *no node* | Pure procedural, never touched | none |
| `Solid(material)` | Edited to this material | 2 B material |
| `Air` | Edited to air (carved out) | 0 B extra — needs its own flag so it is distinguishable from "procedural stone" |
| internal | Contains a mix | 8-node block, same `mask`/`firstChild` rules |

So the node format needs **one new flag bit**: `kSvoFlagDelta` (`1u << 2`)
meaning "this node's material is an *override*, not a procedural value". The
flags byte has 8 bits and only 2 are used (`kSvoFlagSolid`, `kSvoFlagResident`),
so this is free and does not disturb the wire layout or the shader.

Rules:

- Writes are `override(x, y, z, material)` (including `material == 0` for air).
- A node whose eight children are *identical overrides* collapses — reuse
  `Brick::collapse()`'s logic, parameterised by "identical" instead of
  "identical leaves".
- **Pruning:** if an override equals what the procedural generator would have
  produced at that cell, the override is deleted (path shortened). This is what
  keeps saves proportional to *net* change rather than to churn. Doing it
  requires one `terrainMaterial()` query per candidate cell — cheap and only on
  the write path.

### 2.2 Why this is "the additional SVO" rather than a second full voxel SVO

A full second voxel SVO would double RAM and disk for zero information: the
procedural half is already recoverable. The provenance SVO stores only the
*residual* between the generator and reality. Reading it back is:

```
buildBrick(rootIndex, maxDepth):
    proceduralWalk(rootIndex, maxDepth)          // existing generate(), depth-limited
    if regionHasDelta(rootIndex):
        applyDelta(deltaTree(rootIndex), maxDepth)   // writeLeaf along delta paths
    collapseReachable(); bump revision
```

Consequence: **evicting a resident voxel brick is always lossless.** The delta
tree is the source of truth; the resident voxel brick is a cache. That single
invariant is what lets us bound RAM by LRU instead of by world size.

### 2.3 Two levels of provenance

1. **Brick granularity:** a `hasDelta` bit in the region slot table. `false` ⇒
   don't even touch the file for that brick.
2. **Voxel granularity:** path existence inside the delta tree.

Both are cheap, both are needed: (1) makes "load this region" O(dirty bricks),
(2) makes the saved file proportional to edits.

---

## 3. Storage — region files

### 3.1 Layout on disk

```
<world-dir>/
  world.meta                     small, read at startup
  regions/
    r000_000_000.vxr
    r001_000_000.vxr
    ...
```

`world.meta` (fixed little-endian, versioned):

```text
magic "VXW1"          u32
formatVersion         u16   (= 1)
generatorVersion      u32   (hash of terrain constants; must match or refuse)
seed                  u64
sizeX sizeY sizeZ     u32 x3
brick                 u32
regionBricks          u32   (region edge in bricks, default 8)
rootDimX Y Z          u32 x3
regionCount           u32
[ per region ]  { rrx, rry, rrz : u16 x3; nodeCount : u32;
                  bytes : u32; revision : u32; mtime : u64; flags : u16 }
```

`regions/r<X>_<Y>_<Z>.vxr`:

```text
magic "VXR1"                       u32
formatVersion                      u16
regionOrigin (rx, ry, rz)          u32 x3
regionBricks                       u32
slotCount                          u32          = number of *dirty* bricks
[ per dirty brick ]
   localBrickIndex                 u32          (index inside the region)
   nodeCount                       u32
   payloadOffset                   u32
   revision                        u32
payload[]                          u32 x nodeCount   (packed SvoNode words)
checksum                           u32          (CRC32 over header+payload)
```

- Region edge `regionBricks` bricks (default 8 ⇒ `8·128 = 1024` cells per axis;
  512 bricks per region). Default world `8×8×2` root ⇒ **one** region file.
  A `64×64×8` root ⇒ 64 region files. Powers of two keep region boundaries
  aligned to brick boundaries, so a brick is never split across two files.
- `world.meta` holds only offsets/summaries ⇒ startup reads a few KB, not the
  world.
- Payload is **not** decompressed to load: we only need the *dirty* bricks, and
  only for bricks a player actually walks into.

### 3.2 Atomicity and concurrency

- Write to `r…vxr.tmp`, `fsync`, `rename()` over the target. A crash therefore
  loses at most the edits of one region *since the last flush*, never a torn
  file.
- Each region has its own `std::shared_mutex`: edits take it exclusively, the
  flush thread copies a snapshot of the dirty delta trees out under the lock
  and writes to disk without holding it.
- Dirty tracking: `std::vector<std::atomic<bool>> m_regionDirty` sized
  `regionCount` (one bit per region is enough at this granularity).
- Flush triggers: every `--save-interval` seconds (default 30), on `Server::stop()`,
  and when dirty regions exceed `--save-max-pending` (default 64).

### 3.3 Compression decision (open)

Octree deltas from real editing are extremely compressible (long runs of
identical siblings). Recommendation: **phase 1 stores raw nodes** (zero new
dependencies, trivially testable), phase 5 adds optional LZ4 block compression
on the payload via `FetchContent` behind a flag. Keep `flags` bit 0 reserved
for "payload is compressed" so the format does not need another version bump.

---

## 4. Layer B — resident voxel SVO with budgets

### 4.1 Containers

Replace the single dense `std::vector<Brick>` with three purpose-built
containers:

```cpp
// Dense over the root layer — O(rootCount) *metadata* only, never voxel data.
struct BrickSlot {
    std::uint32_t revision   = 0;   // streamed revision (already exists today)
    std::uint32_t lastUsed    = 0;   // tick of last collision/stream use (LRU)
    std::uint16_t finestLod   = 0xFFFF; // leaf level resident, 0xFFFF = none
    std::uint8_t  state       = kBrickNone; // None / Resident / Generating
    std::uint8_t  flags       = 0;   // kSlotHasDelta, kSlotPinned
    bool          hasDelta() const { return flags & kSlotHasDelta; }
};
std::vector<BrickSlot> m_slots;                 // 24 B × rootCount

// Resident voxel data, only for bricks inside the interest window.
std::unordered_map<std::uint32_t, Brick> m_resident;   // LRU by m_slots.lastUsed

// Derived reduced snapshots shared by all clients.
struct ReducedKey   { std::uint32_t rootIndex; std::uint8_t lod; };
struct ReducedEntry { std::uint32_t revision; std::vector<SvoNode> nodes; };
std::unordered_map<std::uint64_t, ReducedEntry> m_reduced;   // LRU by m_slots.lastUsed
```

Numbers: `524 288` bricks (a `256×256×8` root) × 24 B = **12.6 MB of metadata**,
and only the resident window ever allocates nodes. For roots beyond ~1 M bricks,
switch `m_slots` to an open-addressed map keyed by `rootIndex` (the README
already names this as the upgrade path); call that out in code comments.

### 4.2 Pinning

A brick is **pinned** while any player's swept collision AABB (expanded by one
cell) intersects it, or while an edit to it is unflushed. Pinned bricks are
never evicted, so `Authority::collides()` never regenerates mid-sweep. As soon
as the last player leaves, it becomes evictable.

### 4.3 Budgets

| Budget | Flag | Default | Enforced by |
|---|---|---|---|
| Resident nodes | `--cache` | 4 M nodes (32 MiB) | LRU evict until under budget, after every install |
| Reduced snapshots | `--cache-reduced` | 1 M nodes | LRU evict (they are always re-derivable) |
| Pinned bricks | — | derived from `kMaxPlayers` (16) × AABB | hard, not a budget |
| Delta trees loaded | `--delta-cache` | 256 bricks | LRU; flushing writes them out first |
| Dirty regions pending | `--save-max-pending` | 64 | triggers an immediate flush |

Eviction order: reduced snapshots → unpinned resident bricks (fully, including
`Brick::m_nodes`) → delta trees (after flush). Every step is safe because of the
invariant in §2.2.

### 4.4 Threading

Generation is ~1.2 ms/brick today and currently runs *inside* `m_worldMutex`
from a stream thread, so it can stall the 60 Hz tick. Split ownership:

```
tick thread (UDP)      : authority + collision. May call buildBrick() for a
                         *pinned* brick synchronously (rare: only on first
                         contact with a new brick) — budgeted, warn > 2 ms.
stream threads (TCP)   : never generate. They *request* a brick and get either
                         a ready shared snapshot or "busy, try next loop".
world worker (1 thread): owns m_resident / m_reduced / eviction / flush queue.
                         Processes (index, maxDepth) requests from a bounded
                         queue; publishes via a lock-free hand-off of an
                         immutable, already-compacted `std::vector<SvoNode>`.
```

Stream threads should hold a `std::shared_lock` only while copying a
`std::shared_ptr<const std::vector<SvoNode>>` out of a slot — the actual
`encode()` + `sendFrame()` happen outside any lock. This removes the current
serialisation bottleneck where every client's compaction blocks every other
client and the tick.

---

## 5. Layer C — LOD

### 5.1 Ring table (one source of truth, shipped to the client)

```cpp
// src/simulation/lod.hpp
struct LodRing { std::uint16_t maxDistanceBricks; std::uint8_t lodLeafLevel; };
inline constexpr LodRing kDefaultRings[] = {
    { 1,  1 },   // 0–1 bricks away   → leaves of 2 cells   (near)
    { 3,  4 },   // 2–3 bricks        → leaves of 16 cells  (mid)
    { 5,  8 },   // 4–5 bricks        → leaves of 256 cells (far)
};
std::uint8_t lodForDistance(int chebyshevBricks, std::span<const LodRing>);
```

`lodLeafLevel` is the level of the **finest nodes stored** in that brick's
snapshot (level `k` ⇒ leaf edge `2^k` cells; level 0 = full detail). The brick
root keeps `lodLevel = brickLodLevel(brick) = 7`, because it still geometrically
covers 128 cells — only the leaves stop early. This is exactly what
`Brick::allocateBlock()` already does when it decrements `lodLevel` per level.

The **outer ring radius must track `--ray`**: rays travel `maxRayDistance`
cells, so nothing beyond `ceil(maxRayDistance / brick) + 1` bricks is ever
visible. Derive the default outer radius from that instead of hard-coding 6 —
for the default `512 / 128 + 1 = 5`. Make the ring table part of
`WorldDescriptor` so client eviction order and server generation can never
disagree.

### 5.2 Generating directly at a coarse depth (pristine bricks)

Parameterise the existing generator — this is the cheapest possible LOD because
the height mipmap already answers "is this box uniform" in O(1):

```cpp
void World::generate(std::uint32_t rootIndex, std::uint8_t maxDepth);
// identical walk; when current.level == maxDepth and the box is NOT uniform,
// instead of recursing to level 0, emit a single conservative solid leaf.
```

Conservative rule (never produces holes at an LOD seam):

```text
existing tests first (all-air / all-lava / all-stone / all-dirt → uniform leaf)
else (mixed box):
    fillTo = high            // max column top in the box, from the mipmap
    if fillTo <= z0:  writeLeaf(air)
    else:             writeLeaf(surfaceMaterialAt(cx, cy, fillTo))
```

*Why max, not average:* every coarse box is at least as tall as reality, so a
coarse brick can never be *shorter* than its finer neighbour across a brick
boundary — the shared face is solid for the full height of the shorter one, so
no cracks appear. Average would be able to dig a gap. Coarse boxes also all sit
on the same `2^k` lattice, so two coarse neighbours tile exactly.

Optimisation: for `maxDepth > 0` build the mipmap **top-down** (sample
`2^maxDepth × 2^maxDepth` groups directly) instead of the current bottom-up
sweep. For `maxDepth = 4` that is 1024 `terrainTop()` calls instead of 16 384 —
roughly 16× less work on the generation hot path.

### 5.3 Reducing an already-resident full brick (edited bricks)

Edited bricks must be built at full depth (the delta is stored per voxel), then
reduced bottom-up:

```cpp
// returns the material/occupancy summary of the subtree rooted at node
BoxSummary reduceToLevel(const Brick& src, std::uint32_t node,
                         std::uint8_t targetLevel, Brick& dst);
// if node.lodLevel == targetLevel → copy the node as a leaf
// else reduce children, then:
//    solidCells / totalCells from the children's summaries
//    if all identical leaves        → collapse (existing rule)
//    else if solidFraction >= 0.5   → solid leaf, material = majority
//    else                           → air leaf
```

This costs O(nodes in subtree) and produces a strictly smaller array. Because
the summary is computed bottom-up we get correct majority voting without
changing `SvoNode`.

### 5.4 Which path is used when

| Brick state | Player distance | Build path | Cost |
|---|---|---|---|
| pristine | any ring | `generate(rootIndex, ring.lodLeafLevel)` direct | ~0.1 ms / <2 KB |
| edited | any ring | full `buildBrick()` + `reduceToLevel()` | ~1.2 ms / then reduced |
| pinned (collision) | 0 | full `buildBrick()` | ~1.2 ms, once |

### 5.5 Estimated savings (default world, outer radius 5)

| Ring | Bricks | Leaf | Est. nodes/brick | Bytes/brick | Subtotal |
|---|---|---|---|---|---|
| 0–1 | 27 | 2 cells | ~8 000 | ~64 KB | ~1.7 MB |
| 2–3 | 316 | 16 cells | ~120 | ~1 KB | ~0.3 MB |
| 4–5 | 1 024 | 256 cells | ~40 | ~0.3 KB | ~0.3 MB |
| **total** | **1 367** | — | — | — | **~2.3 MB, ~0.5 M nodes** |

versus today's full-detail equivalent ≈ **240 MB / 30 M nodes**. That is
**~100× fewer nodes, ~100× less bandwidth**, and it finally fits the default
4 M-node GPU pool at ~13% utilisation instead of 750%.

*These node counts are estimates from the generator's collapse behaviour and
**must be measured** in phase 0 (add counters, print a table); they drive the
ring defaults.*

---

## 6. Streaming with multiple players

Per client, independently (each has its own TCP thread and interest centre):

```cpp
// server.cpp, inside worldConnection()
const std::uint8_t lod = lodForDistance(chebyshevBricksFrom(center, index), rings);
const std::uint32_t want = cache->request(index, lod);   // or "busy"
if (want == kBusy) continue;                             // try again next loop
const auto& snap = cache->shared(index, lod, want);
if (alreadySent(index, want, lod)) continue;
sendFrame(encode(BrickSnapshot{..., revision = want, lodLevel = lod,
                               nodes = *snap.nodes}));
rememberSent(index, want, lod);
```

The **server-resident** LOD is the *finest* any player needs
(`min over players of lodForDistance`), because one brick serves all clients.
Client A at distance 0 gets level-0 leaves from a brick that client B, 500
cells away, receives at level 8. Only `m_resident` is deduplicated;
`m_reduced` is keyed by `(rootIndex, lod)` so two clients in the same ring
share the compaction work (they still each get their own TCP copy, as today).

Refresh policy: when a player moves, only bricks whose **ring changed** are
re-requested; the ring table is coarse enough that this is a small set per
movement. Revisions stay per-brick, so nothing is resent when nothing changed.

Client-side replacement rule becomes: replace if `newRevision != heldRevision
|| newLod != heldLod`. That is why the ack/sent maps must key on both.

---

## 7. Protocol changes — bump `kProtocol` 2 → 3

| Message | Change | Why |
|---|---|---|
| `WorldDescriptor` | + `lodRingCount : u8`, + `lodRing[ ] : {u16 maxDistanceBricks, u8 lodLeafLevel}` | client must evict/prefetch on the *same* rings the server generates |
| `WorldDescriptor` | + `outerRadius : u8` | replaces the client's hard-coded 6 |
| `WorldDescriptor` | + `generatorVersion : u32` | client can detect a server world rebuilt under a different generator |
| `BrickSnapshot` | `lodLevel` **redefined** as "finest stored leaf level" (was always `brickLodLevel`) | already present on the wire — only its meaning changes |
| `WorldAck` | + `lod : u8` | acks must be matched against `{revision, lod}` or a stale full-res ack would suppress a needed coarse send |
| `BrickUpdate` (client-internal, `state.hpp`) | + `lod : u8` | renderer bookkeeping / UI |

`sent` / `acknowledged` maps in `Server::StreamClient` change from
`unordered_map<u32,u32>` to `unordered_map<u32, Entry{u32 revision; u8 lod;}>`.

No change needed to `SvoNode`, the root table layout, or the shader's
traversal: a coarse leaf is still just a leaf whose `lodLevel` is larger.

---

## 8. Client and renderer changes

1. **`Client::enqueueBrick`** — replace on `(revision, lod)` change; store `lod`
   next to `revision` in `m_resident`.
2. **`Client::updateRender`** — evict by `outerRadius` from `WorldDescriptor`
   instead of the local `m_interestRadius = 6`; also drop anything beyond
   `maxRayDistance / brick + 1` so the pool is never filled with invisible data.
3. **Prefetch order** — send interest for the *whole* outer ring but let the
   server order by distance (already does). Optionally add a `wantLod` hint so
   the client can say "I am stationary, give me the near ring first"; keep it
   optional to avoid a protocol round-trip in the common case.
4. **`VulkanContext`** — reads `update.lod` into `Occupancy` so the UI can show
   per-ring resident counts and the pool histogram per ring (cheap to add to the
   existing clay overlay).
5. **Shader (optional, phase 5)** — `lodLevel` is already in the root table
   entry, so `compute.slang` can:
   - skip the shadow ray for nodes with `lodLevel >= 6` (far bricks pay for the
     most expensive part of the trace for the least visible gain),
   - apply distance fog / a slight colour desaturation to hide LOD seams,
   - exit `walkBricks` early once `distance > pc.maxRayDistance` (already
     implicit, but now it can also stop *descending* coarse bricks early).

---

## 9. Config / CLI additions

```text
--world-dir <path>   load/save directory (empty = ephemeral, current behaviour)
--save-interval <s>  flush period (default 30, 0 = only on shutdown)
--save-max-pending <n>  flush when this many regions are dirty (default 64)
--cache <nodes>      resident voxel node budget (default 4M nodes)
--cache-reduced <n>  reduced-snapshot node budget (default 1M)
--region <bricks>    region edge in bricks, power of two (default 8)
--lod <preset|table> override the ring table
--nosave             never write (tests, benchmarks)
--cache-stats        print resident/reduced/delta/flush counters on shutdown
```

`--radius` (existing) now means "outer ring radius override"; its default
becomes derived from `--ray`.

---

## 10. Budget summary — why this scales

| | Small world (default) | Big world `8192×8192×1024` |
|---|---|---|
| Cells | 268 M | 68.7 G |
| Root bricks | 128 | 32 768 |
| Full voxel SVO | 23 MiB | ~1.5 GB ✗ |
| Dense `m_slots` | 3 KB | 0.8 MB ✓ |
| Resident + reduced (budgeted) | ≤ 32 MiB + 8 MiB | ≤ 32 MiB + 8 MiB ✓ |
| Delta trees in RAM | ≤ `--delta-cache` | ≤ `--delta-cache` ✓ |
| **Disk, no edits** | `world.meta` only (~1 KB) | ~4 KB only ✓ |
| **Disk, 100 k edited voxels** | ~0.5–1 MB | ~0.5–1 MB ✓ |

Storage and RAM both become functions of *player activity and interest radius*,
never of world volume. That is the goal.

---

## 11. Correctness invariants (each becomes a test)

1. **Regenerate-equals-rebuild:** for any brick, `buildBrick(i, 7)` ==
   `procedural(i) ⊕ delta(i)` cell-for-cell.
2. **Eviction is lossless:** edit → force-evict → rebuild → every edited cell
   still reads back the edited value.
3. **Prune is sound:** deleting an override that equals the procedural value
   does not change any `World::get()`.
4. **Save/load round-trip:** build → flush → construct a fresh `World` from the
   same directory → identical `World::get()` over the whole world (reuse the
   existing whole-world snapshot test shape from `tests/svo_test.cpp`).
5. **LOD is conservative:** every solid cell of the full-resolution brick lies
   inside a solid coarse box (no holes introduced by reduction).
6. **LOD is monotone:** a coarser ring never has more nodes than a finer one.
7. **Crack-free:** adjacent bricks reduced at the same leaf level tile without
   gaps across their shared face.
8. **Acknowledgement correctness:** a `(revision, lod)` ack never suppresses a
   later send at a different `lod`.
9. **Crash safety:** killing the process mid-flush leaves either the old or the
   new region file, never a truncated one; `checksum` mismatch ⇒ region
   refused with a clear error, not silent corruption.
10. **Budget:** a synthetic walk never exceeds `--cache` resident nodes.
11. **Tick isolation:** generation of a distant brick never blocks the tick
    thread (assert the tick never exceeds 2× `kTickSeconds`).

---

## 12. Phased implementation plan

Each phase compiles, passes `ctest`, and is independently valuable.

### Phase 0 — Measure first (small, no behaviour change)

*Goal:* replace the estimates in §5.5 with numbers, so ring defaults are data-driven.

- Add counters to `World::generate()`: nodes, bytes, microseconds per brick and
  per depth.
- Add a `--lod-bench` mode that builds bricks at `maxDepth = 0,2,4,6,8` and
  prints a table (this requires the `maxDepth` parameter — implement it here as
  a no-op-by-default argument).
- Record current `--pool` utilisation vs. radius for the default world.

*Deliverable:* a table in this document's §5.5 replaced by real numbers.

### Phase 1 — Provenance SVO + region persistence (server only, no wire change)

*Goal:* restart preserves edits. Nothing about LOD yet.

New files:
- `src/simulation/provenance.hpp/.cpp`
  - `class ProvenanceTree` — override octree per brick: `override()`,
    `query()`, `prune()`, `collapse()`, `applyTo(Brick&, maxDepth)`,
    `serialize()`, `deserialize()`, `nodeCount()`.
  - Reuses `SvoNode`; adds `kSvoFlagDelta = 1u << 2`.
- `src/simulation/region.hpp/.cpp`
  - `struct WorldMeta { … }`, `class RegionStore`
    - `open(dir)`, `loadRegion(r)`, `flushRegion(r)`, `flushAll()`
    - `deltaFor(rootIndex)` → `ProvenanceTree*`, `markDirty(rootIndex)`
    - atomic `tmp + rename`, CRC32, per-region `shared_mutex`
    - `generatorVersion()` guard (`svo.cpp` gains
      `constexpr std::uint32_t kGeneratorVersion`, hashed from
      `kTerrainSeed/kTerrainBase/kLavaBand/kDirtBand`)

Modified:
- `src/simulation/svo.hpp/.cpp`
  - `World` gains an optional `RegionStore*`; `set()` writes the resident brick
    **and** the provenance tree; `hasDelta(rootIndex)` on the slot.
  - `generate()` gains `maxDepth` (default = current behaviour) — used by phase 2,
    harmless here.
  - `isSolid()` fast path: if `!hasDelta && !resident`, evaluate
    `terrainMaterial()` directly with no brick allocation at all (this alone
    removes most on-demand generation from the collision path).
- `src/network/server.cpp` — `--world-dir`, flush interval, `stop()` flushes.
- `src/main.cpp`, `src/config.*` — new flags.
- `CMakeLists.txt` — new sources + `VoxelPersistTests`.

Tests (`tests/persist_test.cpp`):
- round-trip (invariant 4), prune soundness (3), atomic write + corrupt-file
  rejection (9), generator-version mismatch refusal, `--nosave` produces no
  files.

*Milestone:* place 1000 blocks, `Ctrl-C`, restart, blocks are still there;
`world.meta` is a few KB for an untouched world.

### Phase 2 — Budgeted resident cache with eviction (server only)

*Goal:* RAM bounded for a big world; no more `generateAll()`.

- Introduce `m_slots` / `m_resident` / `m_reduced` from §4.1, plus
  `pinBricksAroundPlayer()`.
- Move generation + eviction to a single world-worker thread with a request
  queue; `World::snapshot()` becomes a read of an immutable compacted array.
- Replace `if (rootCount <= 4096) generateAll()` with warm-up of the bricks
  around spawn.
- Eviction: LRU over `m_slots.lastUsed`, respecting `--cache` and pinning.
- Logging: `--cache-stats`.

Modified: `src/network/server.hpp/.cpp`, `src/simulation/svo.*`,
`src/simulation/state.hpp` (nothing yet), `CMakeLists.txt`.

Tests (`tests/cache_test.cpp`):
- eviction losslessness (2), budget ceiling (10), pinning never evicts,
  rebuild-after-evict equals original (1).

*Milestone:* a `8192×8192×1024` world starts instantly, resident memory stays
under `--cache`, and edits survive eviction.

### Phase 3 — LOD generation + protocol v3 (the visible one)

*Goal:* far bricks are small.

- `src/simulation/lod.hpp/.cpp` — ring table, `lodForDistance()`, `reduceToLevel()`.
- `World::generate(rootIndex, maxDepth)` — conservative max-top rule (§5.2),
  top-down mipmap for `maxDepth > 0`.
- `World::snapshot(rootIndex, maxDepth, …)` — dispatches pristine-direct vs.
  edited-full+reduce (§5.4).
- `src/network/protocol.*` — fields from §7, `kProtocol = 3`.
- `src/network/server.cpp` — per-client ring lookup, `{revision, lod}` maps.
- `src/network/client.cpp` — replace on `(revision, lod)`, `outerRadius`
  eviction, ring table from descriptor.
- `src/simulation/state.hpp` — `BrickUpdate.lod`.
- `CMakeLists.txt` — `VoxelLodTests`.

Tests (`tests/lod_test.cpp`):
- conservative (5), monotone (6), crack-free (7), pristine-direct == full-then-reduce
  for a sampled set of boxes (a weaker form of invariant 1),
  ack/lod (8) in `tests/protocol_test.cpp`, plus a two-client lifecycle test in
  `tests/lifecycle_test.cpp` asserting the same `rootIndex` arrives with two
  different `lodLevel`s.

*Milestone:* startup download drops ~100×, `--pool` utilisation falls from
over-capacity to ~13%, and the far horizon still renders as terrain rather than
magenta.

### Phase 4 — Multi-player LOD merging + save integration

*Goal:* the design's end state.

- Server computes the resident LOD as `min over players`; verify a brick
  serving two clients at different distances is resident once and reduced twice.
- Re-request only bricks whose ring changed when interest moves.
- Region flush on interval + shutdown; dirty-region back-pressure.
- Spawn-point pre-generation so the first tick never blocks.
- Handle `generatorVersion`/seed mismatch as a hard, explained error.

Tests: extend `VoxelLifecycleTests` with two clients at distant spawn points;
save/flush under load; restart with a different `--seed` refuses to open.

*Milestone:* two clients 2 000 cells apart each see full detail at their feet
and coarse terrain over the horizon, with a single resident copy of each brick.

### Phase 5 — Polish (optional)

- LZ4 payload compression behind `--compress` (reserved flag bit).
- Shader: shadow-ray cutoff by `lodLevel`, LOD fog/desaturation.
- Sparse `m_slots` above ~1 M bricks.
- A coarse "preview" LOD cached in the region file for very expensive future
  generators (caves/biomes) — i.e. pre-baking part of the procedural baseline
  when regeneration itself becomes the cost. **Not needed with today's
  generator**, which is a pure, cheap function; documented here so the choice
  can be revisited without redesign.
- Sub-pixel brick culling on the client (don't request bricks that project to
  < 1 px given `tanHalfFov`).

---

## 13. Resolved decisions (confirmed 2026-10-11)

| # | Question | Decision |
|---|---|---|
| 1 | Save directory layout | **Single `--world-dir <path>`**, one directory holds one world. Swap the path to switch worlds. |
| 2 | Flush cadence | **Interval (30 s) + every 4096 pending edits.** A hard kill loses at most one threshold, not an unbounded window. |
| 3 | Prune-on-write | **Yes.** An override equal to the procedural value is dropped, so saves track *net* change rather than churn. |
| 4 | Compression | **Phase 5** (LZ4 behind `--compress`, reserved flag bit). Phase 1 stores raw nodes — no new dependency, trivially testable. |
| 5 | Ring defaults | **Derived from `--ray`**, as §5.1 recommends. `outerRadius = ceil(ray / brick) + 1`. |
| 6 | Edited bricks at far rings | **Build full, then reduce** (see the note below). |

**Hardware budget: ≥ 8 GB VRAM, ≥ 16 GB server RAM.** This sets the defaults:

| Flag | Default | Rationale |
|---|---|---|
| `--pool` (GPU node arena) | **256 M nodes = 2 GiB** | ~6 GiB of VRAM left for swapchain, compute image and UI. Under LOD the small world needs ~0.7 M nodes, so this is headroom for a large world, not a tuning target. |
| `--cache` (server resident voxel SVO) | **512 M nodes = 4 GiB** | ~11 GiB of the 16 GiB stays free for the delta cache, the allocator and page cache. |
| `--delta-cache` | **128 M nodes = 1 GiB** | |
| `--cache-reduced` | 64 M nodes = 512 MiB | |

All are flags, so a smaller machine can still run this; the defaults just stop
being conservative.

**On decision 6.** Building full and then reducing costs a full-resolution
bricks' worth of scratch memory (~181 KB, ~1.2 ms) for edited bricks only.
The alternative — applying the delta straight onto a coarse procedural brick —
needs a second, separate reduction path with its own crack-freedom proof. Not
worth it for the number of bricks a player actually edits. If edited bricks at
distance ever become hot, that is the lever.

### 13.1 Pre-existing bug found while planning (fixed in Phase 1)

Found during planning, before any LOD work. Two related defects in the
existing streaming path:

**Magenta that never recovers.** `Server::StreamClient::sent` and
`::acknowledged` (`server.cpp:53-54`) are written on send/ack and **never
purged**. The client *does* evict bricks that leave its interest window
(`client.cpp:291-320`) and clears their GPU root-table entries. When such a
brick re-enters the window the client asks for it again, but `server.cpp:294`
sees a matching `acknowledged` entry and skips it. The brick stays magenta for
the rest of the session. Walking to `x=1024` and back therefore permanently
loses every brick the walk evicted — `bx=7` spans `x ∈ [896, 1024)`, which is
the reported "giant pink circle roughly 100 blocks before the other end".

**Inverted shadows on the missing half.** Same cause. The sun direction is
`(-0.55, -0.35, 0.75)`, so shadow rays travel toward −x/−y. When those bricks
are missing, `compute.slang:206` aborts the shadow ray with `hitMaterial =
0xFFFF` and `walkBricks` returns `false`, which `occluded()` reports as *not
blocked*. Every shadow that should have been cast by the missing half vanishes,
and the lit half reads as though the sun were on the far side. That is the
reported 180° shadow inversion, and it explains why it tracks the magenta
region exactly.

Two aggravating defects in the same path:

- `client.cpp:221` — `while (m_render.bricks.size() > 256) erase(begin())`
  silently discards queued bricks that were already marked resident *and*
  acknowledged, so the server stops resending them.
- `client.cpp:218` — the ack is emitted on *receive*, not on *GPU upload*, so
  the server can never learn that a brick failed to land.

Fixes, all in the Phase 1 stream work:

1. The server purges `sent`/`acknowledged` for any root index outside the
   client's current interest window every pass, so a re-entering brick is
   resent.
2. The client evicts from the *same* window the server streams, so the two
   never disagree.
3. The queue bound becomes **back-pressure** (stop reading frames) instead of
   dropping frames.
4. The ack moves to *after* GPU upload: `BrickUpdate` carries the payload, the
   renderer confirms it with `uploaded = true`, and only then does the client
   queue the `WorldAck`. A brick the pool could not accept is never acked, so
   the server retries it instead of giving up.
5. A brick that is *sent but never acknowledged* is now retried after a 500 ms
   grace period. Without this, the four fixes above are not enough: the client
   would correctly withhold an ack, and the server would wait for it forever.
   `StreamClient::Delivery` carries the revision and the send time for this.
6. The shader's shadow path treats a missing brick as **blocking**
   (`occluded()` returns `hit || hitMaterial == kUnknownMaterial`). Unknown data
   must never read as air; the existing comment at `compute.slang:204` said so
   already, but the shadow path did not honour it.
   `kUnknownMaterial` replaces the bare `0xFFFF` literals, and `walkBricks`
   now clears its out-params so a `false` return cannot leave the caller
   reading uninitialised memory. That removed 6 of the shader's 9
   "returning without initializing out parameter" warnings.

### 13.2 What the regression test found, and one more real bug

`tests/stream_test.cpp` pins the four defects at the seam where a GPU renderer
would normally be, with a stand-in renderer that accepts some updates and
refuses others. Cases 1, 2 and 4 map onto defects above. **Case 3** — walk far
enough that the interest window turns over completely, come back, and require
redelivery — is the headline one, and it was verified to actually fail when the
purge is disabled:

```
# with the purge removed:
case 3: nothing was redelivered on returning to a visited window
```

Writing it surfaced a defect that was not in the original list, and it was one
of my own making: the first version of the ack-after-upload fix recorded a
brick as *resident* when it was **queued**. If the node pool then refused it,
the entry was gone but the record remained, so every resend was dismissed as a
duplicate — the client waiting for an upload that could never arrive, the server
waiting for an ack that was never coming. A wedge, not a hole, but strictly
worse. `m_resident` is now written only where the renderer confirms an upload.

Case 4 then found the second one, also mine: the eviction pass pushed `remove`
updates with no bound at all, and a large window shift queued hundreds of them
(the test measured 468 entries against a 256 limit). Frees cannot simply be
dropped — an unprocessed free leaks the GPU slice the entry still owns — so
they are tracked separately from uploads (`m_pendingFrees`,
`m_evictionBacklog`) and the upload bound is applied to uploads alone, with
bounded slack for frees.

### 13.3 A pre-existing movement bug found on the way

Chasing the walk's failure turned up a bug unrelated to streaming and present
in `HEAD`. `server.cpp:133` blanked the *entire* intent on a replayed tick:

```cpp
if (intent.clientTick <= client.authority->state().acknowledgedInputTick)
    intent = {};
```

Only the **event** fields may be dropped on a replay — re-firing a jump that
already happened is not idempotent — but the continuous inputs are a held
state. Blanking them stopped the player dead mid-stride whenever UDP delivered
slower than the 60 Hz tick rate, which is most of the time. The fix clears
`clientTick`, `jumpPressed` and `descendPressed` and holds the rest. Measured
before/after on loopback: the same walk covered **5.6 blocks** before and
**1181** after.

Two measurement traps are worth recording, because both cost real time here:

- `renderState().localPlayer` is *interpolated*, so sampling it measures the
  render thread's catch-up, not the player. Movement must be read from
  `authoritativeState()`.
- `spawnPosition` returns `(sizeX/2, sizeY/2, top+6)`, so a player spawns
  **floating six blocks above the ground** and falls on the first tick. Combined
  with correct hillside collision, a naive walk stalls at the first rise — the
  test ascends to a safe altitude and measures horizontal distance only.

---

## 14. Open decisions (deferred past Phase 3)

None blocking. Phase 4/5 items stay as written above.

---

## 15. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Generator constants change → old deltas no longer mean what they did | silent world corruption | `kGeneratorVersion` in `world.meta`, refuse to open on mismatch (invariant 9) |
| Coarse LOD rule produces a seam/hole | visible cracks | conservative max-top rule + invariants 5/7 as tests |
| Ring change storms when a player runs along a ring boundary | bandwidth churn | coarse rings (hysteresis: only re-request when distance crosses a ring by ≥ 1 brick, plus a 0.5 s re-request debounce) |
| Generation stalls the tick | physics hitching | single worker thread, pinned-only synchronous path, tick-latency assert (invariant 11) |
| Delta tree grows unbounded from a griefing flood | disk exhaustion | `prune()` on write, `--save-max-pending` back-pressure, per-region size warning in `--cache-stats` |
| `firstChild` is 24-bit (16.7 M nodes) | would cap a *single* brick snapshot | already bounded by `kMaxBrickNodes`; LOD only ever shrinks this |
| Region file torn by crash | lost region | tmp + `fsync` + `rename`, CRC32 verified on load |
| Region edge not a multiple of root dims (tiny worlds) | brick split across files | clamp `regionBricks` to `min(configured, rootDim)` per axis; power-of-two check |

---

## 16. Implementation status

| Phase | Scope | State |
|---|---|---|
| 0 | Measure the ring curve, extract the shared octree | done |
| 1a | `Octree` extracted; `ProvenanceTree`; `RegionStore`; `World` edit path | done |
| 1b | `generatorVersion()` guard, prune-on-write, region files, atomic writes | done |
| 1c | **Streaming bug fix** (sticky `sent`/`acknowledged`, dropped frames, ack-before-upload) | done |
| 2 | `BrickSlot` / `m_resident` / `m_reduced`, LRU eviction, world worker | pending |
| 3a | `reduceToLevel` — LOD derived from the full tree, with ring measurements | done |
| 3b | Ring table + per-brick LOD in the server stream path | done |
| 3c | Client relies on server LOD; renderer consumes reduced bricks | done |
| 1d | **Movement direction**: client resolves WASD against its own camera and sends a world direction | done |
| 4 | Multi-player LOD merging, flush integration | out of scope for this pass |
| 5 | LZ4, shader polish, sparse slots | out of scope for this pass |

`ctest`: 9/9 passing — VoxelSvoTests, VoxelPersistTests, VoxelLodTests,
VoxelProtocolTests, VoxelAuthorityTests, VoxelLifecycleTests, VoxelStreamTests,
VoxelDirectionTests, VoxelLodRingTests.

### 16.1 Measured, not estimated

The ring curve in §5.5 was a guess; here it is, on the default world with a
real 128³ brick (`tests/lod_test.cpp` pins these properties):

| leaf level | nodes | bytes | reduction |
|---|---|---|---|
| 7 (full) | 63,876 | 499 KB | 1× |
| 6 | 5 | 40 B | 12,775× |
| 5 | 37 | 296 B | 1,726× |
| 4 | 293 | 2.3 KB | **218×** |
| 3 | 1,232 | 9.6 KB | 52× |
| 2 | 5,958 | 46.5 KB | 11× |

Note this is *not* monotonic in the level, and that is correct rather than
surprising: a coarser level collapses a whole mixed box into one leaf, so its
size depends on how the terrain happens to fall. Level 6 is nearly free
because a brick's upper half is sky. This is why the plan's "crack-free by
conservative max-top" rule was replaced by a per-child rule (§16.2).

### 16.2 Two corrections made during implementation

**The generator had a real bug, found by an existing test.** `tests/svo_test.cpp`
asserts the world equals a pure `terrainMaterial()` grid. It did not, for
**37% of all cells** — and identically on the pre-existing HEAD, so this was
never a regression. The cause: the generator's box-uniformity tests applied
`kLavaBand`/`kDirtBand` to *brick-local* z while `terrainMaterial` applies them
to *world* z, so every brick repeated the strata at its own floor, and a 32-tall
world grew lava at z=16. A second test in the same file caught the dirt band
collapsing every deep box onto "dirt".

Both are now fixed by asking `terrainMaterial` directly for a box's bottom and
top faces and requiring them to agree, which is exact rather than a
hand-derived bound. The generator now matches `terrainMaterial()` at **every**
cell of the test world (0 differences, was 73,728).

**LOD is derived, never generated.** An earlier draft parameterised the
generator walk with a `maxDepth` and stopped it early. That is a second
implementation of the terrain rules, and the two immediately disagreed — the
same class of bug as above. `World::generate` now has no depth parameter at
all; `reduceToLevel` rewrites the full-resolution tree. See §17.

### 16.3 Cut-out chunks in the terrain (fixed)

Reported as parts of the world being skipped, leaving holes. Found by
comparing generation against `terrainMaterial` at *every* cell of a large
world — an exhaustive check the existing test only ran on a 64×96×32 world,
too small for the defect to exist.

**Cause.** When `buildProcedural` collapses an SVO box to a single leaf it asks
`terrainMaterial` about the box's **centre column** and trusts that answer for
the whole box. That is sound only if every column in the box shares one surface
height. The height mipmap computes exactly that — `low` is the minimum column
top, `high` the maximum — but **`low` was built all the way up the mipmap and
then never read**. Homogeneity was assumed, never established.

Any box straddling a hillside was therefore stamped flat with its centre
column's material, carving terrain away. Measured on 1024×1024×256:

| | before | after |
|---|---|---|
| wrong cells | 493,461 | **0** |
| air where terrain belongs | 302,666 | **0** |

Every mismatch was `0 -> material`: the generator produced **air where terrain
belonged** and never wrongly solid, which is why it read as cut-out chunks
rather than as walls. The counts were identical across five seeds, so this is
structural rather than a property of any one landscape.

**Fix.** A box may only be collapsed when `low == high`. Cost: 234 → 283 KB per
brick, since the tree no longer over-collapses. The fix is exact — no
conservatism, no hole risk.

This is also why the LOD reduction in §3 was worth measuring rather than
assuming: a generator that silently cuts holes would have had them *baked into
every LOD level*, and `reduceToLevel`'s conservative rule would have faithfully
preserved them.

### 16.4 The test suite was inert (fixed)

`CMAKE_BUILD_TYPE=RelWithDebInfo` defines `NDEBUG`, which compiles every
`assert` out to nothing. So `ctest` reported **"Passed" for `VoxelSvoTests`,
`VoxelLodTests` and `VoxelPersistTests` while those binaries asserted
nothing at all** — including the generator oracle above, which passed with the
defect deliberately reintroduced.

Fixed in `CMakeLists.txt` rather than per test file, so the next test cannot
forget it:

```cmake
add_compile_options($<$<CONFIG:Debug,RelWithDebInfo>:-UNDEBUG>)
```

`tests/authority_test.cpp` already carried a comment naming this hazard and
used explicit `CHECK` macros to avoid it; it just was not enforced anywhere
central. After the fix `VoxelSvoTests` went from 0.15 s to 10.9 s, which is the
visible proof it now does real work.

Worth stating plainly: every "N/N tests pass" reported before this was
meaningless for those three suites. The new exhaustive generator test was then
confirmed to *fail* against the old code (`Assertion failed: (wrong == 0)`)
rather than trusted on a green run.

### 16.5 Movement is a direction, not a keypress (fixed)

The wire format used to carry raw WASD, and the server rebuilt the camera
basis from its own replicated `yaw` to resolve it. Two copies of that basis
existed — one in `authority.cpp`, one in the renderer — and only the client
knew which way the screen was actually facing, so the shipped copy walked in
the direction of the *last replicated* yaw rather than the one on screen.

Symptom: holding W always walked along whichever world axis the last agreed yaw
happened to resolve to. Measured against the old behaviour by turning the
camera to four yaws and holding W:

| yaw | expected direction | old behaviour | now |
|---|---|---|---|
| 0° | (1.00, 0.00) | **0.00** (walked +Y) | **1.00** |
| 90° | (0.00, 1.00) | 1.00 (accidentally right) | **1.00** |
| 180° | (−1.00, 0.00) | **−0.00** | **1.00** |
| −90° | (0.00, −1.00) | **−1.00** | **1.00** |

Three of the four cases were simply wrong. `tests/direction_test.cpp` pins it
end to end, because the property under test *is* the wire direction agreeing
with the drawn direction — nothing less exercises it.

**What changed.** `InputIntent` gained `moveDirection`, a world-space vector,
and lost `move`. `Client::submit` resolves `rawMove` through the same
`m_localYaw` the renderer draws with — there is now **one** camera basis in
the codebase — and the server normalises the magnitude so a diagonal cannot
outrun a single axis and a longer vector cannot buy speed. `kProtocol` is 3;
the two fields are the same width, so an unversioned peer would silently
decode WASD as a direction and walk off at a nonsense angle.

### 16.6 LOD rings are live and measured (done)

`reduceToLevel` was built and unit-tested long before anything called it, which
meant the bandwidth saving was real in a test and imaginary in the game. The
server stream path now picks a leaf level from the brick's ring distance and
serialises *that*, so the reduction happens before the frame is built rather
than after.

`src/simulation/lod_rings.cpp` maps Chebyshev ring distance to a leaf level,
chosen against the measured curve in §16.1 rather than the estimates in §5.5:

| band | rings | leaf level | nodes/brick |
|---|---|---|---|
| full | 0–2 | 7 | ~36 000 |
| near | 3–6 | 4 | ~160 |
| outer | 7–14 | 3 | — |
| far | 15–30 | 2 | — |
| scenery | 30+ | 2 | — |

The client is already ring-aware in one respect — it evicts on the same
Chebyshev window the server streams within — so the two never disagree about
which bricks are near, and no protocol change was needed to make the server's
choice visible: `BrickSnapshot.lodLevel` was already on the wire and is now
honoured, and `BrickUpdate` carries it through so the renderer can see it.

Measured on a 64×64-brick window, walking outward (`tests/lod_rings_test.cpp`):

```
with LOD   : 354 bricks,  19.0 MB,   36 146 nodes/brick full, 160 reduced
without LOD: 354 bricks, 100.2 MB,   all 354 at full resolution, 0 reduced
```

**A 226× reduction in nodes per brick, 5.3× less data over the same window.**
The test pins both the ring mapping and this end-to-end figure, and was verified
to fail when the mapping is disabled (4 of 5 assertions, plus the reported
byte count).

### 16.7 The reduction was destroying the terrain (fixed)

Wiring the rings up exposed the bug that was reported: terrain past ~300 blocks
rendering as large stone cubes. Two defects in `reduceToLevel`, both confirmed by
measurement rather than inspection.

**Defect 1: the descent never stopped.** `leafLevel` had no effect at all --
every request returned the full-resolution tree (74,153 nodes per brick,
identical at levels 2 through 7). The walk ran to level 0 because a mixed box
was told to "keep structure" with no stop condition.

**Defect 2: the collapse rule.** "Solid if any child is solid" is a 1-of-8
threshold. It turned a 128^3 brick into **7 nodes** and filled carved-out air
with stone in 32^3 blocks. That is the reported symptom exactly.

Fixing both gives a real reduction, and the error is now *measured* rather than
assumed. On the default world, reducing every brick and classifying each
differing cell by what kind of error it is:

| threshold | nodes | cubes (sky) | holes (ground) | recolor |
|---|---|---|---|---|
| agree-only | 4,632,957 | 0 | 0 | 0 | *(no reduction)* |
| 8 | 1,791,341 | 5,194 | 0 | 3,548,951 |
| **7 (shipped)** | **1,745,663** | **5,194** | **0** | **3,549,701** |
| 5 | 1,610,782 | 35,665 | 0 | 3,722,553 |
| *old rule* | *578* | *664,117* | *21,836,319* | -- |

Three things that table settles:

* **Holes are free.** No threshold produces a single solid->air cell. The old
  rule's 21.8M "holes" were almost all *recolor* -- the collapse wrote the
  wrong material and the measurement counted any difference as a hole. Recolor
  is a thin wrong-coloured band at a stratum boundary; a hole is a missing
  hillside, and only the latter is visible from any distance.
* **Cubes are the price of reduction, and they are cheap.** 5,194 cells --
  four orders of magnitude better than the old rule's 664,117.
* **The material vote matters.** Picking the *first* solid child's material
  recoloured lava<->stone across strata. Picking the *majority* material is the
  difference between a right and a wrong colour on 3.5M cells.

The rule is deliberately biased: a box becomes solid when it is almost solid
(7 of 8 children), but **air is never produced by a vote** -- only by the exact
`agree` rule when every child is already an air leaf. A slightly-too-solid
hillside is far easier to miss than a missing one.

## 18. Phase 2: the bounded arena, and streaming the whole world

Budgets this phase is built to: **3 GiB of GPU memory for world data on the
client** (402,653,184 SVO nodes) and **8 GiB of RAM for the world on the
server**. Nothing else is charged to either; geometry, textures and
framebuffers are outside these numbers.

### 18.1 The arena (`src/simulation/brick_pool.{hpp,cpp}`)

A fixed-capacity arena with LRU eviction, deliberately free of any Vulkan type
so the policy can be tested. Three defects it fixes, each measured:

| Defect | Symptom |
|---|---|
| Runs were never trimmed | A brick streamed at full resolution (~36k nodes) kept that reservation after its LOD dropped to ~300 nodes. Walking outward turned every near brick into one of these. The arena filled with held-but-unused runs while wanted bricks were refused. **This, not world size, is what made storage unbounded.** |
| No eviction at all | On exhaustion the brick was dropped and retried forever, so terrain simply never appeared. |
| Stale recency index | A re-claimed brick left its old entry in the index, so the eviction order was a lie and the brick under the player's feet was thrown away first. |

Trimming in place (release the tail, keep the offset) is what makes the first
one cheap: one free-range split, no move, no fragmentation.

### 18.2 Eviction must be reported, or it is permanent

`RenderState::evictedRoots` carries what the renderer dropped. The client
clears those from its resident set and sends **`WorldForget`** (protocol 4); the
server erases its delivery record for that root.

Without it the two sides disagree permanently: the server still believes the
brick was delivered, so it skips every future send, and nothing ever brings it
back. `VoxelLruTests` pins this by asserting the stream stays busy — a broken
retraction drops uploads from ~2,950 to 246 and fails loudly.

### 18.3 The arena is larger than the window, so the world streams in full

With 3 GiB against a world that costs 37 MB full-resolution, the old rule — evict
every brick that leaves the interest window — made the cache exactly one window
deep and wasted the budget. Retention is now the arena's decision, by capacity.
The window only sets **priority**: `BrickPool::setWindow` makes eviction take
bricks outside the window before bricks inside it, so filling the arena with the
rest of the world never costs the player the ground they are standing on.

The server now sends in two tiers — the window nearest-first, then a background
sweep of the whole world from a cursor — under a **token-bucket bandwidth
ceiling** (`--bw`, default 32 MiB/s, one second of burst credit).

Measured on a 128-brick world at a 400 KiB/s ceiling: 100% of the world
delivered, spread over ~3,600 client iterations, sustaining 441 KiB/s. Disabling
the pacer collapses that to a single burst of 4 iterations
(`VoxelStreamAllTests`).

### 18.4 What is left

* `--pool` defaults to the full 3 GiB; the renderer still allocates the arena
  eagerly, so a client that never needs more than the window pays for the whole
  budget up front. Lazy growth to the working set is the obvious next step.
* Server world generation above 4096 root bricks is lazy, but the 8 GiB budget
  is only reported as a worst-case bound at startup, not enforced.
* No Tracy integration. The built-in counters (pool used/capacity, resident
  bricks, eviction total, server stream rate) were sufficient to find every
  defect above and avoid a dependency; a proper sampling profiler is still the
  right tool for frame-level work.

## 19. Invariants this design now enforces

These are the rules the code is built around. Each has a test.

1. **One generator.** `World::generate(rootIndex)` builds the real world at
   full resolution. There is no shallower walk. Coarser levels are rewrites of
   that result, and needing a finer level is the same operation in reverse:
   regenerate from the generator plus the change tree, then reduce again.
   *(The voxel SVO is a cache in both directions; the seed and the change tree
   are what make that safe.)*

2. **Two trees, two jobs.** The voxel SVO holds material ids and nothing else.
   The change tree holds only what a player altered, and nothing else — it is
   absent for any cell the generator already produces. Measured: **7.6 MB of
   voxels against 30 KB of changes for 1000 edits (0.4%)**.

3. **Eviction is lossless.** Because the change tree is the only durable
   record, throwing away a resident voxel brick cannot lose anything.

4. **No LOD hole.** A coarse box is solid whenever *any* of its children is.
   A majority vote would punch holes in distant hillsides — you cannot see that
   terrain is slightly too solid, but you can see through a cliff.
   *(This replaced §5.2's "conservative max-top", which could not express the
   rule for a general box.)*

5. **The change flag is load-bearing.** It distinguishes "left at its default"
   from "set back to the default on purpose", which is what lets pruning delete
   an undone edit and regeneration rebuild exactly.

6. **The rendered SVO is the simulated SVO.** There is one world, and physics
   reads it through `World::isSolid` — the same `generate()` the GPU is handed.
   A generation bug therefore shows up in the render *and* in collision
   identically, which is what makes it diagnosable.

7. **Generation is exact, not conservative.** Every cell matches
   `terrainMaterial()` at 1024×1024×256. The collapse rule may only shortcut a
   box once the height mipmap has *proved* the box homogeneous (`low == high`),
   never on the assumption that it is. *(This replaced a rule that read the
   centre column and hoped — see §16.3.)*

8. **A test that cannot fail is worse than no test.** The build must not define
   `NDEBUG` for test targets; a suite that silently asserts nothing reports
   "Passed" forever. See §16.4.

9. **The authority owns outcomes, the client owns intent.** `moveDirection` is
   a direction of travel, not a keypress and not a position: the client says
   where it wants to go, the server decides how far it gets. Anything else
   either duplicates the world on both sides or lets one side decide the other
   can move.

*End of plan.*
