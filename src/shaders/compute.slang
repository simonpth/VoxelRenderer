[[vk::binding(0, 0)]] RWTexture2D<float4> pixelOutput;

struct PushConstants {
    uint width;
    uint height;
    float time;
    float deltaTime;
};

[[vk::push_constant]] PushConstants pc;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.width || id.y >= pc.height) return;

    // A deterministic animated color gradient using push constants instead of GetDimensions()
    float2 uv = float2(id.xy) / float2(max(pc.width - 1, 1), max(pc.height - 1, 1));
    float wave = 0.5 + 0.5 * sin(pc.time * 2.0 + (uv.x + uv.y) * 3.14159);
    pixelOutput[id.xy] = float4(uv.x, uv.y, wave, 1.0);
}

