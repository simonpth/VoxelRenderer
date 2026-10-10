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
};

[[vk::push_constant]] PushConstants pc;

uint voxelIndex(int3 cell) {
    return (uint(cell.x) + pc.worldSize * (uint(cell.y) + pc.worldSize * uint(cell.z))) * 2;
}

bool inWorld(int3 cell) {
    return all(cell >= int3(0, 0, 0)) &&
           all(cell < int3(pc.worldSize, pc.worldSize, pc.worldSize));
}

float4 skyColor(float3 direction) {
    float horizon = saturate(direction.z * 0.5 + 0.5);
    float3 horizonColor = float3(0.42, 0.68, 0.95);
    float3 zenithColor = float3(0.06, 0.22, 0.55);
    return float4(lerp(horizonColor, zenithColor, horizon), 1.0);
}

bool occupied(int3 cell) {
    if (!inWorld(cell))
        return false;
    return worldData[voxelIndex(cell)] != 0;
}

bool shadowRay(float3 origin, float3 direction, float maxDistance) {
    float3 inverseDirection = 1.0 / direction;
    float3 point = origin;
    int3 cell = int3(floor(point));
    int3 step = int3(
        direction.x < 0.0 ? -1 : direction.x > 0.0 ? 1 : 0,
        direction.y < 0.0 ? -1 : direction.y > 0.0 ? 1 : 0,
        direction.z < 0.0 ? -1 : direction.z > 0.0 ? 1 : 0);
    const float infinity = 3.402823466e+38;
    float3 boundary = float3(
        step.x < 0 ? float(cell.x) : float(cell.x + 1),
        step.y < 0 ? float(cell.y) : float(cell.y + 1),
        step.z < 0 ? float(cell.z) : float(cell.z + 1));
    float3 tMax = float3(
        step.x == 0 ? infinity : (boundary.x - point.x) * inverseDirection.x,
        step.y == 0 ? infinity : (boundary.y - point.y) * inverseDirection.y,
        step.z == 0 ? infinity : (boundary.z - point.z) * inverseDirection.z);
    float3 tDelta = float3(
        step.x == 0 ? infinity : abs(inverseDirection.x),
        step.y == 0 ? infinity : abs(inverseDirection.y),
        step.z == 0 ? infinity : abs(inverseDirection.z));

    for (uint iteration = 0; iteration < pc.worldSize * 3; ++iteration) {
        if (occupied(cell))
            return true;
        float next = min(min(tMax.x, tMax.y), tMax.z);
        if (next > maxDistance)
            return false;
        if (tMax.x <= next + 0.000001) {
            cell.x += step.x;
            tMax.x += tDelta.x;
        }
        if (tMax.y <= next + 0.000001) {
            cell.y += step.y;
            tMax.y += tDelta.y;
        }
        if (tMax.z <= next + 0.000001) {
            cell.z += step.z;
            tMax.z += tDelta.z;
        }
    }
    return false;
}

float4 traceVoxel(float3 origin, float3 direction) {
    const float size = float(pc.worldSize);
    const float3 worldMin = float3(0.0);
    const float3 worldMax = float3(size);
    const float infinity = 3.402823466e+38;

    // Intersect the ray with the world before converting it to a grid ray.
    // Infinite inverse directions are intentional for axes parallel to a slab.
    float3 inverseDirection = 1.0 / direction;
    float3 slabA = (worldMin - origin) * inverseDirection;
    float3 slabB = (worldMax - origin) * inverseDirection;
    float3 slabMin = min(slabA, slabB);
    float3 slabMax = max(slabA, slabB);
    float entry = max(max(slabMin.x, slabMin.y), slabMin.z);
    float exit = min(min(slabMax.x, slabMax.y), slabMax.z);
    if (entry > exit || exit < 0.0)
        return skyColor(direction);

    const float epsilon = 0.00001;
    float t = max(entry, 0.0) + epsilon;
    float3 point = origin + direction * t;
    int3 cell = int3(floor(point));
    int3 step = int3(
        direction.x < 0.0 ? -1 : direction.x > 0.0 ? 1 : 0,
        direction.y < 0.0 ? -1 : direction.y > 0.0 ? 1 : 0,
        direction.z < 0.0 ? -1 : direction.z > 0.0 ? 1 : 0);
    float3 boundary = float3(
        step.x < 0 ? float(cell.x) : float(cell.x + 1),
        step.y < 0 ? float(cell.y) : float(cell.y + 1),
        step.z < 0 ? float(cell.z) : float(cell.z + 1));
    float3 tMax = float3(
        step.x == 0 ? infinity : t + (boundary.x - point.x) * inverseDirection.x,
        step.y == 0 ? infinity : t + (boundary.y - point.y) * inverseDirection.y,
        step.z == 0 ? infinity : t + (boundary.z - point.z) * inverseDirection.z);
    float3 tDelta = float3(
        step.x == 0 ? infinity : abs(inverseDirection.x),
        step.y == 0 ? infinity : abs(inverseDirection.y),
        step.z == 0 ? infinity : abs(inverseDirection.z));

    float3 hitNormal = float3(0.0);
    for (uint iteration = 0; iteration < pc.worldSize * 3; ++iteration) {
        if (!inWorld(cell) || t > exit)
            break;

        uint dataIndex = voxelIndex(cell);
        if (worldData[dataIndex] != 0) {
            uint color = worldData[dataIndex + 1];
            float3 hitPoint = origin + direction * t;
            const float3 sunDirection = normalize(float3(-0.55, -0.35, 0.75));
            float shadowDistance = 2.0 * size;
            bool shadowed = shadowRay(hitPoint + hitNormal * 0.001, sunDirection, shadowDistance);
            float diffuse = shadowed ? 0.18 : max(dot(hitNormal, sunDirection), 0.0);
            float light = 0.24 + diffuse * 0.76;
            return float4(float((color >> 0) & 255) / 255.0 * light,
                          float((color >> 8) & 255) / 255.0 * light,
                          float((color >> 16) & 255) / 255.0 * light,
                          float((color >> 24) & 255) / 255.0);
        }

        t = min(min(tMax.x, tMax.y), tMax.z);
        const float tieEpsilon = 0.000001;
        if (tMax.x <= t + tieEpsilon) {
            cell.x += step.x;
            tMax.x += tDelta.x;
            hitNormal = float3(-step.x, 0.0, 0.0);
        }
        if (tMax.y <= t + tieEpsilon) {
            cell.y += step.y;
            tMax.y += tDelta.y;
            hitNormal = float3(0.0, -step.y, 0.0);
        }
        if (tMax.z <= t + tieEpsilon) {
            cell.z += step.z;
            tMax.z += tDelta.z;
            hitNormal = float3(0.0, 0.0, -step.z);
        }
    }
    return skyColor(direction);
}

[[shader("compute")]]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height)
        return;

    float2 pixel = (float2(id.xy) + 0.5) / float2(pc.width, pc.height);
    float aspect = float(pc.width) / float(max(pc.height, 1));
    // Fragment coordinates start at the top of the Vulkan framebuffer, while
    // positive camera-up must point toward the top of the image.
    float2 screen = float2(pixel.x * 2.0 - 1.0, 1.0 - pixel.y * 2.0);
    screen.x *= aspect;

    float3 forward = normalize(pc.viewDirection.xyz);
    float3 right = normalize(cross(float3(0.0, 0.0, 1.0), forward));
    float3 up = normalize(cross(forward, right));
    float3 rayDirection = normalize(forward + screen.x * right * 0.8 + screen.y * up * 0.8);
    pixelOutput[id.xy] = traceVoxel(pc.playerPosition.xyz, rayDirection);
}
