#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 view_proj;
    float4 sun_dir;
    float4 ambient;
    float4 diffuse;
    float4 sky;
    float4 team_color;
    float4 model_pose;
    // x is the ground-ring radius in elmos. 0 hides it.
    float4 marker;
};

struct Vertex {
    float px;
    float py;
    float pz;
    float pad0;
    float nx;
    float ny;
    float nz;
    float pad1;
    float u;
    float v;
    float mu;
    float mv;
};

struct Varying {
    float4 position [[position]];
    float3 normal;
    float2 uv;
    float2 metal_uv;
    float world_y;
    float2 world_xz;
};

vertex Varying terrain_vs(uint vid [[vertex_id]],
                          const device Vertex* vertices [[buffer(1)]],
                          constant Uniforms& uniforms [[buffer(0)]]) {
    Vertex point = vertices[vid];
    Varying out;
    out.position = uniforms.view_proj * float4(point.px, point.py, point.pz, 1.0);
    out.normal = float3(point.nx, point.ny, point.nz);
    out.uv = float2(point.u, point.v);
    out.metal_uv = float2(point.mu, point.mv);
    out.world_y = point.py;
    out.world_xz = float2(point.px, point.pz);
    return out;
}

fragment float4 terrain_fs(Varying in [[stage_in]],
                           constant Uniforms& uniforms [[buffer(0)]],
                           texture2d<float> minimap [[texture(0)]],
                           texture2d<float> metal_map [[texture(1)]],
                           sampler samp [[sampler(0)]]) {
    float3 albedo = minimap.sample(samp, in.uv).rgb;
    float metal = metal_map.sample(samp, in.metal_uv).r;
    albedo = mix(albedo, float3(0.92, 0.78, 0.18), saturate(metal * 1.4));
    float3 normal = normalize(in.normal);
    float ndotl = saturate(dot(normal, uniforms.sun_dir.xyz));
    float3 lit = albedo * (uniforms.ambient.rgb + uniforms.diffuse.rgb * ndotl);
    float shore = saturate(1.0 - abs(in.world_y) / 12.0);
    lit = mix(lit, lit * float3(1.15, 1.05, 0.75), shore * 0.35);
    float radius = uniforms.marker.x;
    if (radius > 0.0) {
        float dist = length(in.world_xz - uniforms.model_pose.xz);
        float band = max(radius * 0.045, 1.2);
        float ring = 1.0 - smoothstep(band * 0.25, band, abs(dist - radius));
        lit = mix(lit, uniforms.team_color.rgb, saturate(ring) * 0.92);
    }
    return float4(lit, 1.0);
}

vertex Varying model_vs(uint vid [[vertex_id]],
                        const device Vertex* vertices [[buffer(1)]],
                        const device float4x4* pieces [[buffer(2)]],
                        constant Uniforms& uniforms [[buffer(0)]]) {
    Vertex point = vertices[vid];
    int piece = int(point.pad0 + 0.5);
    float4x4 skin = pieces[piece];
    // A script Hide stores 0 in the matrix w component.
    if (skin[3][3] == 0.0) {
        Varying hidden;
        hidden.position = float4(0.0, -2.0, 0.0, 1.0);
        hidden.normal = float3(0.0, 1.0, 0.0);
        hidden.uv = float2(0.0, 0.0);
        hidden.metal_uv = float2(0.0, 0.0);
        hidden.world_y = -10000.0;
        hidden.world_xz = float2(0.0, 0.0);
        return hidden;
    }
    float3 local = (skin * float4(point.px, point.py, point.pz, 1.0)).xyz;
    float3 local_normal = (skin * float4(point.nx, point.ny, point.nz, 0.0)).xyz;
    // Positive bank rolls around +z and lowers local +x. Heading 0 faces +z, so
    // local +x is the left wing, and a positive yaw turns toward that wing.
    float cb = cos(uniforms.marker.y);
    float sb = sin(uniforms.marker.y);
    float rolled_x = local.x * cb + local.y * sb;
    float rolled_y = -local.x * sb + local.y * cb;
    float nx = local_normal.x * cb + local_normal.y * sb;
    float ny = -local_normal.x * sb + local_normal.y * cb;
    float c = cos(uniforms.model_pose.w);
    float s = sin(uniforms.model_pose.w);
    // Heading 0 keeps local +z. A positive heading turns that axis toward +x.
    // The same sign is the piece Y rotation: positive Y sends local +z toward +x.
    float3 world = float3(uniforms.model_pose.x + rolled_x * c + local.z * s,
                          uniforms.model_pose.y + rolled_y,
                          uniforms.model_pose.z - rolled_x * s + local.z * c);
    float3 normal = float3(nx * c + local_normal.z * s, ny, -nx * s + local_normal.z * c);
    Varying out;
    out.position = uniforms.view_proj * float4(world, 1.0);
    out.normal = normal;
    out.uv = float2(point.u, point.v);
    out.metal_uv = float2(point.mu, point.mv);
    out.world_y = world.y;
    out.world_xz = world.xz;
    return out;
}

fragment float4 model_fs(Varying in [[stage_in]],
                         constant Uniforms& uniforms [[buffer(0)]],
                         texture2d<float> color_tex [[texture(0)]],
                         texture2d<float> other_tex [[texture(1)]],
                         sampler samp [[sampler(0)]]) {
    float4 albedo = color_tex.sample(samp, in.uv);
    // BAR mixes team color by tex1 alpha. tex2 alpha is opaque on this model.
    float team = albedo.a;
    float3 base = mix(albedo.rgb, uniforms.team_color.rgb, saturate(team));
    float ndotl = saturate(dot(normalize(in.normal), uniforms.sun_dir.xyz));
    float3 lit = base * (uniforms.ambient.rgb + uniforms.diffuse.rgb * ndotl);
    return float4(lit, 1.0);
}

fragment float4 water_fs(Varying in [[stage_in]], constant Uniforms& uniforms [[buffer(0)]]) {
    float ndotl = saturate(dot(normalize(in.normal), uniforms.sun_dir.xyz));
    float3 color = float3(0.05, 0.42, 0.48) * (0.45 + 0.9 * ndotl);
    return float4(color, 0.72);
}

// Free mex ring. gui_metalspots uses rgb (0, 1, 0.5) and fades the ring near alpha 0.5.
fragment float4 ring_fs(Varying in [[stage_in]]) {
    return float4(0.0, 1.0, 0.5, 0.6);
}

struct HudIn {
    float x;
    float y;
    float u;
    float v;
    float r;
    float g;
    float b;
    float a;
};

struct HudOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
};

vertex HudOut hud_vs(uint vid [[vertex_id]], const device HudIn* vertices [[buffer(1)]]) {
    HudIn point = vertices[vid];
    HudOut out;
    out.position = float4(point.x, point.y, 0.0, 1.0);
    out.color = float4(point.r, point.g, point.b, point.a);
    out.uv = float2(point.u, point.v);
    return out;
}

fragment float4 hud_color_fs(HudOut in [[stage_in]]) {
    return in.color;
}

fragment float4 hud_text_fs(HudOut in [[stage_in]], texture2d<float> atlas [[texture(0)]], sampler samp [[sampler(0)]]) {
    // Nearest keeps small outlined income glyphs crisp; linear turned them into color blobs.
    constexpr sampler nearest_samp(coord::normalized, address::clamp_to_edge, filter::nearest);
    (void)samp;
    return atlas.sample(nearest_samp, in.uv);
}
