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
    // World to sun clip space for the shadow map (Metal depth 0..1).
    float4x4 shadow_vp;
    // x groundShadowDensity, y unitShadowDensity (mapinfo lighting), z 1 when the map is valid.
    float4 shadow;
    // Recoil cameraPos.
    float4 eye;
    // groundSpecularColor (xyz) and specularExponent (w).
    float4 ground_specular;
    float4 splat_scales;
    float4 splat_mults;
    // x specularTex, y DNTS splatting, z detailNormalTex blending, w splatDetailNormalDiffuseAlpha.
    float4 smf_flags;
    // x, y map size in elmos; z SMF_WATER_ABSORPTION; w unused.
    float4 map;
    float4 water_base;
    float4 water_min;
    float4 water_absorb;
    float4 unit_ambient;
    float4 unit_diffuse;
    float4 unit_specular;
    // CUS: x simFrame, y unitID, z 1 when normalTex is bound.
    float4 unit_frame;
    float4x4 sky_inv;
    // Water planeColor; w 1 when the map has a water plane.
    float4 plane_color;
    float4 water_repeat;
    float4 water_fresnel;
    float4 water_waves;
    float4 water_light;
    float4 water_specular;
    float4 water_surface;
    float4 water_shore;
    float4 water_extra;
    float4 water_screen;
    float4 grass_a;
    float4 grass_b;
    float4 grass_c;
    float4 grass_u;
    float4 platter;
};

// Recoil CGlobalRendering::SMF_INTENSITY_MULT.
constant float SMF_INTENSITY_MULT = 210.0 / 255.0;
constant float SMF_SHALLOW_WATER_DEPTH_INV = 1.0 / 10.0;

// Recoil: shadowCoeff = mix(1, shadow2DProj(...), density). 5x5 PCF over a
// comparison sampler; softness is allowed to differ from Recoil's.
float shadow_term(float3 world, constant Uniforms& u, depth2d<float> shadow_map, float density) {
    if (u.shadow.z < 0.5) {
        return 1.0;
    }
    float4 clip = u.shadow_vp * float4(world, 1.0);
    float3 ndc = clip.xyz / clip.w;
    float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || ndc.z > 1.0) {
        return 1.0;
    }
    constexpr sampler cmp(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
    const float texel = 1.0 / float(shadow_map.get_width());
    const float bias = 0.0015;
    float lit = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            lit += shadow_map.sample_compare(cmp, uv + float2(x, y) * texel, ndc.z - bias);
        }
    }
    return mix(1.0, lit / 25.0, density);
}

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
    // Model only: x CUS vertex AO, y selfIllumMod.
    float2 unit_terms;
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
    out.unit_terms = float2(1.0, 1.0);
    return out;
}

// Recoil SMFFragProg.glsl (forward pass), with the defines this map enables:
// SMF_ADV_SHADING, SMF_SPECULAR_LIGHTING, SMF_DETAIL_NORMAL_TEXTURE_SPLATTING,
// SMF_DETAIL_NORMAL_DIFFUSE_ALPHA, SMF_BLEND_NORMALS, SMF_WATER_ABSORPTION, HAVE_SHADOWS.
// Each optional define is a uniform flag set from what the map actually provides.
// DDS maps are sampled at (u, 1 - v): Recoil loads them through nv_dds with a vertical flip.
fragment float4 terrain_fs(Varying in [[stage_in]],
                           constant Uniforms& u [[buffer(0)]],
                           texture2d<float> diffuse_tex [[texture(0)]],
                           texture2d<float> normals_tex [[texture(1)]],
                           depth2d<float> shadow_map [[texture(2)]],
                           texture2d<float> specular_tex [[texture(3)]],
                           texture2d<float> splat_distr_tex [[texture(4)]],
                           texture2d<float> blend_normals_tex [[texture(5)]],
                           texture2d<float> splat_n1 [[texture(6)]],
                           texture2d<float> splat_n2 [[texture(7)]],
                           texture2d<float> splat_n3 [[texture(8)]],
                           texture2d<float> splat_n4 [[texture(9)]]) {
    // Persistent square textures: LINEAR_MIPMAP_LINEAR, 4 levels, SMFTexAniso 4.
    constexpr sampler diffuse_s(coord::normalized, filter::linear, mip_filter::linear, address::clamp_to_edge,
                                max_anisotropy(4));
    // normalsTex: MAG linear, MIN nearest, no mips.
    constexpr sampler normals_s(coord::normalized, mag_filter::linear, min_filter::nearest, address::clamp_to_edge);
    // nv_dds uploads the file's mips; GL default wrap is REPEAT.
    constexpr sampler dds_s(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
    // Splat detail normals: CreateMipMapTexture with SSMFTexAniso 4, REPEAT.
    constexpr sampler splat_s(coord::normalized, filter::linear, mip_filter::linear, address::repeat,
                              max_anisotropy(4));

    const float3 world = float3(in.world_xz.x, in.world_y, in.world_xz.y);
    const float2 map_uv = world.xz / u.map.xy;
    const float2 dds_uv = float2(map_uv.x, 1.0 - map_uv.y);

    float3 normal;
    normal.xz = normals_tex.sample(normals_s, map_uv).rg;
    normal.y = sqrt(max(0.0, 1.0 - dot(normal.xz, normal.xz)));

    // STN space: for an up normal, S and T follow +x and +z.
    const float3 t_tangent = normalize(cross(normal, float3(-1.0, 0.0, 0.0)));
    const float3 s_tangent = cross(normal, t_tangent);
    const float3x3 stn = float3x3(s_tangent, t_tangent, normal);

    if (u.smf_flags.z > 0.5) {
        const float4 dt = blend_normals_tex.sample(dds_s, dds_uv);
        normal = normalize(mix(normal, stn * (dt.xyz * 2.0 - 1.0), dt.a));
    }

    float detail = 0.0;
    if (u.smf_flags.y > 0.5) {
        const float4 cofac = splat_distr_tex.sample(dds_s, dds_uv) * u.splat_mults;
        const float strength = min(1.0, dot(cofac, float4(1.0)));
        float4 sdn = (splat_n1.sample(splat_s, world.xz * u.splat_scales.x) * 2.0 - 1.0) * cofac.x;
        sdn += (splat_n2.sample(splat_s, world.xz * u.splat_scales.y) * 2.0 - 1.0) * cofac.y;
        sdn += (splat_n3.sample(splat_s, world.xz * u.splat_scales.z) * 2.0 - 1.0) * cofac.z;
        sdn += (splat_n4.sample(splat_s, world.xz * u.splat_scales.w) * 2.0 - 1.0) * cofac.w;
        sdn.y = max(sdn.y, 0.01);
        if (u.smf_flags.w > 0.5) {
            detail = clamp(sdn.a, -1.0, 1.0);
        }
        normal = normalize(mix(normal, normalize(stn * sdn.xyz), strength));
    }

    const float3 light = u.sun_dir.xyz;
    const float3 half_dir = normalize(light + normalize(u.eye.xyz - world));
    const float cos_diffuse = clamp(dot(light, normal), 0.0, 1.0);
    const float cos_specular = clamp(dot(half_dir, normal), 0.001, 1.0);

    const float4 diffuse_col = diffuse_tex.sample(diffuse_s, map_uv);
    const float shadow_coeff = shadow_term(world, u, shadow_map, u.shadow.x);

    // GetShadeInt.
    float3 shade = (u.ambient.rgb + u.diffuse.rgb * (cos_diffuse * shadow_coeff)) * SMF_INTENSITY_MULT;
    if (u.map.z > 0.5 && world.y < 0.0) {
        float alpha = abs(world.y) * SMF_SHALLOW_WATER_DEPTH_INV;
        const float decay = 0.2 + alpha * 0.1;
        const float step_height = min(1023.0, -world.y);
        const float light_int = min(cos_diffuse * 2.0 + 0.4, 1.0);
        alpha = min(1.0, alpha + float(world.y <= -10.0));
        float3 water = u.water_base.rgb - u.water_absorb.rgb * step_height;
        water = max(u.water_min.rgb, water);
        water *= SMF_INTENSITY_MULT * light_int;
        water *= 1.0 - decay * (1.0 - shadow_coeff);
        shade = mix(shade, water, alpha);
    }

    float3 color = (diffuse_col.rgb + float3(detail)) * shade;

    float4 specular_col = float4(u.ground_specular.rgb, 1.0);
    float specular_exp = u.ground_specular.w;
    if (u.smf_flags.x > 0.5) {
        specular_col = specular_tex.sample(dds_s, dds_uv);
        specular_exp = specular_col.a * 16.0;
    }
    color += specular_col.rgb * pow(cos_specular, specular_exp) * shadow_coeff;

    const float radius = u.marker.x;
    if (radius > 0.0) {
        const float dist = length(in.world_xz - u.model_pose.xz);
        const float band = max(radius * 0.045, 1.2);
        const float ring = 1.0 - smoothstep(band * 0.25, band, abs(dist - radius));
        color = mix(color, u.team_color.rgb, saturate(ring) * 0.92);
    }
    return float4(color, 1.0);
}

struct ModelPoint {
    bool hidden;
    float3 world;
    float3 normal;
};

// Piece skin, bank, then heading and position: shared by the color and shadow passes.
ModelPoint model_point(Vertex point, const device float4x4* pieces, constant Uniforms& uniforms) {
    ModelPoint out;
    int piece = int(point.pad0 + 0.5);
    float4x4 skin = pieces[piece];
    // A script Hide stores 0 in the matrix w component.
    out.hidden = skin[3][3] == 0.0;
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
    out.world = float3(uniforms.model_pose.x + rolled_x * c + local.z * s,
                       uniforms.model_pose.y + rolled_y,
                       uniforms.model_pose.z - rolled_x * s + local.z * c);
    out.normal = float3(nx * c + local_normal.z * s, ny, -nx * s + local_normal.z * c);
    return out;
}

vertex Varying model_vs(uint vid [[vertex_id]],
                        const device Vertex* vertices [[buffer(1)]],
                        const device float4x4* pieces [[buffer(2)]],
                        constant Uniforms& uniforms [[buffer(0)]]) {
    Vertex point = vertices[vid];
    ModelPoint mp = model_point(point, pieces, uniforms);
    if (mp.hidden) {
        Varying hidden;
        hidden.position = float4(0.0, -2.0, 0.0, 1.0);
        hidden.normal = float3(0.0, 1.0, 0.0);
        hidden.uv = float2(0.0, 0.0);
        hidden.metal_uv = float2(0.0, 0.0);
        hidden.world_y = -10000.0;
        hidden.world_xz = float2(0.0, 0.0);
        hidden.unit_terms = float2(1.0, 1.0);
        return hidden;
    }
    Varying out;
    out.position = uniforms.view_proj * float4(mp.world, 1.0);
    out.normal = mp.normal;
    out.uv = float2(point.u, point.v);
    out.metal_uv = float2(point.mu, point.mv);
    out.world_y = mp.world.y;
    out.world_xz = mp.world.xz;
    // cus_gl4.vert OPTION_VERTEX_AO: AO lives in the low bits of the s3o u coordinate.
    out.unit_terms.x = clamp(fract(point.u * 16384.0), 0.1, 1.0);
    // OPTION_FLASHLIGHTS (armunit bin).
    out.unit_terms.y = max(-0.2, sin(uniforms.unit_frame.x * 2.0 / 30.0 + uniforms.unit_frame.y * 0.1)) + 0.2;
    return out;
}

struct ShadowOut {
    float4 position [[position]];
};

vertex ShadowOut shadow_terrain_vs(uint vid [[vertex_id]],
                                   const device Vertex* vertices [[buffer(1)]],
                                   constant Uniforms& uniforms [[buffer(0)]]) {
    Vertex point = vertices[vid];
    ShadowOut out;
    out.position = uniforms.shadow_vp * float4(point.px, point.py, point.pz, 1.0);
    return out;
}

vertex ShadowOut shadow_model_vs(uint vid [[vertex_id]],
                                 const device Vertex* vertices [[buffer(1)]],
                                 const device float4x4* pieces [[buffer(2)]],
                                 constant Uniforms& uniforms [[buffer(0)]]) {
    ModelPoint mp = model_point(vertices[vid], pieces, uniforms);
    ShadowOut out;
    out.position = mp.hidden ? float4(0.0, 0.0, -2.0, 1.0) : uniforms.shadow_vp * float4(mp.world, 1.0);
    return out;
}

// BAR cus_gl4.frag.glsl forward pass for the armunit bin (001_units_s3o_assimp defines:
// SPECULAR_AO, ROUGHNESS_AA 1, USE_ENVIRONMENT_DIFFUSE/SPECULAR 1, TONEMAP CustomTM, modelGamma 1).
// pbrParams is the shader-local array (the uniform declaration is commented out in CUS).
// The env SH call is commented out in CUS, so iblDiffuse/iblSpecular stay 0.
// $reflection is the CubeMapHandler sky + terrain cube (128 per face, no mips).
constant float CUS_PBR_PARAMS[8] = {4.85, 0.85, 3.5, 0.85, 1.0, 0.23, 1.0, 1.0};
constant float CUS_MIN_ROUGHNESS = 0.04;
constant float CUS_DEFAULT_F0 = 0.04;
constant float CUS_EPS = 1e-4;
// cus_gl4.lua armunit uniform bin.
constant float CUS_BRIGHTNESS_FACTOR = 1.5;
constant float3 CUS_LUMA = float3(0.2126, 0.7152, 0.0722);

float3 cus_tonemap(float3 x) {
    constant float* p = CUS_PBR_PARAMS;
    return (x * (p[0] * x + p[1])) / (x * (p[2] * x + p[3]) + p[4]);
}

fragment float4 model_fs(Varying in [[stage_in]],
                         constant Uniforms& uniforms [[buffer(0)]],
                         texture2d<float> color_tex [[texture(0)]],
                         texture2d<float> other_tex [[texture(1)]],
                         depth2d<float> shadow_map [[texture(2)]],
                         texture2d<float> normal_tex [[texture(3)]],
                         texture2d<float> brdf_lut [[texture(4)]],
                         texturecube<float> reflect_tex [[texture(11)]],
                         sampler samp [[sampler(0)]]) {
    constexpr sampler lut_s(coord::normalized, filter::linear, address::clamp_to_edge);
    // Unit textures carry their DDS mip chain (trilinear, as GL samples a mipmapped texture).
    constexpr sampler unit_s(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
    float3 world = float3(in.world_xz.x, in.world_y, in.world_xz.y);
    // Recoil loads unit DDS through nv_dds with a vertical flip and samples raw s3o uv; the
    // BC3 mips here are uploaded unflipped, so the same texel sits at (u, 1 - v).
    float2 tex_uv = float2(in.uv.x, 1.0 - in.uv.y);
    float3 worldNormal = normalize(in.normal);
    // Recoil ModelUtils::CalculateTangents: sTangent = dP/du; CUS bitangent = cross(N, T).
    float3 dp1 = dfdx(world);
    float3 dp2 = dfdy(world);
    float2 duv1 = dfdx(in.uv);
    float2 duv2 = dfdy(in.uv);
    float3 tangent = cross(dp2, worldNormal) * duv1.x + cross(worldNormal, dp1) * duv2.x;
    tangent = tangent - worldNormal * dot(worldNormal, tangent);
    float tlen = length(tangent);
    tangent = tlen > 1e-12 ? tangent / tlen : float3(1.0, 0.0, 0.0);
    float3 bitangent = normalize(cross(worldNormal, tangent));
    float3 tbnNormal = float3(0.0, 0.0, 1.0);
    if (uniforms.unit_frame.z > 0.5) {
        tbnNormal = normalize(normal_tex.sample(unit_s, tex_uv).xyz * 2.0 - 1.0);
    }
    float3 N = normalize(float3x3(tangent, bitangent, worldNormal) * tbnNormal);

    float4 texColor1 = color_tex.sample(unit_s, tex_uv);
    float4 texColor2 = other_tex.sample(unit_s, tex_uv);
    float3 albedo = mix(texColor1.rgb, uniforms.team_color.rgb, texColor1.a);
    float ao = in.unit_terms.x;
    float emissiveness = saturate(in.unit_terms.y * texColor2.r);
    float metalness = saturate(texColor2.g);
    float roughness = texColor2.b;
    {
        // ROUGHNESS_AA 1: AdjustRoughnessByNormalMap on the normalized tbnNormal (length 1: no change).
        float nlen2 = dot(tbnNormal, tbnNormal);
        if (nlen2 < 1.0) {
            float nlen = sqrt(nlen2);
            float kappa = (3.0 * nlen - nlen2 * nlen) / (1.0 - nlen2);
            roughness = min(1.0, sqrt(roughness * roughness + 1.0 / kappa));
        }
    }
    roughness = clamp(roughness, CUS_MIN_ROUGHNESS, 1.0);
    float roughness2 = roughness * roughness;
    float roughness4 = roughness2 * roughness2;

    float3 L = normalize(uniforms.sun_dir.xyz);
    float3 V = normalize(uniforms.eye.xyz - world);
    float3 H = normalize(L + V);
    float NdotLu = dot(N, L);
    float NdotL = saturate(NdotLu);
    float NdotH = saturate(dot(H, N));
    float NdotV = clamp(dot(N, V), CUS_EPS, 1.0);
    float VdotH = saturate(dot(V, H));

    float gShadow = shadow_term(world, uniforms, shadow_map, 1.0);
    float nShadow = smoothstep(0.0, 0.35, NdotLu);
    float shadowMult = mix(1.0, min(nShadow, gShadow), uniforms.shadow.y);

    float3 F0 = mix(float3(CUS_DEFAULT_F0), albedo, metalness);
    float reflectance = max(F0.r, max(F0.g, F0.b));
    float3 F90 = float3(saturate(reflectance * 50.0));
    float2 envBRDF = brdf_lut.sample(lut_s, float2(NdotV, roughness), level(0.0)).rg;
    float3 energyCompensation = clamp(1.0 + F0 * (1.0 / max(envBRDF.x, CUS_EPS) - 1.0), float3(1.0), float3(2.0));

    float3 dirContrib = float3(0.0);
    if (any(float2(NdotL, NdotV) > float2(CUS_EPS))) {
        float3 F = F0 + (F90 - F0) * pow(saturate(1.0 - VdotH), 5.0);
        float GGXV = NdotL * (NdotV * (1.0 - roughness2) + roughness2);
        float GGXL = NdotV * (NdotL * (1.0 - roughness2) + roughness2);
        float GGX = GGXV + GGXL;
        float Vis = GGX > 0.0 ? 0.5 / GGX : 0.0;
        float f = (NdotH * roughness4 - NdotH) * NdotH + 1.0;
        float D = roughness4 / (f * f);
        float3 spec = F * Vis * D;
        float3 sunSpec = uniforms.unit_specular.rgb;
        float3 sunDiff = uniforms.unit_diffuse.rgb;
        float3 maxSun = mix(sunSpec, sunDiff, step(dot(sunSpec, CUS_LUMA), dot(sunDiff, CUS_LUMA)));
        maxSun *= CUS_PBR_PARAMS[6];
        spec *= maxSun * NdotL * shadowMult * energyCompensation;
        float3 kD = (1.0 - F) * (1.0 - metalness);
        dirContrib = maxSun * (kD * albedo) * NdotL * shadowMult + spec;
    }

    float3 outColor;
    {
        float3 F = F0 + (max(F90 - float3(roughness), F0) - F0) * pow(1.0 - VdotH, 5.0);
        float3 kD = (1.0 - F) * (1.0 - metalness);
        float3 iblDiffuse = mix(uniforms.unit_ambient.rgb, float3(0.0), CUS_PBR_PARAMS[5]);
        float3 diffuse = iblDiffuse * albedo * ao;
        // Rv = mix(-reflect(V, N), N, roughness4); SampleEnvironmentWithRoughness (lod bias
        // (log2(128) - 4) * roughness, a no-op without mips); FAKE_ENV_HDR 0.4 above luma 0.55;
        // then mix toward iblSpecular (0) by roughness.
        constexpr sampler cube_s(coord::normalized, filter::linear, address::clamp_to_edge);
        float3 Rv = mix(-reflect(V, N), N, roughness4);
        float3 reflectionColor = reflect_tex.sample(cube_s, Rv).rgb;
        reflectionColor *= 1.0 + 0.4 * smoothstep(0.55, 1.0, dot(CUS_LUMA, reflectionColor));
        reflectionColor = mix(reflectionColor, float3(0.0), roughness);
        float aoTermSpec = saturate(pow(NdotV + ao, exp2(-16.0 * roughness2 - 1.0)) - 1.0 + ao);
        float3 specular = reflectionColor * (F0 * envBRDF.x + F90 * envBRDF.y) * aoTermSpec * energyCompensation;
        outColor = kD * diffuse + specular + dirContrib;
    }
    outColor += emissiveness * albedo;
    outColor *= CUS_PBR_PARAMS[7];
    outColor = cus_tonemap(outColor);
    outColor *= CUS_BRIGHTNESS_FACTOR;
    // CUS SELECTION EFFECTS (selectedness 1, set by gui_selectedunits_gl4): rim on the
    // interpolated vertex normal, tinted by the team colour clamped to [0.65, 1].
    if (uniforms.platter.y > 0.5) {
        float rim = saturate(1.0 - dot(worldNormal, V));
        outColor += rim * rim * clamp(uniforms.team_color.rgb, 0.65, 1.0);
    }
    return float4(outColor, 1.0);
}

// CSkyBox: CubeMapVS/CubeMapFS. The cube is drawn around the camera at depth 1; here a
// fullscreen triangle rebuilds the same view ray from the inverse view-projection.
struct SkyOut {
    float4 position [[position]];
    float2 ndc;
};

vertex SkyOut sky_vs(uint vid [[vertex_id]]) {
    const float2 corners[3] = {float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0)};
    SkyOut out;
    out.ndc = corners[vid];
    out.position = float4(corners[vid], 1.0, 1.0);
    return out;
}

fragment float4 sky_fs(SkyOut in [[stage_in]],
                       constant Uniforms& u [[buffer(0)]],
                       texturecube<float> skybox [[texture(10)]]) {
    constexpr sampler sky_s(coord::normalized, filter::linear, mip_filter::linear, address::clamp_to_edge);
    float4 far_point = u.sky_inv * float4(in.ndc, 1.0, 1.0);
    float4 near_point = u.sky_inv * float4(in.ndc, 0.0, 1.0);
    float3 dir = normalize(far_point.xyz / far_point.w - near_point.xyz / near_point.w);
    float4 tex = skybox.sample(sky_s, dir);
    // CubeMapFS: uvw = pos * uvFlip(1, -1, 1), so uvw.y is -dir.y.
    float below = smoothstep(0.0, 0.5, -dir.y) * u.plane_color.w;
    return float4(mix(tex.rgb, u.plane_color.rgb, below), 1.0);
}

// Advanced water, written for bar-metal from the mapinfo water parameters:
// screen-space refraction of the (already depth-absorbed) seabed, procedural wave normals,
// Fresnel sky/terrain reflection from the reflection cube, sun specular, shore foam bands
// driven by water depth, and procedural caustics in the shallows.
float3 water_world_at(constant Uniforms& u, float2 screen_uv, float d) {
    const float4 ndc = float4(screen_uv.x * 2.0 - 1.0, 1.0 - screen_uv.y * 2.0, d, 1.0);
    const float4 w = u.sky_inv * ndc;
    return w.xyz / w.w;
}

fragment float4 water_fs(Varying in [[stage_in]],
                         constant Uniforms& u [[buffer(0)]],
                         texturecube<float> reflect_tex [[texture(11)]],
                         texture2d<float> water_tex [[texture(12)]],
                         texture2d<float> scene_tex [[texture(13)]],
                         depth2d<float> scene_depth [[texture(14)]],
                         texture2d<float> foam_tex [[texture(15)]],
                         texture2d<float> wave_tex [[texture(16)]],
                         texture2d<float> caustics_tex [[texture(17)]]) {
    constexpr sampler wrap_s(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
    constexpr sampler clamp_s(coord::normalized, filter::linear, address::clamp_to_edge);
    constexpr sampler cube_s(coord::normalized, filter::linear, address::clamp_to_edge);

    const float t = u.water_surface.w;
    const float3 world = float3(in.world_xz.x, in.world_y, in.world_xz.y);
    const float2 map_uv = in.world_xz / max(u.map.xy, float2(1.0));

    // Wave normal: four octaves of the procedural normal map, each scrolling with the wind.
    float2 slope = float2(0.0);
    float freq = u.water_waves.x;
    float amp = 1.0;
    float amp_sum = 0.0;
    for (int o = 0; o < 4; ++o) {
        const float a = 0.7 + 1.9 * float(o);
        const float2 dir = float2(cos(a), sin(a));
        const float2 uv = map_uv * freq + dir * t * u.water_waves.w * 0.03 * (1.0 + 0.35 * float(o));
        const float3 n = wave_tex.sample(wrap_s, uv).xyz * 2.0 - 1.0;
        slope += amp * n.xy / max(n.z, 0.2);
        amp_sum += amp;
        freq *= u.water_waves.y;
        amp *= u.water_waves.z * 0.8;
    }
    slope /= max(amp_sum, 1e-3);
    const float3 N = normalize(float3(slope.x * 0.6, 1.0, slope.y * 0.6));

    const float3 V = normalize(u.eye.xyz - world);
    const float2 screen = in.position.xy * u.water_screen.xy;
    const uint2 size = uint2(scene_depth.get_width(), scene_depth.get_height());

    // Seabed under this pixel, and the water depth above it.
    const uint2 pix = min(uint2(in.position.xy), size - 1);
    const float3 ground = water_world_at(u, screen, scene_depth.read(pix));
    const float depth = max(0.0, world.y - ground.y);

    // Refraction: offset by the wave slope, more in deep water; reject samples that land on
    // geometry above the surface so shoreline objects do not smear into the water.
    float2 refr_uv = screen + N.xz * 0.03 * u.water_fresnel.w * saturate(depth / 30.0);
    const uint2 rpix = min(uint2(refr_uv * float2(size)), size - 1);
    const float3 refr_ground = water_world_at(u, refr_uv, scene_depth.read(rpix));
    float bed_depth = max(0.0, world.y - refr_ground.y);
    if (refr_ground.y > world.y) {
        refr_uv = screen;
        bed_depth = depth;
    }
    float3 refr = scene_tex.sample(clamp_s, refr_uv).rgb;

    // Caustics: two scrolling layers of the procedural network, multiplied, fading with depth.
    {
        const float res = max(u.water_extra.x, 1.0);
        const float2 cuv = ground.xz / (res * 4.0);
        const float c1 = caustics_tex.sample(wrap_s, cuv + float2(0.013, 0.007) * t).r;
        const float c2 = caustics_tex.sample(wrap_s, cuv * 1.37 - float2(0.009, 0.015) * t).r;
        const float shallow = exp(-bed_depth / 8.0) * saturate(bed_depth / 2.0);
        refr += float3(sqrt(c1 * c2)) * u.water_extra.y * 2.0 * shallow;
    }

    // Surface tint from the base water texture and the map's surface colour.
    float3 surface = u.water_surface.rgb;
    if (u.water_repeat.z > 0.5) {
        surface *= water_tex.sample(wrap_s, in.uv * u.water_repeat.xy).rgb;
    }
    const float3 L = normalize(u.sun_dir.xyz);
    const float lit = u.ambient.r * u.water_light.x + max(dot(N, L), 0.0) * u.water_light.y * 0.5;
    float3 color = mix(refr, surface * lit, u.water_specular.w);

    // Fresnel reflection of sky and terrain.
    const float ndv = saturate(dot(N, V));
    const float fresnel = saturate(u.water_fresnel.x +
                                   (u.water_fresnel.y - u.water_fresnel.x) * pow(1.0 - ndv, u.water_fresnel.z));
    if (u.water_extra.w > 0.5) {
        float3 R = reflect(-V, N);
        R.y = abs(R.y);
        const float3 reflection = reflect_tex.sample(cube_s, R).rgb;
        color = mix(color, reflection, fresnel);
    }

    // Sun specular.
    const float3 H = normalize(L + V);
    color += u.water_specular.rgb * u.water_light.z * pow(max(dot(N, H), 0.0), max(u.water_light.w, 1.0));

    // Shore foam: bands travelling towards the beach plus a thin wash at the waterline.
    if (u.water_shore.x > 0.5) {
        const float shore = saturate(1.0 - depth / 20.0);
        const float2 fuv = world.xz / 220.0 + slope * u.water_shore.w;
        const float foam = (u.water_extra.z > 0.5 ? foam_tex.sample(wrap_s, fuv).r
                                                  : wave_tex.sample(wrap_s, fuv).a);
        const float band = 0.5 + 0.5 * sin(depth * u.water_shore.y * 6.0 + t * 1.6);
        float amount = shore * shore * smoothstep(0.55, 0.95, band) * foam;
        amount += pow(shore, 6.0) * foam * 0.8;
        color = mix(color, float3(0.92, 0.96, 1.0), saturate(amount * u.water_shore.z));
    }

    // Soft waterline: fade to the plain seabed over the first elmo of depth.
    color = mix(refr, color, saturate(depth));
    return float4(color, 1.0);
}

// Map Grass GL4 (BAR luaui/Shaders/map_grass_gl4.{vert,frag}.glsl, MIT, Beherith), ported
// to MSL. COMPACTVBO layout; UNITBENDENABLED off (no unit bend data), LOS factor 1 and no
// fog (bar-metal has no $info LOS texture; the widget disables fog), nightFactor 1.
struct GrassVertex {
    float4 pos_u;
    float4 norm_v;
};

struct GrassOut {
    float4 position [[position]];
    float4 tex;       // xy texcoords0, w normal facing (debug only)
    float4 map_color;
    float4 params;    // x distance fade, y shadow, z LOS, w wind shade
};

vertex GrassOut grass_vs(uint vid [[vertex_id]], uint iid [[instance_id]],
                         const device GrassVertex* vertices [[buffer(1)]],
                         const device float4* instances [[buffer(3)]],
                         constant Uniforms& u [[buffer(0)]],
                         depth2d<float> shadow_map [[texture(2)]],
                         texture2d<float> perlin [[texture(19)]],
                         texture2d<float> heightmap [[texture(20)]],
                         texture2d<float> map_color_tex [[texture(21)]]) {
    constexpr sampler lin(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
    constexpr sampler clamp_s(coord::normalized, filter::linear, address::clamp_to_edge);
    constexpr sampler cmp(coord::normalized, filter::linear, address::clamp_to_edge, compare_func::less_equal);
    const GrassVertex vert = vertices[vid];
    const float4 inst = instances[iid];  // x, rot, z, size
    GrassOut out;
    out.position = float4(2.0, 2.0, 2.0, 1.0);
    out.tex = float4(0.0);
    out.map_color = float4(0.0);
    out.params = float4(0.0);

    const float4 clip0 = u.view_proj * float4(inst.x, 0.0, inst.z, 1.0);
    const float absw = abs(clip0.w);
    if (absw < 0.00001) return out;
    // GL clip z is -w..w; Metal's is 0..w.
    if (clip0.z < -absw || clip0.z > absw) return out;
    if (inst.w <= 0.0) return out;

    const float3 vpos = vert.pos_u.xyz;
    const float blade = inst.w;
    float3 p = vpos * blade;
    const float c = cos(inst.y), s = sin(inst.y);
    // GLSL rotation3dY(a): mat3(c, 0, s, 0, 1, 0, -s, 0, c) (column-major), times p.
    const float3 rp = float3(c * p.x - s * p.z, p.y, s * p.x + c * p.z);
    p.xz = rp.xz + inst.xz;

    const float2 map_size = u.map.xy;
    const float2 uvhm = float2(clamp(p.x, 8.0, map_size.x - 8.0), clamp(p.z, 8.0, map_size.y - 8.0)) / map_size;
    const float ground = heightmap.sample(clamp_s, uvhm, level(0.0)).x;
    p.y = (vpos.y + 0.5) * blade + ground;

    const float los = 1.0;

    // SHADOWTAPS 4: the cross of four taps around the vertex.
    float shadow = 1.0;
    if (u.shadow.z > 0.5) {
        const float2 offs[4] = {float2(4.0, 4.0), float2(-4.0, -4.0), float2(-4.0, 4.0), float2(4.0, -4.0)};
        for (int k = 0; k < 4; ++k) {
            const float4 sc = u.shadow_vp * float4(p + float3(offs[k].x, 0.0, offs[k].y), 1.0);
            const float3 ndc = sc.xyz / sc.w;
            const float2 suv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
            float lit = 1.0;
            if (all(suv >= 0.0) && all(suv <= 1.0) && ndc.z <= 1.0) {
                lit = shadow_map.sample_compare(cmp, suv, ndc.z - 0.0015, level(0));
            }
            shadow += clamp(lit, u.grass_b.w, 1.0);
        }
        shadow *= 0.2;
    }
    out.params.y = clamp(shadow, u.grass_b.w, 1.0);
    out.params.z = los;

    out.map_color = map_color_tex.sample(clamp_s, float2(p.x / map_size.x, p.z / map_size.y), level(0.0));

    float4 noise = perlin.sample(lin, (p.xz + u.grass_u.xy * u.grass_b.y) * u.grass_b.z, level(0.0));
    noise = (noise - 0.5).xzyw;
    float shade = noise.y * 2.0;
    shade = (shade - 0.66) * 3.0;
    noise.y = noise.y - 0.4;
    out.params.w = mix(1.0, shade, vert.norm_v.w);
    p = p + noise.xyz * vpos.y * blade * u.grass_b.x * u.grass_u.z;

    const float dist = length(p - u.eye.xyz);
    out.params.x = clamp((u.grass_c.y - dist) / (u.grass_c.y - u.grass_c.x), 0.0, 1.0);
    out.tex = float4(vert.pos_u.w, vert.norm_v.w, 0.0, 0.0);
    out.position = u.view_proj * float4(p, 1.0);
    return out;
}

fragment float4 grass_fs(GrassOut in [[stage_in]],
                         constant Uniforms& u [[buffer(0)]],
                         texture2d<float> blade_tex [[texture(18)]]) {
    constexpr sampler lin(coord::normalized, filter::linear, mip_filter::linear, address::repeat);
    // DDS blade art is stored top-first; GL's v = 0 is the bottom row after nv_dds' flip.
    float4 color = blade_tex.sample(lin, float2(in.tex.x, 1.0 - in.tex.y));
    color.rgb = mix(color.rgb, color.rgb * (in.map_color.rgb * 2.0), u.grass_a.x);
    color.rgb = mix(color.rgb, in.map_color.rgb, (1.0 - in.tex.y) * u.grass_a.y);
    color.a = color.a * u.grass_u.w * in.params.x;
    color.rgb *= in.params.y * in.params.z * in.params.w * u.grass_a.w;
    color.a = clamp((color.a - 0.5) * 1.5 + 0.5, 0.0, 1.0);
    if (color.a < u.grass_a.z) discard_fragment();
    return color;
}

// gui_selectedunits_gl4 platter through DrawPrimitiveAtUnit (USE_CIRCLES, 64 vertices,
// HEIGHTOFFSET 4, ZPULL 256). Vertices 0..191 are the fill fan; 192..575 the outline band
// from the fill edge out to the addRadius 1.3 polygon (the stencil leaves only that band).
struct PlatterOut {
    float4 position [[position]];
    float4 color;
};

vertex PlatterOut platter_vs(uint vid [[vertex_id]], constant Uniforms& u [[buffer(0)]]) {
    const uint sides = 64;
    // addRadiusCorr = 1 / sin(internalAngle / 2), internalAngle = (n - 2) * pi / n.
    const float corr = 1.0 / sin(float(sides - 2) * M_PI_F / float(sides) * 0.5);
    const float radius = u.platter.x * u.platter.z;
    const float outer = radius + 1.3 * corr;
    bool outline = vid >= sides * 3;
    float r = radius;
    uint edge = 0;
    bool center = false;
    if (!outline) {
        uint tri = vid / 3;
        uint corner = vid % 3;
        center = corner == 0;
        edge = tri + (corner == 2 ? 1 : 0);
    } else {
        uint local = vid - sides * 3;
        uint quad = local / 6;
        const uint pick[6] = {0, 1, 2, 2, 1, 3};
        uint corner = pick[local % 6];
        edge = quad + (corner & 1);
        r = (corner & 2) ? outer : radius;
    }
    // The strip's vertices start at -x and step pi / 32 round the circle.
    float phi = float(edge % sides) * (2.0 * M_PI_F / float(sides)) - M_PI_F * 0.5;
    float2 offset = center ? float2(0.0) : float2(sin(phi), cos(phi)) * r;
    float3 world = float3(u.model_pose.x + offset.x, u.model_pose.y + 4.0, u.model_pose.z + offset.y);
    PlatterOut out;
    out.position = u.view_proj * float4(world, 1.0);
    // ZPULL: GL pulls clip z by 256 / w over a [-1, 1] depth range; half that over Metal's [0, 1].
    out.position.z -= 128.0 / out.position.w;
    // POST_SHADING: mix(teamColor + addRadius, 1, 1 - 0.6), alpha 0.19 + addRadius (clamped).
    float add = outline ? 1.3 : 0.0;
    out.color = saturate(float4(mix(u.team_color.rgb + add, float3(1.0), 0.4), 0.19 + add));
    return out;
}

fragment float4 platter_fs(PlatterOut in [[stage_in]]) {
    return in.color;
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

// LuaUI images: premultiplied RGBA tinted by gl.Color (GL modulate).
fragment float4 hud_image_fs(HudOut in [[stage_in]], texture2d<float> image [[texture(0)]]) {
    // Recoil named textures keep GL's default REPEAT wrap (only ':c:' clamps, and no
    // hosted widget asks for it); FlowUI tiles rgbnoise with texcoords past 1.
    constexpr sampler lin(coord::normalized, address::repeat, filter::linear, mip_filter::linear);
    const float4 c = in.color;
    return image.sample(lin, in.uv) * float4(c.rgb * c.a, c.a);
}

fragment float4 hud_text_fs(HudOut in [[stage_in]], texture2d<float> atlas [[texture(0)]], sampler samp [[sampler(0)]]) {
    // Nearest keeps small outlined income glyphs crisp; linear turned them into color blobs.
    constexpr sampler nearest_samp(coord::normalized, address::clamp_to_edge, filter::nearest);
    (void)samp;
    return atlas.sample(nearest_samp, in.uv);
}
