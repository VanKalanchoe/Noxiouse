// Custom Lit (MaterialShadingModel::CustomLit; docs/Stylized_Rendering_And_Material_Graph_Plan_2026-11.md step 3): what a Material Shader
// codes against. A Material Shader is a .slang file in the project's Assets (Content Browser > + Add > Material Shader) with two
// functions:
//
//   float3 light(CustomLitSurface s, CustomLitLight l)       -- one light's share of the pixel's light, called for every light
//   float3 ambient(CustomLitSurface s, CustomLitAmbient a)   -- the light from all around (sky, GI, reflections), called once
//
// The pixel's colour: the sum of light() over the lights, plus ambient(), plus the material's emissive; then the fog in front of it.
// Units as the engine's own lighting: light() returns what Default Lit's evaluateDirectLight returns (a Lambert surface: its base colour
// / pi x the light's colour x N.L), ambient() what its ambient terms add.
//
// The renderer (Renderer/CustomLitShaders.h) puts every Material Shader of the project in a namespace of its own inside one generated
// file and calls the one the pixel's material names: a file may define helper functions, structs and constants of its own; it may not
// #include (shaderIO.h and this file are already there, with the helpers below).
//
// Where it runs: the deferred frame's Custom Lit pass (DeferredLighting.slang compiled with NOX_CUSTOM_LIT over the G-buffer's Custom
// Lit pixels, after the deferred lighting) and the forward pass (Material_TransparentLit_Mesh.slang with NOX_CUSTOM_LIT: transparent
// surfaces, the editor's other viewports and the Material Editor's preview). Ray traced reflections, GI bounces and the path tracer
// see a Custom Lit surface as Default Lit (its base colour, roughness, metallic).
#pragma once
#include "shaderIO.h"

static const float CustomLitPi = 3.14159265358979;

// The surface at the pixel.
struct CustomLitSurface
{
    float3 position;     // world position, in the project's world units (centimetres in a cm project: unitsPerMeter)
    float3 normal;       // the shading normal (normal map applied), unit length, facing the camera's side
    float3 view;         // from the surface towards the camera, unit length
    float3 baseColor;    // the material's base colour (texture and vertex colour applied), linear
    float metallic;
    float roughness;     // perceptual roughness (the Material Editor's)
    float occlusion;     // ambient occlusion: the material's map times SSAO / GTAO / RTAO -- for ambient()
    float3 emissive;     // the material's emissive (the renderer adds it after; here to read)
    float2 uv;           // the mesh's first UV set
    float uvFootprint;   // how far uv moves across one pixel (custom_lit_texture picks its mip from it)
    float2 screenUV;     // 0..1 across the view (top left 0, 0)
    float time;          // seconds since the renderer started (wraps every hour): animation
    float unitsPerMeter; // world units in a meter (1 or 100)
    float4 parameters[4];       // the material's Parameter 1-4 (Material Editor > Custom Lit): colours or numbers, as the shader reads them
    uint textures[2];           // Custom Texture 1 (sRGB colour) and 2 (linear data); 0xFFFFFFFF: not set -- read with custom_lit_texture
    UniformBufferObject* ubo;   // (the renderer's frame data: custom_lit_texture reads through it)
};

// One light, as it reaches the surface.
struct CustomLitLight
{
    float3 direction;  // from the surface towards the light, unit length
    float3 color;      // the light's colour x intensity x its distance and cone falloff -- not its shadow
    float shadow;      // 1 lit .. 0 in shadow (shadow maps, ray traced shadows, cloud shadows); 0 where the surface faces away from it
    float distance;    // world units to the light (a directional light: very far)
    uint type;         // 0 directional (sun, moon), 1 point, 2 spot, 3 rect / disc (reaching the surface as a point light at its centre)
};

// The light from all around.
struct CustomLitAmbient
{
    float3 diffuse;     // what a white, matte surface here reflects of the sky and the scene's indirect light (GI): times your colour
    float3 reflection;  // the surroundings seen along the mirrored view direction, blurred by the roughness (ray traced / screen-space
                        // reflections, reflection probes, the sky) as a perfect mirror of that roughness shows them: times your Fresnel
};

// Custom Texture 1 or 2 (index 0 / 1) at `uv`, its mip from the surface's footprint (right for uv = s.uv or an offset of it; for a
// scaled UV use custom_lit_texture_footprint with the footprint scaled the same). float4(0) when the material sets no texture there.
float4 custom_lit_texture_footprint(CustomLitSurface s, uint index, float2 uv, float footprint)
{
    const uint texture = s.textures[min(index, 1u)];
    if (texture == 0xFFFFFFFFu)
        return float4(0.0);
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    return SampleMaterialTextureGrad(s.ubo, texture, linearRepeat, uv, float2(footprint, 0.0), float2(0.0, footprint));
}

float4 custom_lit_texture(CustomLitSurface s, uint index, float2 uv)
{
    return custom_lit_texture_footprint(s, index, uv, s.uvFootprint);
}

// Its sharpest mip (screen-space patterns: hatching by s.screenUV).
float4 custom_lit_texture_level(CustomLitSurface s, uint index, float2 uv, float lod)
{
    const uint texture = s.textures[min(index, 1u)];
    if (texture == 0xFFFFFFFFu)
        return float4(0.0);
    SamplerState linearRepeat = SamplerState.Handle(uint2(SamplerIndex::SAMPLER_LINEAR_REPEAT, 0));
    return SampleMaterialTextureLevel(s.ubo, texture, linearRepeat, uv, lod);
}

float custom_lit_luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Schlick's Fresnel: f0 at normal incidence, rising to 1 at grazing angles.
float3 custom_lit_fresnel(float3 f0, float cosTheta)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

// Default Lit's diffuse share of a light, as a starting point: a Lambert surface in the base colour.
float3 custom_lit_lambert(CustomLitSurface s, CustomLitLight l)
{
    return s.baseColor / CustomLitPi * l.color * saturate(dot(s.normal, l.direction)) * l.shadow;
}

// ---- Engine side (not for Material Shaders) ----

// A rect / disc light seen from `position` as a point light at its centre: its radiance x the solid angle it fills (area x the cosine
// at its face / distance squared, one-sided as the light is) x its range window. `toLight` and `distance` towards the centre.
float custom_lit_area_attenuation(LightData light, float3 position, out float3 toLight, out float distance)
{
    const float3 toCentre = light.position.xyz - position;
    distance = length(toCentre);
    toLight = distance > 1e-4 ? toCentre / distance : float3(0.0, 1.0, 0.0);
    const float3 front = normalize(light.direction.xyz);
    const float facing = max(dot(front, -toLight), 0.0);
    return rect_light_area(light) * facing / max(distance * distance, 1e-8) * rect_light_window(light, distance);
}

// The surface from what both passes know, and the material's record (its parameters and textures).
CustomLitSurface custom_lit_surface(UniformBufferObject* ubo, GpuMaterial* material, float3 position, float3 normal, float3 view, float3 baseColor,
                                    float metallic, float roughness, float occlusion, float3 emissive, float2 uv, float uvFootprint, float2 screenUV)
{
    CustomLitSurface s;
    s.position = position;
    s.normal = normal;
    s.view = view;
    s.baseColor = baseColor;
    s.metallic = metallic;
    s.roughness = roughness;
    s.occlusion = occlusion;
    s.emissive = emissive;
    s.uv = uv;
    s.uvFootprint = uvFootprint;
    s.screenUV = screenUV;
    s.time = ubo->timeSeconds;
    s.unitsPerMeter = ubo->unitsPerMeter;
    for (uint i = 0; i < 4u; ++i)
        s.parameters[i] = material.customParameters[i];
    s.textures[0] = material.customTextureIndex[0];
    s.textures[1] = material.customTextureIndex[1];
    s.ubo = ubo;
    return s;
}

CustomLitLight custom_lit_make_light(float3 direction, float3 color, float shadow, float distance, uint type)
{
    CustomLitLight l;
    l.direction = direction;
    l.color = color;
    l.shadow = shadow;
    l.distance = distance;
    l.type = type;
    return l;
}

CustomLitAmbient custom_lit_make_ambient(float3 diffuse, float3 reflection)
{
    CustomLitAmbient a;
    a.diffuse = diffuse;
    a.reflection = reflection;
    return a;
}

// A Custom Lit pixel's G-buffer data (gbuffer_encode_custom_lit): its Material Shader's slot, its material, its UV and footprint.
struct CustomLitPixel
{
    uint slot;
    uint materialIndex;
    float2 uv;
    float uvFootprint;
};

CustomLitPixel custom_lit_decode_pixel(uint4 packed)
{
    CustomLitPixel pixel;
    pixel.slot = packed.x;
    pixel.materialIndex = packed.y;
    pixel.uv = float2(f16tof32(packed.z & 0xFFFFu), f16tof32(packed.z >> 16u));
    pixel.uvFootprint = f16tof32(packed.w & 0xFFFFu);
    return pixel;
}
