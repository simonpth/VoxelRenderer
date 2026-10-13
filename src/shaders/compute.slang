// Fullscreen ray trace over the sparse voxel octree.
//
// Bindings:
//   0  storage image  compute output
//   1  sampled image  the same image, read by the blit
//   2  root table     one uint4 per root-layer brick (octree root + grid directory)
//   3  node pool      every streamed node of every resident brick
//   4  material table uint2 per material id
//   5  grid pool      flat packed averaged cells for resident pyramid levels
//
// The traversal never trusts a CPU-supplied index: every pool load is bounds
// checked, the stack is a fixed size, and the iteration count is capped, so
// malformed data cannot hang the GPU. Grid offsets are in-bounds by
// construction (see gridLevelOffset): every edge in the table was validated
// against its cell count before upload, so the sums always land inside the
// brick's own run.

#include "svo.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> pixelOutput;
[[vk::binding(1, 0)]] Texture2D<float4> sampledOutput;
[[vk::binding(2, 0)]] StructuredBuffer<uint4> rootTable;
[[vk::binding(3, 0)]] StructuredBuffer<uint2> nodePool;
[[vk::binding(4, 0)]] StructuredBuffer<uint2> materialTable;
[[vk::binding(5, 0)]] StructuredBuffer<uint> gridPool;

struct PushConstants {
    uint width;
    uint height;
    float time;
    float deltaTime;
    float4 eye;
    float4 forward;
    uint sizeX;
    uint sizeY;
    uint sizeZ;
    uint brick;
    uint dimX;
    uint dimY;
    uint dimZ;
    uint nodeCount;
    uint materialCount;
    uint maxRayDistance;
    float tanHalfFovX;
    float tanHalfFovY;
    float4 sunDirection;
    // Interest window, in brick coordinates. See expectedByClient().
    uint interestX;
    uint interestY;
    uint interestZ;
    uint interestRadius;
};

[[vk::push_constant]] PushConstants pc;

// Terminal node state for the currently allocated brick.
struct Walker {
    uint node[kStackSize];       // pool index
    float4 box[kStackSize];      // xyz = box minimum, w = edge length
};

// Traverses one brick octree between `tStart` and `tLimit`.
// With `anyHit` set the first terminal solid node wins, which is all a shadow
// ray needs. Otherwise the nearest hit is returned.
bool traverseBrick(float3 origin, float3 inverseDirection, float3 dir,
                   uint2 rootPacked, float3 brickLo, float brickEdge,
                   float tStart, float tLimit, bool anyHit,
                   out float hitDistance, out float3 hitNormal,
                   out uint hitMaterial) {
    Walker walker;
    int sp = 0;
    int guard = 0;

    uint packed0 = rootPacked.x;
    uint packed1 = rootPacked.y;
    float3 lo = brickLo;
    float edge = brickEdge;
    float entry = tStart;

    float best = tLimit;
    bool found = false;
    hitDistance = 0.0f;
    hitNormal = float3(0.0f, 0.0f, 1.0f);
    hitMaterial = 0;

    bool haveNode = true;
    while (haveNode) {
        if (++guard > 192) break;

        // --- visit the current node ---
        const uint mask = (packed1 >> 16) & 0xFFu;
        if (mask == 0u) {
            if (((packed0 >> 24) & kSvoFlagSolid) != 0u && entry <= best) {
                best = entry;
                hitDistance = entry;
                hitMaterial = packed1 & 0xFFFFu;
                hitNormal = boxNormal(origin, dir, lo, edge, entry);
                found = true;
                if (anyHit) return true;
            }
        } else {
            const uint first = packed0 & 0x00FFFFFFu;
            const float half = edge * 0.5f;
            float childNear[8];
            uint childIndex[8];
            uint childCount = 0;
            for (uint i = 0; i < 8; ++i) {
                if ((mask & (1u << i)) == 0u) continue;
                const uint index = first + childOffset(mask, i);
                if (index >= pc.nodeCount) continue;
                const float3 childLo = boxMinimum(lo, half, i);
                float childEntry, childExit;
                if (!intersectBox(origin, inverseDirection, childLo,
                                  childLo + half, childEntry, childExit))
                    continue;
                childEntry = max(childEntry, entry);
                if (childEntry > best) continue;
                // Insertion sort, descending, so the nearest child ends up on
                // top of the stack.
                uint slot = childCount++;
                while (slot > 0 && childNear[slot - 1] < childEntry) {
                    childNear[slot] = childNear[slot - 1];
                    childIndex[slot] = childIndex[slot - 1];
                    --slot;
                }
                childNear[slot] = childEntry;
                childIndex[slot] = i;
            }
            for (uint k = 0; k < childCount && sp < kStackSize; ++k) {
                const uint i = childIndex[k];
                const uint index = first + childOffset(mask, i);
                const float3 childLo = boxMinimum(lo, half, i);
                walker.node[sp] = index;
                walker.box[sp] = float4(childLo, half);
                ++sp;
            }
        }

        // --- pop the next candidate ---
        haveNode = false;
        while (sp > 0) {
            --sp;
            const float3 candidateLo = walker.box[sp].xyz;
            const float candidateEdge = walker.box[sp].w;
            float candidateEntry, candidateExit;
            if (!intersectBox(origin, inverseDirection, candidateLo,
                              candidateLo + candidateEdge, candidateEntry,
                              candidateExit))
                continue;
            if (candidateEntry > best) continue;
            const uint2 packed = nodePool[walker.node[sp]];
            packed0 = packed.x;
            packed1 = packed.y;
            lo = candidateLo;
            edge = candidateEdge;
            entry = candidateEntry;
            haveNode = true;
            break;
        }
    }
    return found;
}

// Samples one averaged grid between `tStart` and `tLimit` with a 3D DDA at
// the level's cell size. Empty cells are air and the ray walks through them --
// there is no alpha, so nothing blends and nothing sorts. With `anyHit` the
// first solid cell wins (a shadow ray's whole question); otherwise the nearest
// hit is returned, mirroring traverseBrick's contract so the caller dispatches
// without caring which representation answered.
//
// Bounded by construction: a level edge is at most 128, so the walk crosses
// at most 3*edge cell boundaries before leaving the brick, and the cap below
// is past that for every level.
bool traverseGrid(float3 origin, float3 inverseDirection, float3 dir,
                  uint4 rootEntry, uint level, float3 brickLo, float brickEdge,
                  float tStart, float tLimit, bool anyHit,
                  out float hitDistance, out float3 hitNormal,
                  out float3 hitColor) {
    hitDistance = 0.0f;
    hitNormal = float3(0.0f, 0.0f, 1.0f);
    hitColor = float3(0.0f, 0.0f, 0.0f);

    const uint edgeLog = gridEdgeLog(rootEntry, level);
    const float cell = (float)(1u << edgeLog);
    const uint edge = 1u << edgeLog;
    const uint base = gridLevelOffset(rootEntry, level);

    // Grid index of the entry point, clamped: tStart is already inside the
    // brick, so clamping only absorbs float error at the far wall.
    const float3 rel = (origin + dir * tStart - brickLo) / cell;
    int3 g = int3((int)floor(rel.x), (int)floor(rel.y), (int)floor(rel.z));
    g = int3(clamp(g.x, 0, (int)edge - 1), clamp(g.y, 0, (int)edge - 1),
             clamp(g.z, 0, (int)edge - 1));

    const int3 step = int3(dir.x < 0.0f ? -1 : 1, dir.y < 0.0f ? -1 : 1,
                           dir.z < 0.0f ? -1 : 1);
    // Distance along the ray to the next boundary per axis, from the entry.
    // The +1/0 per axis picks the max or min wall of the current cell.
    const float3 wall =
        brickLo + cell * (float3(g) + float3(max(step, int3(0, 0, 0))));
    float3 tMax = (wall - origin) * inverseDirection;
    const float3 tDelta = abs(cell * inverseDirection);

    float t = tStart;
    int axis = -1;
    for (int i = 0; i < 388; ++i) {
        const uint packed =
            gridPool[base + (uint)(g.x + (int)edge * (g.y + (int)edge * g.z))];
        if (gridCellSolid(packed)) {
            hitDistance = t;
            hitColor = gridCellColor(packed);
            hitNormal = axis < 0 ? boxNormal(origin, dir, brickLo, brickEdge, t)
                                 : -float3((float)(axis == 0 ? step.x : 0),
                                           (float)(axis == 1 ? step.y : 0),
                                           (float)(axis == 2 ? step.z : 0));
            return true;
        }
        // Advance past the nearest wall. Ties step x first; any consistent
        // order resolves the same cells, and a tie is a shared edge either way.
        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            axis = 0;
            g.x += step.x;
            t = tMax.x;
            tMax.x += tDelta.x;
        } else if (tMax.y <= tMax.z) {
            axis = 1;
            g.y += step.y;
            t = tMax.y;
            tMax.y += tDelta.y;
        } else {
            axis = 2;
            g.z += step.z;
            t = tMax.z;
            tMax.z += tDelta.z;
        }
        if (t > tLimit) return false;
        if (g.x < 0 || g.y < 0 || g.z < 0 || g.x >= (int)edge ||
            g.y >= (int)edge || g.z >= (int)edge)
            return false;
    }
    return false;
}

float4 unpackRgba8(uint packed) {
    return float4((float)((packed >> 24) & 0xFFu) / 255.0f,
                  (float)((packed >> 16) & 0xFFu) / 255.0f,
                  (float)((packed >> 8) & 0xFFu) / 255.0f,
                  (float)(packed & 0xFFu) / 255.0f);
}

// Did the client ever promise to hold this brick?
//
// This is the distinction the whole "missing brick" rendering hangs on. The
// client runs a bounded arena on a world far larger than it, so the great
// majority of the root layer is not resident and never will be. Reporting
// *every* non-resident brick as missing is what filled the sky with purple
// cubes: on a 128-brick world a missing brick was a real gap, but on a
// 16,777,216-brick one every ray into open sky left the window almost
// immediately and hit a brick the client had never been asked for.
//
// So "missing" has to mean "inside the window and not delivered yet" -- a hole
// that is filling in. Outside the window the client makes no promise, and a
// brick it does not hold is simply empty space as far as it can tell.
bool expectedByClient(int3 brickCell) {
    const int radius = (int)pc.interestRadius;
    if (radius <= 0) return false;
    const int3 centre = int3((int)pc.interestX, (int)pc.interestY,
                             (int)pc.interestZ);
    return all(abs(brickCell - centre) <= int3(radius, radius, radius));
}

// Walks the root layer along a ray, brick by brick.
//
// A ray whose origin sits exactly on a lattice plane and whose direction
// component along that plane's normal is tiny produces a degenerate,
// zero-length slab overlap. A fixed distance epsilon would then be below
// float32 precision at these coordinates, so the walk also advances the cell
// coordinate, which always lands in a genuinely different voxel.
bool walkBricks(float3 origin, float3 dir, float rayLimit, bool anyHit,
                out float hitDistance, out float3 hitNormal,
                out uint hitMaterial, out float3 hitColor) {
    // Cleared up front, because not every early return sets them: a caller
    // that inspects the outputs after a false result -- raycast does, to tell
    // "hit air" from "ran out of data" -- would otherwise read uninitialised
    // memory. A miss with material 0 means the ray genuinely reached nothing.
    hitDistance = 0.0f;
    hitNormal = float3(0.0f, 0.0f, 1.0f);
    hitMaterial = 0u;
    hitColor = float3(0.0f, 0.0f, 0.0f);

    const float3 inverseDirection = safeInverse(dir);
    const float3 worldLo = float3(0.0f, 0.0f, 0.0f);
    const float3 worldHi = float3((float)pc.sizeX, (float)pc.sizeY,
                                  (float)pc.sizeZ);
    float tNear, tFar;
    if (!intersectBox(origin, inverseDirection, worldLo, worldHi, tNear, tFar))
        return false;
    tNear = max(tNear, 0.0f);
    tFar = min(tFar, rayLimit);
    if (tFar <= tNear) return false;

    const int brick = (int)pc.brick;
    const int dimX = (int)pc.dimX;
    const int dimY = (int)pc.dimY;
    const int dimZ = (int)pc.dimZ;
    const float brickEdge = (float)pc.brick;

    const int3 worldMax = int3((int)pc.sizeX - 1, (int)pc.sizeY - 1,
                               (int)pc.sizeZ - 1);
    int3 cell = (int3)floor(origin + dir * tNear);
    cell = int3(clamp(cell.x, 0, worldMax.x), clamp(cell.y, 0, worldMax.y),
                clamp(cell.z, 0, worldMax.z));
    const int3 step = int3(dir.x < 0.0f ? -1 : 1, dir.y < 0.0f ? -1 : 1,
                           dir.z < 0.0f ? -1 : 1);

    float distance = tNear;
    // Bounded so malformed data cannot hang the GPU, but high enough to cross
    // the world. A ray travelling the full 65536-cell width steps through 512
    // bricks, and a diagonal one through more than that, so the old cap of 512
    // stopped rays at roughly half a world and they ended in open sky no matter
    // what was there.
    for (int iteration = 0; iteration < 2048; ++iteration) {
        const int3 brickCell =
            int3(cell.x / brick, cell.y / brick, cell.z / brick);
        const uint rootIndex =
            (uint)(brickCell.x + dimX * (brickCell.y + dimY * brickCell.z));
        if (rootIndex >= (uint)(dimX * dimY * dimZ)) return false;
        const uint4 rootPacked = rootTable[rootIndex];
        const bool resident = ((rootPacked.x >> 24) & kSvoFlagResident) != 0u;
        // Usable means any resident level, not just the octree: a brick whose
        // detail brick was evicted but whose grids stayed still renders, from
        // the grids. Unknown data is never silently treated as empty space
        // *where the client owes the viewer data*: inside the window a brick
        // with nothing usable is a hole still filling in, and the caller
        // renders it as missing. Outside it, the client was never asked, so
        // the ray passes through.
        const bool usable = resident || (gridLevelMask(rootPacked) & 0xFEu) != 0u;
        if (!usable && expectedByClient(brickCell)) {
            hitMaterial = kUnknownMaterial;
            return false;
        }

        const float3 brickLo = float3((float)(brickCell.x * brick),
                                      (float)(brickCell.y * brick),
                                      (float)(brickCell.z * brick));
        float brickNear, brickFar;
        const bool overlaps =
            intersectBox(origin, inverseDirection, brickLo,
                         brickLo + brickEdge, brickNear, brickFar);
        // The box is still intersected geometrically when the brick is not
        // resident, so an unheld brick is stepped over in one move rather than
        // a cell at a time. That keeps a sky ray crossing hundreds of empty
        // bricks inside the iteration cap instead of exhausting it.
        if (overlaps && usable) {
            const float entry = max(distance, brickNear);
            const float limit = min(brickFar, tFar);
            // Usable guarantees an answer: either the mask is non-zero and
            // selection picks among resident levels, or only the octree is
            // resident and selection falls through to level 0. Level 0 is the
            // exact octree; anything above is an averaged grid at this brick's
            // own scale.
            const uint level =
                selectGridLevel(rootPacked, desiredGridLevel(entry, (uint)brick));
            if (level == 0u) {
                if (traverseBrick(origin, inverseDirection, dir,
                                  rootPacked.xy, brickLo, brickEdge, entry,
                                  limit, anyHit, hitDistance, hitNormal,
                                  hitMaterial))
                    return true;
            } else {
                float gridDist;
                float3 gridNormal;
                float3 gridColor;
                if (traverseGrid(origin, inverseDirection, dir, rootPacked,
                                 level, brickLo, brickEdge, entry, limit,
                                 anyHit, gridDist, gridNormal, gridColor)) {
                    // Nearest-hit walk: bricks arrive in ray order and the
                    // grid sample is nearest within this one, so the first
                    // brick that answers wins -- same contract as the octree.
                    // (For anyHit the first solid cell is the whole answer.)
                    hitDistance = gridDist;
                    hitNormal = gridNormal;
                    hitMaterial = kAveragedMaterial;
                    hitColor = gridColor;
                    return true;
                }
            }
        }

        const float nextDistance =
            overlaps ? min(brickFar, tFar) : distance + 1e-3f;
        if (nextDistance <= distance) {
            cell = int3(clamp(cell.x + step.x, 0, worldMax.x),
                        clamp(cell.y + step.y, 0, worldMax.y),
                        clamp(cell.z + step.z, 0, worldMax.z));
            distance += 1e-3f;
        } else {
            distance = nextDistance;
            cell = (int3)floor(origin + dir * (distance + 1e-3f));
            cell = int3(clamp(cell.x, 0, worldMax.x),
                        clamp(cell.y, 0, worldMax.y),
                        clamp(cell.z, 0, worldMax.z));
        }
        if (distance >= tFar) return false;
    }
    return false;
}

// Shadow test. Only "did anything block this" matters, so the first terminal
// solid node wins and no shading is done.
//
// A missing brick counts as *blocking*. walkBricks reports it by returning
// false with hitMaterial set to kUnknown, which is the same signal raycast
// reads -- and raycast correctly renders it as missing rather than as air. The
// shadow path used to return that false straight through, so a shadow ray that
// merely clipped an unloaded brick reported "unblocked" and the surface lit up
// as though the sun were behind the camera. The visible result was shadows
// pointing away from the light, or missing altogether wherever streaming was
// still in progress.
//
// Treating unknown as blocked is the conservative direction and the one
// consistent with the rest of the shader: you cannot see that a shadow is
// slightly too dark, but you very much can see a false highlight.
bool occluded(float3 origin, float3 dir) {
    float hitDistance;
    float3 hitNormal;
    uint hitMaterial;
    // A shadow ray needs to escape the terrain, not to cross the world. Giving
    // it the primary ray's distance means every lit surface walks hundreds of
    // empty bricks looking for something that is not between it and the sun.
    const float shadowDistance = min((float)pc.maxRayDistance, 8192.0f);
    float3 shadowColor;
    const bool hit = walkBricks(origin, dir, shadowDistance, true,
                                hitDistance, hitNormal, hitMaterial,
                                shadowColor);
    // `hitMaterial` is kUnknownMaterial exactly when the walk stopped because a
    // brick had not been streamed. Both cases block.
    return hit || hitMaterial == kUnknownMaterial;
}

// Primary ray: the nearest terminal solid node, shaded by the material table.
bool raycast(float3 origin, float3 dir, float rayLimit, out float3 color,
             out float3 normal, out uint material) {
    float hitDistance;
    float3 hitNormal;
    float3 hitColor;
    uint hitMaterial = 0;
    // Set before any early return, so a caller that inspectes the outputs
    // after a miss reads defined values rather than whatever was in the
    // registers. The miss path returns without touching color/normal/material.
    color = kUnknownColor;
    normal = float3(0.0f, 0.0f, 1.0f);
    material = kUnknownMaterial;
    if (!walkBricks(origin, dir, rayLimit, false, hitDistance, hitNormal,
                    hitMaterial, hitColor)) {
        if (hitMaterial != kUnknownMaterial) return false;
        // Missing brick: show it as unknown data, not as empty space.
        color = kUnknownColor;
        normal = float3(0.0f, 0.0f, 1.0f);
        material = kUnknownMaterial;
        return true;
    }

    material = hitMaterial;
    normal = hitNormal;
    if (hitMaterial == kAveragedMaterial) {
        // A coarse hit shades from its own averaged colour: no material
        // lookup (levels above 0 carry no material ids), no emissive branch.
        // Same diffuse model as the octree path, so a brick reads as one
        // surface whether the ray resolved it exact or averaged.
        color = hitColor;
        const float3 toSun = pc.sunDirection.xyz;
        const float3 hitPoint = origin + dir * hitDistance + hitNormal * 1e-3f;
        color *= occluded(hitPoint, toSun)
                     ? 0.25f
                     : 0.28f + 0.72f * max(dot(hitNormal, toSun), 0.0f);
        return true;
    }
    color = float3(0.5f, 0.5f, 0.5f);
    if (hitMaterial < pc.materialCount) {
        const uint2 entry = materialTable[hitMaterial];
        color = unpackRgba8(entry.x).rgb;
        // Emissive materials ignore direct lighting entirely.
        if ((entry.y & kMaterialEmissive) != 0u) return true;
    }

    const float3 toSun = pc.sunDirection.xyz;
    const float3 hitPoint = origin + dir * hitDistance + hitNormal * 1e-3f;
    color *= occluded(hitPoint, toSun)
                 ? 0.25f
                 : 0.28f + 0.72f * max(dot(hitNormal, toSun), 0.0f);
    return true;
}

float4 skyColor(float3 direction) {
    const float horizon = saturate(direction.z * 0.5f + 0.5f);
    return float4(lerp(kSkyHorizon, kSkyZenith, horizon), 1.0f);
}

[numthreads(8, 8, 1)]
[[shader("compute")]]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;

    const float2 pixel =
        (float2(id.xy) + 0.5f) / float2((float)pc.width, (float)pc.height);
    const float2 screen = float2(pixel.x * 2.0f - 1.0f, 1.0f - pixel.y * 2.0f);
    const float aspect = (float)pc.width / max((float)pc.height, 1.0f);

    float3 forward = normalize(pc.forward.xyz);
    float3 right = normalize(cross(forward, float3(0.0f, 0.0f, 1.0f)));
    if (length(right) < 0.5f) right = float3(1.0f, 0.0f, 0.0f);
    const float3 up = normalize(cross(right, forward));
    const float3 direction =
        normalize(forward + screen.x * aspect * pc.tanHalfFovX * right +
                  screen.y * pc.tanHalfFovY * up);

    float3 color;
    float3 normal;
    uint material;
    pixelOutput[id.xy] =
        raycast(pc.eye.xyz, direction, (float)pc.maxRayDistance, color,
                normal, material)
            ? float4(color, 1.0f)
            : skyColor(direction);
}
