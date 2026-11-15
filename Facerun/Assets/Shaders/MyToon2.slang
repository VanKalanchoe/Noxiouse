#include "CustomLit.slang" // (for your code editor: the engine's inputs and helpers; the engine has them already)

// A Material Shader (Custom Lit): how a material whose Shading Model is "Custom Lit" is lit.
//   light()   runs once for every light that reaches the surface: that light's share of the pixel's light.
//   ambient() runs once: the light from all around (sky, GI, reflections).
// The pixel is the sum of light() over the lights + ambient() + the material's emissive (then the fog in front of it).
// Inputs: CustomLitSurface s (position, normal, view, baseColor, metallic, roughness, occlusion, uv, screenUV, time,
// parameters[0..3], textures), CustomLitLight l (direction, color, shadow, distance, type), CustomLitAmbient a (diffuse,
// reflection). Helpers: custom_lit_texture(s, 0 or 1, uv), custom_lit_luminance, custom_lit_fresnel, custom_lit_lambert.
// (NoxEditor/assets/shaders/CustomLit.slang describes every field.) No other #include: the engine puts this file in a namespace.
//
// This one: a two-tone toon. Parameter 1 (rgb) tints the shadow side, Parameter 2 x is the rim's strength.

float3 light(CustomLitSurface s, CustomLitLight l)
{
    const float facing = dot(s.normal, l.direction);
    const float lit = smoothstep(0.0, 0.04, facing) * step(0.5, l.shadow); // lit or not: no gradient
    const float3 shadowTint = s.parameters[0].rgb;
    const float3 tone = lerp(shadowTint * 0.25, float3(1.0), lit);
    const float rim = smoothstep(0.65, 0.7, 1.0 - saturate(dot(s.normal, s.view))) * lit * s.parameters[1].x;
    return (s.baseColor * tone + rim) / CustomLitPi * l.color;
}

float3 ambient(CustomLitSurface s, CustomLitAmbient a)
{
    return s.baseColor * a.diffuse * s.occlusion;
}
