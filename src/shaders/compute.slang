[[vk::binding(0, 0)]] RWTexture2D<float4> pixelOutput;
[[vk::binding(2, 0)]] StructuredBuffer<uint> worldData;

struct PushConstants {
    uint width;
    uint height;
    float time;
    float deltaTime;
    float4 playerPosition;
    float4 viewDirection;
    uint worldSize;
    uint worldDepth;
    uint maxRayDistance;
    float lodPixelThreshold;
};

[[vk::push_constant]] PushConstants pc;

float4 skyColor(float3 direction) {
    float horizon = saturate(direction.z * 0.5 + 0.5);
    return float4(lerp(float3(0.42, 0.68, 0.95),
                       float3(0.06, 0.22, 0.55), horizon), 1.0);
}

bool intersectBox(float3 origin, float3 direction, float3 minimum,
                  float3 maximum, out float entry, out float exit) {
    const float epsilon = 1.0e-6;
    float3 inverseDirection = float3(
        abs(direction.x) < epsilon ? (direction.x < 0.0 ? -1.0 / epsilon : 1.0 / epsilon) : 1.0 / direction.x,
        abs(direction.y) < epsilon ? (direction.y < 0.0 ? -1.0 / epsilon : 1.0 / epsilon) : 1.0 / direction.y,
        abs(direction.z) < epsilon ? (direction.z < 0.0 ? -1.0 / epsilon : 1.0 / epsilon) : 1.0 / direction.z);
    float3 a = (minimum - origin) * inverseDirection;
    float3 b = (maximum - origin) * inverseDirection;
    float3 low = min(a, b);
    float3 high = max(a, b);
    entry = max(max(low.x, low.y), low.z);
    exit = min(min(high.x, high.y), high.z);
    return entry <= exit && exit >= 0.0;
}

bool findRegion(int3 cell, out uint header) {
    header = 0;
    uint count = min(worldData[0], 64u);
    if (cell.x < 0 || cell.x >= int(pc.worldSize) ||
        cell.y < 0 || cell.y >= int(pc.worldSize) ||
        cell.z < 0 || cell.z >= int(pc.worldDepth)) return false;
    uint rx = uint(cell.x) / 1024;
    uint ry = uint(cell.y) / 1024;
    uint rz = uint(cell.z) / 1024;
    const uint headerWords = 8;
    const uint headerBase = 1;
    for (uint i = 0; i < count; ++i) {
        uint candidate = headerBase + i * headerWords;
        if (worldData[candidate] == rx &&
            worldData[candidate + 1] == ry &&
            worldData[candidate + 2] == rz) {
            header = candidate;
            return worldData[candidate + 4] != 0;
        }
    }
    return false;
}

uint nodeWord(uint base, uint node, uint word) {
    return worldData[base + node * 3 + word];
}

bool traceRegion(float3 origin, float3 direction, uint header,
                 int3 regionOrigin, out float hitDistance,
                 out float3 hitNormal, out uint material, float rayLimit) {
    hitDistance = 0.0;
    hitNormal = float3(0.0);
    material = 0;
    const uint nodeBase = worldData[header + 3];
    const uint nodeCount = min(worldData[header + 4], 5000000u);
    if (nodeCount == 0) return false;
    if (nodeBase < 513u) return false;
    if (nodeBase + nodeCount * 3u > 16u * 1024u * 1024u)
        return false;

    uint nodeStack[256];
    float nearStack[256];
    float farStack[256];
    uint3 originStack[256];
    uint sizeStack[256];
    uint stackSize = 0;
    uint visits = 0;
    float rootNear, rootFar;
    if (!intersectBox(origin, direction, float3(regionOrigin),
                      float3(regionOrigin + int3(1024)), rootNear, rootFar))
        return false;
    if (rootNear > rayLimit)
        return false;
    nodeStack[stackSize] = 0;
    nearStack[stackSize] = max(rootNear, 0.0);
    farStack[stackSize] = rootFar;
    originStack[stackSize] = uint3(0, 0, 0);
    sizeStack[stackSize] = 1024;
    ++stackSize;

    while (stackSize != 0 && visits++ < 4096) {
        uint best = 0;
        for (uint i = 1; i < stackSize; ++i)
            if (nearStack[i] < nearStack[best]) best = i;
        uint node = nodeStack[best];
        float nodeNear = nearStack[best];
        float nodeFar = farStack[best];
        uint3 localOrigin = originStack[best];
        uint size = sizeStack[best];
        --stackSize;
        if (node >= nodeCount || nodeNear > nodeFar) continue;

        uint childMask = nodeWord(nodeBase, node, 0);
        uint packedMaterial = nodeWord(nodeBase, node, 2);
        uint flags = packedMaterial >> 16;
        float projectedSize = float(size) * float(pc.height) /
                              max(nodeNear, 1.0) / 2.0;
        if (childMask != 0 && (flags & 4) != 0 &&
            projectedSize < pc.lodPixelThreshold) {
            childMask = 0;
        }
        if (childMask == 0) {
            if ((flags & 1) != 0 ||
                ((flags & 4) != 0 && projectedSize < pc.lodPixelThreshold)) {
                hitDistance = nodeNear;
                if (hitDistance > rayLimit)
                    continue;
                material = packedMaterial & 0xFFFF;
                float3 point = origin + direction * hitDistance;
                float3 low = float3(regionOrigin) + float3(localOrigin);
                float3 high = low + float(size);
                float3 distanceToLow = abs(point - low);
                float3 distanceToHigh = abs(high - point);
                float minimum = min(min(min(distanceToLow.x, distanceToLow.y),
                                        distanceToLow.z),
                                    min(min(distanceToHigh.x, distanceToHigh.y),
                                        distanceToHigh.z));
                hitNormal = minimum == distanceToLow.x ? float3(-1, 0, 0) :
                            minimum == distanceToLow.y ? float3(0, -1, 0) :
                            minimum == distanceToLow.z ? float3(0, 0, -1) :
                            minimum == distanceToHigh.x ? float3(1, 0, 0) :
                            minimum == distanceToHigh.y ? float3(0, 1, 0) :
                            float3(0, 0, 1);
                return true;
            }
            continue;
        }
        if (size == 1) continue;
        uint half = size >> 1;
        uint firstChild = nodeWord(nodeBase, node, 1);
        for (int child = 7; child >= 0; --child) {
            if ((childMask & (1u << uint(child))) == 0 || stackSize >= 256) continue;
            if (firstChild + uint(child) >= nodeCount) continue;
            uint3 childOrigin = localOrigin;
            if ((child & 1) != 0) childOrigin.x += half;
            if ((child & 2) != 0) childOrigin.y += half;
            if ((child & 4) != 0) childOrigin.z += half;
            float childNear, childFar;
            float3 childLow = float3(regionOrigin) + float3(childOrigin);
            if (!intersectBox(origin, direction, childLow,
                              childLow + float(half), childNear, childFar))
                continue;
            nodeStack[stackSize] = firstChild + uint(child);
            nearStack[stackSize] = max(childNear, nodeNear);
            farStack[stackSize] = min(childFar, nodeFar);
            originStack[stackSize] = childOrigin;
            sizeStack[stackSize] = half;
            ++stackSize;
        }
    }
    return false;
}

bool traceShadow(float3 origin, float3 direction) {
    float distance = 0.0;
    for (uint segment = 0; segment < 8 && distance < float(pc.maxRayDistance); ++segment) {
        float3 point = origin + direction * (distance + 0.0001);
        int3 cell = int3(floor(point));
        uint header;
        if (!findRegion(cell, header)) {
            distance += 1024.0;
            continue;
        }
        int3 regionOrigin = int3(
            (cell.x / 1024) * 1024, (cell.y / 1024) * 1024,
            (cell.z / 1024) * 1024);
        if (cell.x < 0) regionOrigin.x -= 1024;
        if (cell.y < 0) regionOrigin.y -= 1024;
        float hitDistance;
        float3 normal;
        uint material;
        if (traceRegion(origin, direction, header, regionOrigin,
                        hitDistance, normal, material,
                        float(pc.maxRayDistance)))
            return true;
        float regionNear, regionFar;
        if (!intersectBox(origin, direction, float3(regionOrigin),
                          float3(regionOrigin + int3(1024)),
                          regionNear, regionFar))
            distance += 1024.0;
        else
            distance = max(distance + 0.01, regionFar);
    }
    return false;
}

bool traceVoxel(float3 origin, float3 direction, out float3 color) {
    color = float3(0.0);
    float distance = 0.0;
    const float rayLimit = float(pc.maxRayDistance);
    const float epsilon = 0.001;

    // Walk the ray one 1024-cell region at a time inside the finite world.
    for (uint segment = 0; segment < 64 && distance < rayLimit; ++segment) {
        float3 point = origin + direction * (distance + epsilon);
        int3 cell = int3(floor(point));
        uint header;
        int3 regionOrigin = int3(floor(point / 1024.0)) * 1024;
        float regionNear, regionFar;
        bool intersects = intersectBox(
            origin, direction, float3(regionOrigin),
            float3(regionOrigin + int3(1024)), regionNear, regionFar);
        float nextDistance = max(distance + 0.01, regionFar);
        if (!intersects || nextDistance <= distance)
            nextDistance = distance + 0.5;

        // Missing regions are unknown space.  Still advance to the exact
        // region boundary instead of skipping a fixed distance.
        if (!findRegion(cell, header)) {
            distance = min(nextDistance, rayLimit);
            continue;
        }

        float hitDistance;
        float3 normal;
        uint material;
        if (traceRegion(origin, direction, header, regionOrigin,
                        hitDistance, normal, material, rayLimit)) {
            float3 sunDirection = normalize(float3(-0.55, -0.35, 0.75));
            float3 hitPoint = origin + direction * hitDistance;
            float light = 0.28 + max(dot(normal, sunDirection), 0.0) * 0.72;
            color = (material == 1 ? float3(0.50, 0.50, 0.50) :
                     float3(0.75, 0.35, 0.18)) * light;
            return true;
        }
        distance = min(nextDistance, rayLimit);
    }
    return false;
}

[numthreads(8, 8, 1)]
[[shader("compute")]]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;
    float2 pixel = (float2(id.xy) + 0.5) / float2(pc.width, pc.height);
    float aspect = float(pc.width) / float(max(pc.height, 1));
    float2 screen = float2(pixel.x * 2.0 - 1.0, 1.0 - pixel.y * 2.0);
    screen.x *= aspect;
    float3 forward = normalize(pc.viewDirection.xyz);
    float3 right = normalize(cross(forward, float3(0.0, 0.0, 1.0)));
    float3 up = normalize(cross(right, forward));
    float3 direction = normalize(forward + screen.x * right * 0.8 +
                                 screen.y * up * 0.8);
    float3 color;
    pixelOutput[id.xy] = traceVoxel(pc.playerPosition.xyz, direction, color)
        ? float4(color, 1.0) : skyColor(direction);
}
