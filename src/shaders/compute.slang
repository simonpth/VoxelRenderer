// Fullscreen ray trace over the sparse voxel octree.
//
// Bindings:
//   0  storage image  compute output
//   1  sampled image  the same image, read by the blit
//   2  root table     one uint2 per root-layer brick (the brick root node)
//   3  node pool      every streamed node of every resident brick
//   4  material table uint2 per material id
//
// The traversal never trusts a CPU-supplied index: every pool load is bounds
// checked, the stack is a fixed size, and the iteration count is capped, so
// malformed data cannot hang the GPU.

#include "svo.slang"

[[vk::binding(0, 0)]] RWTexture2D<float4> pixelOutput;
[[vk::binding(1, 0)]] Texture2D<float4> sampledOutput;
[[vk::binding(2, 0)]] StructuredBuffer<uint2> rootTable;
[[vk::binding(3, 0)]] StructuredBuffer<uint2> nodePool;
[[vk::binding(4, 0)]] StructuredBuffer<uint2> materialTable;

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

float4 unpackRgba8(uint packed) {
    return float4((float)((packed >> 24) & 0xFFu) / 255.0f,
                  (float)((packed >> 16) & 0xFFu) / 255.0f,
                  (float)((packed >> 8) & 0xFFu) / 255.0f,
                  (float)(packed & 0xFFu) / 255.0f);
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
                out uint hitMaterial) {
    // Cleared up front, because not every early return sets them: a caller
    // that inspects the outputs after a false result -- raycast does, to tell
    // "hit air" from "ran out of data" -- would otherwise read uninitialised
    // memory. A miss with material 0 means the ray genuinely reached nothing.
    hitDistance = 0.0f;
    hitNormal = float3(0.0f, 0.0f, 1.0f);
    hitMaterial = 0u;

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
    for (int iteration = 0; iteration < 512; ++iteration) {
        const int3 brickCell =
            int3(cell.x / brick, cell.y / brick, cell.z / brick);
        const uint rootIndex =
            (uint)(brickCell.x + dimX * (brickCell.y + dimY * brickCell.z));
        if (rootIndex >= (uint)(dimX * dimY * dimZ)) return false;
        const uint2 rootPacked = rootTable[rootIndex];
        // Unknown data is never silently treated as empty space: the caller
        // renders it as missing rather than as air.
        if (((rootPacked.x >> 24) & kSvoFlagResident) == 0u) {
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
        if (overlaps) {
            const float entry = max(distance, brickNear);
            if (traverseBrick(origin, inverseDirection, dir, rootPacked,
                              brickLo, brickEdge, entry,
                              min(brickFar, tFar), anyHit, hitDistance,
                              hitNormal, hitMaterial))
                return true;
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
    const bool hit = walkBricks(origin, dir, (float)pc.maxRayDistance, true,
                                hitDistance, hitNormal, hitMaterial);
    // `hitMaterial` is kUnknownMaterial exactly when the walk stopped because a
    // brick had not been streamed. Both cases block.
    return hit || hitMaterial == kUnknownMaterial;
}

// Primary ray: the nearest terminal solid node, shaded by the material table.
bool raycast(float3 origin, float3 dir, float rayLimit, out float3 color,
             out float3 normal, out uint material) {
    float hitDistance;
    float3 hitNormal;
    uint hitMaterial = 0;
    if (!walkBricks(origin, dir, rayLimit, false, hitDistance, hitNormal,
                    hitMaterial)) {
        if (hitMaterial != kUnknownMaterial) return false;
        // Missing brick: show it as unknown data, not as empty space.
        color = kUnknownColor;
        normal = float3(0.0f, 0.0f, 1.0f);
        material = kUnknownMaterial;
        return true;
    }

    material = hitMaterial;
    normal = hitNormal;
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
