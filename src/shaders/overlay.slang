struct OverlayVertexInput {
    float2 position : POSITION;
    float4 color : COLOR;
};

struct OverlayVertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR;
};

[shader("vertex")]
OverlayVertexOutput overlayVertexMain(OverlayVertexInput input) {
    OverlayVertexOutput output;
    output.position = float4(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

[shader("fragment")]
float4 overlayFragmentMain(OverlayVertexOutput input) : SV_Target0 {
    return input.color;
}
