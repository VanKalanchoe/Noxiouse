/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/


#pragma once
#include "../RTXPTCompat.slang"
float2 ndir_to_oct_equal_area_unorm(float3 n)
{
    // Use atan2 to avoid explicit div-by-zero check in atan(y/x).
    float r = sqrt(1.f - abs(n.z));
    float phi = atan2(abs(n.y), abs(n.x));

    // Compute p = (u,v) in the first quadrant.
    float2 p;
    p.y = r * phi * K_2_PI;
    p.x = r - p.y;

    // Reflect p over the diagonals, and move to the correct quadrant.
    if (n.z < 0.f) p = 1.f - p.yx;
    p *= sign(n.xy);

    return saturate(p * 0.5f + 0.5f);
}

/** Converts point in the octahedral map to normalized direction (equal area, unsigned normalized).
    \param[in] p Position in octahedral map in [0,1] for each component.
    \return Normalized direction.
*/
float3 oct_to_ndir_equal_area_unorm(float2 p)
{
    p = p * 2.f - 1.f;

    // Compute radius r without branching. The radius r=0 at +z (center) and at -z (corners).
    float d = 1.f - (abs(p.x) + abs(p.y));
    float r = 1.f - abs(d);

    // Compute phi in [0,pi/2] (first quadrant) and sin/cos without branching.
    // TODO: Analyze fp32 precision, do we need a small epsilon instead of 0.0 here?
    float phi = (r > 0.f) ? ((abs(p.y) - abs(p.x)) / r + 1.f) * K_PI_4 : 0.f;

    // Convert to Cartesian coordinates. Note that sign(x)=0 for x=0, but that's fine here.
    float f = r * sqrt(2.f - r*r);
    float x = f * sign(p.x) * cos(phi);
    float y = f * sign(p.y) * sin(phi);
    float z = sign(d) * (1.f - r*r);

    return float3(x, y, z);
}


float3 SampleSphereUniform(float2 u) { float z=1-2*u.x; float r=sqrt(max(0.0,1-z*z)); float phi=2*K_PI*u.y; return float3(r*cos(phi),r*sin(phi),z); }
float SampleSphereUniformPDF() { return 1.0/(4*K_PI); }
/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#ifndef __ENV_MAP_HLSLI__
#define __ENV_MAP_HLSLI__

#if !defined(__cplusplus)





#endif


// Environment map color/intensity and orientation modifiers ("in-scene" settings)
struct EnvMapSceneParams
{
	float3x4    Transform;              ///< Local to world transform.
	float3x4    InvTransform;           ///< World to local transform.

    float3      ColorMultiplier;        ///< Color & radiance scale (Tint * Intensity)
    float       Enabled;                ///< 1 if enabled, 0 if not
};

// Environment map importance sampling internals
struct EnvMapImportanceSamplingParams
{
    // MIP descent sampling
    float2      ImportanceInvDim;       ///< 1.0 / dimension.
    uint        ImportanceBaseMip;      ///< Mip level for 1x1 resolution.
    uint        padding0;
};

// Returned by importance sampling functions
struct DistantLightSample
{
    float3  Dir;        ///< Sampled direction towards the light in world space.
    float   Pdf;        ///< Probability density function for the sampled direction with respect to solid angle.
    float3  Le;         ///< Emitted radiance.
};

#if !defined(__cplusplus)


// Core envmap access
struct EnvMap
{
    TextureCube<float4> Texture;            ///< Environment map texture.
    SamplerState        TextureSampler;     ///< Environment map sampler (linear, wrap).
    EnvMapSceneParams   SceneParams;        ///< Environment map scene parameters.
    
    static EnvMap make( TextureCube<float4>     texture         ///< Environment map texture.
                        , SamplerState          textureSampler  ///< Environment map texture sampler.
                        , EnvMapSceneParams     sceneParams     ///< Environment map data.
                       )
    {
        EnvMap envMap;
        envMap.Texture = texture;
        envMap.TextureSampler = textureSampler;
        envMap.SceneParams = sceneParams;
        return envMap;
    }

   // Transform direction from local to world space.
    float3 ToWorld(float3 dir)
    {
        return mul(dir, (float3x3)SceneParams.Transform);
    }

    // Transform direction from world to local space.
    float3 ToLocal(float3 dir)
    {
        return mul(dir, (float3x3)SceneParams.InvTransform);
    } 

    float3 EvalLocal(float3 localDir, float lod = 0.f)
    {
        return Texture.SampleLevel(TextureSampler, localDir, lod).rgb * SceneParams.ColorMultiplier.rgb;
    }

    float3 Eval(float3 worldDir, float lod = 0.f)
    {
        return EvalLocal(ToLocal(worldDir), lod);
    }
};

// Runtime envmap sampler & importance sampler
struct EnvMapSampler
{
    EnvMap              EnvironmentMap;

    // MIP descent sampler
    SamplerState        PointClampSampler;
    Texture2D<float>    ImportanceMap;          ///< Hierarchical importance map (entire mip chain).
    
    EnvMapImportanceSamplingParams
                        ImportanceSamplingParams;
    
// #ifndef EMIS_ENABLE_CORE_ONLY
//     Buffer<uint2>       PresampledBuffer;       ///< ENVMAP_PRESAMPLED_COUNT number of (encoded) samples pre-sampled for each frame or PT pass
// #endif 
    
    static EnvMapSampler make(
          SamplerState          pointClampSampler                   ///< Point sampling with clamp to edge.
        , Texture2D<float>      importanceMap                       ///< Hierarchical importance map (entire mip chain).
        , EnvMapImportanceSamplingParams importanceSamplingParams   ///< Data needed by importance sampling

        , TextureCube<float4>   environmentMap                      ///< Environment map texture.
        , SamplerState          environmentMapTextureSampler        ///< Environment map texture sampler.
        , EnvMapSceneParams     envMapSceneParams                   ///< Environment map data.        
    
// #ifndef EMIS_ENABLE_CORE_ONLY
//         , Buffer<uint2>         presampledBuffer
// #endif
        ) 
    {
        EnvMapSampler envMapSampler;

        envMapSampler.EnvironmentMap        = EnvMap::make( environmentMap, environmentMapTextureSampler, envMapSceneParams );
            
        envMapSampler.PointClampSampler     = pointClampSampler;
        envMapSampler.ImportanceMap         = importanceMap;    
        envMapSampler.ImportanceSamplingParams = importanceSamplingParams; 
        
// #ifndef EMIS_ENABLE_CORE_ONLY
//         envMapSampler.PresampledBuffer      = presampledBuffer;
// #endif
        
        return envMapSampler;
    }
    
    // Transform direction from local to world space.
    float3 ToWorld(float3 dir)
    {
        return EnvironmentMap.ToWorld(dir);
    }

    // Transform direction from world to local space.
    float3 ToLocal(float3 dir)
    {
        return EnvironmentMap.ToLocal(dir);
    } 

    float3 Eval(float3 worldDir, float lod = 0.f)
    {
        return EnvironmentMap.Eval(worldDir, lod);
    }
    
    DistantLightSample UniformSample(const float2 rnd)
    {
        DistantLightSample result;
        float3 dir = SampleSphereUniform(rnd);
        result.Le = EnvironmentMap.EvalLocal(dir);
        result.Dir = ToWorld(dir);
        result.Pdf = SampleSphereUniformPDF();
        return result;
    }
    float UniformEvalPdf(const float3 worldDir)
    {
        return SampleSphereUniformPDF();
    }

    // Importance sampling of the environment map using the MIP descent approach
    DistantLightSample MIPDescentSample(const float2 rnd)
    {
        DistantLightSample result;
        
        float2 p = rnd;     // Random sample in [0,1)^2.
        uint2 pos = 0;      // Top-left texel pos of current 2x2 region.

        // Iterate over mips of 2x2...NxN resolution.
        for (int mip = ImportanceSamplingParams.ImportanceBaseMip - 1; mip >= 0; mip--)
        {
            // Scale position to current mip.
            pos *= 2;

            // Load the four texels at the current position.
            float w[4];
            w[0] = ImportanceMap.Load(int3(pos, mip));
            w[1] = ImportanceMap.Load(int3(pos + uint2(1, 0), mip));
            w[2] = ImportanceMap.Load(int3(pos + uint2(0, 1), mip));
            w[3] = ImportanceMap.Load(int3(pos + uint2(1, 1), mip));

            float q[2];
            q[0] = w[0] + w[2];
            q[1] = w[1] + w[3];

            uint2 off;

            // Horizontal warp.
            float d = saturate(q[0] / (q[0] + q[1]));   // saturate is to guard against div-by-zero. In case both probabilities are 0, d will be 1 by convention (doesn't really matter either way).

            if (p.x < d) // left
            {
                off.x = 0;
                p.x = p.x / d;
            }
            else // right
            {
                off.x = 1;
                p.x = (p.x - d) / (1.f - d);
            }

            // Vertical warp.
            // Avoid stack allocation by not using dynamic indexing.
            // float e = w[off.x] / q[off.x];
            float e = off.x == 0 ? (w[0] / q[0]) : (w[1] / q[1]);

            if (p.y < e) // bottom
            {
                off.y = 0;
                p.y = p.y / e;
            }
            else // top
            {
                off.y = 1;
                p.y = (p.y - e) / (1.f - e);
            }

            pos += off;
        }

        // At this point, we have chosen a texel 'pos' in the range [0,dimension) for each component.
        // The 2D sample point 'p' has been warped along the way, and is in the range [0,1) representing sub-texel location.

#if 0 // if correctness testing vs envMapEvalPdf - due to differences in UV math samples on the border can be different; this is rare and is fine/acceptable but use this to temporarily ensure it's only that
        float eps = 2e-4f; // // eps found empirically
        p = p * (1.0-2.0*eps).xx+eps.xx;
#endif

        // Compute final sample position and map to direction.
        float2 uv = ((float2)pos + p) * ImportanceSamplingParams.ImportanceInvDim;     // Final sample in [0,1)^2.
        float3 dir = oct_to_ndir_equal_area_unorm(uv);

        // Compute final pdf.
        // We sample exactly according to the intensity of where the final samples lies in the octahedral map, normalized to its average intensity.
        float avg_w = ImportanceMap.Load(int3(0, 0, ImportanceSamplingParams.ImportanceBaseMip)); // 1x1 mip holds integral over importance map. TODO: Replace by constant or rescale in setup so that the integral is 1.0
        float pdf = ImportanceMap[pos] / avg_w;

        result.Le = EnvironmentMap.EvalLocal(dir);
        result.Dir = ToWorld(dir);
        result.Pdf = pdf / (4.0 * K_PI);

        return result;
    }

    // Evaluates the probability density function for a specific direction when using MIPDescentSample importance sampling.
    //    Note that the sample() function already returns the pdf for the sampled location.
    //    But, in some cases we need to evaluate the pdf for other directions (e.g. for MIS).
    float MIPDescentEvalPdf(const float3 worldDir)
    {
        float2 uv = ndir_to_oct_equal_area_unorm(ToLocal(worldDir));
        float avg_w = ImportanceMap.Load(int3(0, 0, ImportanceSamplingParams.ImportanceBaseMip)); // 1x1 mip holds integral over importance map. TODO: Replace by constant or rescale in setup so that the integral is 1.0
        float pdf = ImportanceMap.SampleLevel(PointClampSampler, uv, 0) / avg_w;
        return pdf / (4.0 * K_PI);
    }

};

#endif

#endif // #define __ENV_MAP_HLSLI__
