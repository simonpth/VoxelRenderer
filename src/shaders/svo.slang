// ---------------------------------------------------------------------------
// SVO layout constants and traversal helpers.
//
// Mirrors `src/simulation/svo_node.hpp`. The node layout is:
//
//   word0 (u32): bits  0..23  firstChild  index of the first stored child
//                        24..31  flags      kSvoFlagSolid | kSvoFlagResident
//   word1 (u16): bits  0..15  material     16-bit material id
//          (u8): bits 16..23  mask         bit i => child i is stored
//          (u8): bits 24..31  lodLevel     log2 of the node edge in cells
//
// A node's children are contiguous and stored in mask order, so child i lives
// at `firstChild + popcount(mask & ((1 << i) - 1))`.
// ---------------------------------------------------------------------------

#ifndef VOXEL_SVO_SLANG
#define VOXEL_SVO_SLANG

static const uint kSvoFlagSolid = 1u;
static const uint kSvoFlagResident = 1u << 1u;

// Material table stride: a textureless uint2, .x is packed RGBA8,
// .y is the server-provided flag bits.
static const uint kMaterialEmissive = 2u;

// Rendered when a ray reaches a brick the client has not streamed yet.
static const float3 kUnknownColor = float3(0.55f, 0.0f, 0.55f);
// Sentinel material meaning "this ray never resolved because data is missing",
// as distinct from material 0, which means "resolved, and it is air". Every
// consumer must distinguish the two: treating unknown as air turns a loading
// gap into a hole you can see through.
static const uint kUnknownMaterial = 0xFFFFu;
// Sentinel material for an averaged-grid hit. A grid cell carries its own
// colour, so the caller shades it directly instead of looking a material up --
// and it must never be read as either of the two sentinels above.
static const uint kAveragedMaterial = 0xFFFEu;
static const float3 kSkyZenith = float3(0.06f, 0.22f, 0.55f);
static const float3 kSkyHorizon = float3(0.42f, 0.68f, 0.95f);

// Traversal stack. A 128-cell brick is seven levels deep and each level pushes
// at most eight children, so 56 slots can never overflow.
static const int kStackSize = 56;

// Inverse ray direction with a safe stand-in for zero components, so a
// degenerate axis never produces a NaN in the slab test.
float3 safeInverse(float3 direction) {
    const float big = 1.0e30f;
    return float3(direction.x == 0.0f ? (direction.x < 0.0f ? -big : big)
                                      : 1.0f / direction.x,
                  direction.y == 0.0f ? (direction.y < 0.0f ? -big : big)
                                      : 1.0f / direction.y,
                  direction.z == 0.0f ? (direction.z < 0.0f ? -big : big)
                                      : 1.0f / direction.z);
}

// Slab test against a world-space axis aligned box. Writes the entry and exit
// distances; the ray overlaps the box when `entry <= exit && exit >= 0`.
bool intersectBox(float3 origin, float3 inverseDirection, float3 lo, float3 hi,
                  out float entry, out float exit) {
    const float3 a = (lo - origin) * inverseDirection;
    const float3 b = (hi - origin) * inverseDirection;
    const float3 low = min(a, b);
    const float3 high = max(a, b);
    entry = max(max(low.x, low.y), low.z);
    exit = min(min(high.x, high.y), high.z);
    return entry <= exit && exit >= 0.0f;
}

// Face normal of the box the ray entered at `distance`.
float3 boxNormal(float3 origin, float3 direction, float3 lo, float edge,
                 float distance) {
    const float3 point = origin + direction * distance;
    const float3 delta = point - lo;
    const float3 remaining = edge - delta;
    float3 n = float3(0.0f, 0.0f, 1.0f);
    float best = 3.4e38f;
    if (delta.x < best) { best = delta.x; n = float3(-1.0f, 0.0f, 0.0f); }
    if (delta.y < best) { best = delta.y; n = float3(0.0f, -1.0f, 0.0f); }
    if (delta.z < best) { best = delta.z; n = float3(0.0f, 0.0f, -1.0f); }
    if (remaining.x < best) { best = remaining.x; n = float3(1.0f, 0.0f, 0.0f); }
    if (remaining.y < best) { best = remaining.y; n = float3(0.0f, 1.0f, 0.0f); }
    if (remaining.z < best) { best = remaining.z; n = float3(0.0f, 0.0f, 1.0f); }
    return n;
}

// Number of stored children that precede child `i` in their parent's block.
uint childOffset(uint mask, uint i) {
    return (uint)countbits(mask & ((1u << (i & 7u)) - 1u));
}

float3 boxMinimum(float3 parentLo, float half, uint index) {
    return parentLo + half * float3((float)(index & 1u),
                                    (float)((index >> 1u) & 1u),
                                    (float)((index >> 2u) & 1u));
}

// ---------------------------------------------------------------------------
// Pyramid coarse grids.
//
// Mirrors the renderer's root-table tail and GridLevel packing (see
// VulkanContext: the header comment on m_rootTable, and packCell). One packed
// cell is a uint: R,G,B in bits 24,16,8, solid flag in bit 0. There is no
// alpha anywhere -- an empty cell is air and the ray walks through it, exactly
// as it walks past an empty octree node.
//
// A root entry's z word carries bit L for "pyramid level L is on the GPU"
// (bit 0 mirrors kSvoFlagResident, so one word tests "anything usable") and
// three bits of log2 grid edge per level 1..7. Log 0 is the single cell, which
// is both a real 1-edge grid and the wire fast path: the two are the same
// size on the GPU, so nothing here distinguishes them. The w word is the
// brick's grid-pool base in uints; levels sit consecutively in level order,
// so a level's offset is the base plus the sizes of the resident finer levels
// -- at most seven adds.
// ---------------------------------------------------------------------------

// Solid flag of one packed averaged cell.
bool gridCellSolid(uint packed) {
    return (packed & 1u) != 0u;
}

// Unpacks to linear 0..1. The bytes are sRGB material colours, sampled flat:
// coarse levels carry no lighting detail of their own, so the caller shades
// the face normal directly rather than looking anything up.
float3 gridCellColor(uint packed) {
    return float3((float)((packed >> 24) & 0xFFu) / 255.0f,
                  (float)((packed >> 16) & 0xFFu) / 255.0f,
                  (float)((packed >> 8) & 0xFFu) / 255.0f);
}

// Bit L of the mask: pyramid level L resident on the GPU.
uint gridLevelMask(uint4 entry) {
    return entry.z & 0xFFu;
}

// Log2 grid edge for levels 1..7 (0 = single cell).
uint gridEdgeLog(uint4 entry, uint level) {
    return (entry.z >> (8u + 3u * (level - 1u))) & 0x7u;
}

// Cells of level `level`: (1 << edgeLog)^3. In-bounds by construction: every
// edge log in the table was written from a grid whose edge^3 was validated
// against its cell count on upload, and levels are laid out consecutively in
// the order the offsets sum -- so a computed offset always lands inside the
// brick's own run. The shader trusts the table the way it trusts the root
// flags: both are CPU-authored, and everything the wire influences is checked
// before it is written.
uint gridLevelCells(uint4 entry, uint level) {
    const uint edge = 1u << gridEdgeLog(entry, level);
    return edge * edge * edge;
}

// Pool offset of `level` within the brick's run: base plus the sizes of the
// resident finer levels. Skips cleared bits, so an evicted level's hole is
// never addressed -- the bit is cleared in the same breath as the host entry
// goes, which is what keeps "bit set" meaning "the data here is this level".
uint gridLevelOffset(uint4 entry, uint level) {
    uint offset = entry.w;
    const uint mask = gridLevelMask(entry);
    for (uint l = 1u; l < level; ++l) {
        if ((mask & (1u << l)) != 0u)
            offset += gridLevelCells(entry, l);
    }
    return offset;
}

// Which pyramid level to sample for a brick entered at `entryDist.
//
// Cells cover roughly wantCell world cells per pixel, where wantCell grows
// with distance; the level whose cells match it is round(log2(wantCell)).
// Level 0 is the exact octree and always correct -- it is just slow -- so the
// desired level names the coarsest approximation that stays invisible, and the
// selection below falls back to exact whenever nothing coarser is resident.
// Close up the answer is 0 (subpixel footprint: only exact geometry resolves);
// a brick smaller than a pixel wants the top (one cell for the whole brick).
uint desiredGridLevel(float entryDist, uint brickCells) {
    // Pixels per cell at 952 cells per... inverted: cells per pixel. The 952
    // is the existing split constant (a brick splits past 952 * cellSize), so
    // a level whose cells are ~952x smaller than the distance is ~1px cells.
    const float footprint = max(entryDist, 1e-3f) / 952.0f;
    const float exact = log2(max(footprint, 1e-3f));
    if (exact < 0.0f) return 0u;
    if (exact > 7.0f) return 7u;
    return (uint)(exact + 0.5f);
}

// Finest resident level at or coarser than desired; if every resident level is
// finer than desired, the coarsest resident one. Finer-than-desired data is
// always correct, just slower to sample -- so near geometry with only the
// octree walks exact, and far scenery with only grids samples them, and the
// middle case (both resident) takes the grid, which is the whole performance
// point: a grid sample is one DDA where the octree walk is hundreds of nodes.
uint selectGridLevel(uint4 entry, uint desired) {
    const uint mask = gridLevelMask(entry);
    uint coarsest = 0u;
    uint finestCoarseEnough = 99u;
    for (uint l = 0u; l < 8u; ++l) {
        if ((mask & (1u << l)) == 0u) continue;
        coarsest = l;
        if (l >= desired && l < finestCoarseEnough) finestCoarseEnough = l;
    }
    return finestCoarseEnough != 99u ? finestCoarseEnough : coarsest;
}

#endif  // VOXEL_SVO_SLANG
