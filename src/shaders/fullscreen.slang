[[vk::binding(1, 0)]] Texture2D<float4> pixelInput;

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

