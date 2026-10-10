[[vk::binding(0, 0)]] RWTexture2D<float4> pixelOutput;
[[vk::binding(1, 0)]] Texture2D<float4> pixelInput;

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    pixelOutput.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    // A deterministic color gradient from the pixel's integer coordinates.
    float2 uv = float2(id.xy) / float2(max(width - 1, 1), max(height - 1, 1));
    pixelOutput[id.xy] = float4(uv.x, uv.y, (uv.x + uv.y) * 0.5, 1.0);
}

struct VertexOutput {
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertexId : SV_VertexID) {
    // Three vertices cover the viewport without a vertex buffer.
    float2 position = float2((vertexId << 1) & 2, vertexId & 2);
    VertexOutput output;
    output.position = float4(position * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position) : SV_Target0 {
    return pixelInput.Load(int3(int2(position.xy), 0));
}

struct TriangleVertexInput {
    float2 position : POSITION;
    float3 color : COLOR;
};

struct TriangleVertexOutput {
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
TriangleVertexOutput overlayVertexMain(TriangleVertexInput input) {
    TriangleVertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 overlayFragmentMain(TriangleVertexOutput input) : SV_Target0 {
    return float4(input.color, 1.0);
}
