// Fullscreen triangle from SV_VertexID 0..2 (no vertex buffer, no input layout). uv (0,0) is the top-left of the viewport.
struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
};

VsOut vs_main(uint id : SV_VertexID) {
    VsOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
