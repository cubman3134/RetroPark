#pragma once
// Minimal HLSL compiled at runtime with D3DCompile.
namespace rp {

// Fullscreen triangle; samples the core texture. If no core texture is bound the
// pipeline uses the clear color instead (handled on the C++ side).
static const char* kFullscreenVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint vid : SV_VertexID) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.uv = p;
    o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);
    return o;
}
)";

static const char* kSamplePS = R"(
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return tex.Sample(smp, uv);
}
)";
}
