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

#endif  // VOXEL_SVO_SLANG
