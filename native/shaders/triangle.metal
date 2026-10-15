#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
    float4 color;
};

vertex Varyings triangle_vs(uint vid [[vertex_id]]) {
    const float2 pos[3] = {
        float2(-0.8, -0.7),
        float2( 0.8, -0.7),
        float2( 0.0,  0.75)
    };
    const float4 color[3] = {
        float4(0.90, 0.18, 0.14, 1.0),
        float4(0.14, 0.78, 0.32, 1.0),
        float4(0.18, 0.42, 0.95, 1.0)
    };
    Varyings out;
    out.position = float4(pos[vid], 0.0, 1.0);
    out.color = color[vid];
    return out;
}

fragment float4 triangle_fs(Varyings in [[stage_in]]) {
    return in.color;
}
