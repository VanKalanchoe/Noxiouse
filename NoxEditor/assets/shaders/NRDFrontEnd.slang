// NRD's front and back end for the signals the engine denoises, ported verbatim from NRD v4.17 Shaders/NRD.hlsli (keep in
// sync with the library version): radiance is de-modulated by the material before NRD and modulated back after it
// (NRD_MaterialFactors); REBLUR takes YCoCg radiance and a hit distance normalized by its hitDistanceParameters, RELAX
// linear radiance and the raw hit distance.
#ifndef NRD_FRONT_END_SLANG
#define NRD_FRONT_END_SLANG

static const float NRD_FP16_MAX = 65504.0;
static const float NRD_EPS = 1e-6;
static const float NRD_MATERIAL_FACTOR_MIN_SCALE = 0.02; // smaller values may lead to instabilities
static const float NRD_ROUGHNESS_FACTOR_MIN_SCALE = 0.1; // smaller values may lead to instabilities and bias
// nrd::ReblurHitDistanceParameters defaults (NRDSettings.h: A, B, C): the engine does not change them.
static const float3 NRD_REBLUR_HIT_DISTANCE_PARAMETERS = float3(3.0, 0.1, 20.0);

// The engine's denoiser modes (push constants): what a signal was packed for.
static const uint NRD_SIGNAL_RAW = 0;
static const uint NRD_SIGNAL_REBLUR = 1;
static const uint NRD_SIGNAL_RELAX = 2;

bool _NRD_IsInvalid(float3 x) { return any(isnan(x)) || any(isinf(x)); }
bool _NRD_IsInvalid(float x) { return isnan(x) || isinf(x); }

float3 _NRD_LinearToYCoCg( float3 color )
{
    float Y = dot( color, float3( 0.25, 0.5, 0.25 ) );
    float Co = dot( color, float3( 0.5, 0.0, -0.5 ) );
    float Cg = dot( color, float3( -0.25, 0.5, -0.25 ) );

    return float3( Y, Co, Cg );
}

float3 _NRD_YCoCgToLinear( float3 color )
{
    float t = color.x - color.z;

    float3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;

    return max( r, 0.0 );
}

float3 _NRD_EnvironmentTerm_Rtg( float3 Rf0, float NoV, float roughness )
{
    // "Ray Tracing Gems", Chapter 32, Equation 4 - the approximation assumes GGX VNDF and Schlick's approximation
    float m = saturate( roughness * roughness );

    float4 X;
    X.x = 1.0;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;

    float4 Y;
    Y.x = 1.0;
    Y.y = m;
    Y.z = m * m;
    Y.w = m * Y.z;

    const float2x2 M1 = float2x2( 0.99044, -1.28514, 1.29678, -0.755907 );
    const float3x3 M2 = float3x3( 1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627 );

    const float2x2 M3 = float2x2( 0.0365463, 3.32707, 9.0632, -9.04756 );
    const float3x3 M4 = float3x3( 1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123 );

    float bias = dot( mul( M1, X.xy ), Y.xy ) * rcp( max( dot( mul( M2, X.xyw ), Y.xyw ), NRD_EPS ) );
    float scale = dot( mul( M3, X.xy ), Y.xy ) * rcp( max( dot( mul( M4, X.xzw ), Y.xyw ), NRD_EPS ) );

    return saturate( Rf0 * scale + bias );
}

float _NRD_GetSpecMagicCurve( float roughness, float power )
{
    // https://www.desmos.com/calculator/fb1h5kiouj
    float f = 1.0 - exp2( -200.0 * roughness * roughness );
    f *= pow( saturate( roughness ), power );

    return f;
}

float _REBLUR_GetHitDistanceNormalization( float viewZ, float3 hitDistParams, float roughness )
{
    float smc = _NRD_GetSpecMagicCurve( roughness, 0.5 );

    return ( hitDistParams.x + abs( viewZ ) * hitDistParams.y ) * lerp( hitDistParams.z, 1.0, smc );
}

// Front-end usage: diffIrradiance /= diffFactor, specIrradiance /= specFactor. Back-end usage (after NRD):
// diffRadiance *= diffFactor, specRadiance *= specFactor. Same "roughness" for both.
void NRD_MaterialFactors( float3 N, float3 V, float3 albedo, float3 Rf0, float roughness, out float3 diffFactor, out float3 specFactor )
{
    float NoV = abs( dot( N, V ) );
    float3 Fenv = _NRD_EnvironmentTerm_Rtg( Rf0, NoV, roughness );

    diffFactor = ( 1.0 - Fenv ) * albedo;
    diffFactor = lerp( NRD_MATERIAL_FACTOR_MIN_SCALE.xxx, float3( 1.0, 1.0, 1.0 ), diffFactor );

    specFactor = Fenv;
    specFactor *= lerp( NRD_ROUGHNESS_FACTOR_MIN_SCALE.xxx, float3( 1.0, 1.0, 1.0 ), roughness ); // don't be greedy, it's a biased solution
    specFactor = lerp( NRD_MATERIAL_FACTOR_MIN_SCALE.xxx, float3( 1.0, 1.0, 1.0 ), specFactor );
}

// This function returns AO / SO which REBLUR can decode back to "hit distance" internally.
// Use this function only if a diffuse or specular lobe was not skipped due to probabilistic selection
float REBLUR_FrontEnd_GetNormHitDist( float hitDist, float viewZ, float3 hitDistParams, float roughness )
{
    float f = _REBLUR_GetHitDistanceNormalization( viewZ, hitDistParams, roughness );
    hitDist = saturate( hitDist / f );

    // "hitDist = 0" means "no data", i.e. the lobe is skipped due to probabilistic selection of diffuse or specular.
    // But if this function is called, we assume that the lobe was not skipped, thus we need to avoid 0
    return max( hitDist, NRD_EPS );
}

// X => IN_DIFF_RADIANCE_HITDIST, IN_SPEC_RADIANCE_HITDIST
float4 REBLUR_FrontEnd_PackRadianceAndNormHitDist( float3 radiance, float normHitDist, bool sanitize )
{
    if( sanitize )
    {
        radiance = _NRD_IsInvalid( radiance ) ? float3( 0, 0, 0 ) : clamp( radiance, 0, NRD_FP16_MAX );
        normHitDist = _NRD_IsInvalid( normHitDist ) ? 0 : saturate( normHitDist );
    }

    radiance = _NRD_LinearToYCoCg( radiance );

    return float4( radiance, normHitDist );
}

float3 _NRD_EncodeNormalRoughness101010( float3 n, float roughness )
{
    n /= abs( n.x ) + abs( n.y ) + abs( n.z );

    float3 r;
    r.y = n.y * 0.5 + 0.5;
    r.x = n.x * 0.5 + r.y;
    r.y -= n.x * 0.5;

    // Encode "roughness" and "n.z" sign into z-channel ( 10-bits is too much for roughness, linear at least )
    roughness = max( roughness, 1.5 / 512.0 ); // can't be 0 to not ruin "n.z" sign bit
    float s = n.z < 0 ? -roughness : roughness;
    r.z = s * 0.5 + 0.5;

    return r;
}

// X => IN_NORMAL_ROUGHNESS, for the library's NRDConfig.hlsli: NRD_NORMAL_ENCODING 2 (R10G10B10A2_UNORM),
// NRD_ROUGHNESS_ENCODING 1 (linear)
float4 NRD_FrontEnd_PackNormalAndRoughness( float3 N, float roughness, float materialID )
{
    float4 p;
    p.xyz = _NRD_EncodeNormalRoughness101010( N, roughness );
    p.w = saturate( materialID / 3.0 );

    return p;
}

// OUT_DIFF_RADIANCE_HITDIST, OUT_SPEC_RADIANCE_HITDIST => X
float4 REBLUR_BackEnd_UnpackRadianceAndNormHitDist( float4 data )
{
    data.xyz = _NRD_YCoCgToLinear( data.xyz );

    return data;
}

// X => IN_DIFF_RADIANCE_HITDIST, IN_SPEC_RADIANCE_HITDIST
float4 RELAX_FrontEnd_PackRadianceAndHitDist( float3 radiance, float hitDist, bool sanitize )
{
    if( sanitize )
    {
        radiance = _NRD_IsInvalid( radiance ) ? float3( 0, 0, 0 ) : clamp( radiance, 0, NRD_FP16_MAX );
        hitDist = _NRD_IsInvalid( hitDist ) ? 0 : clamp( hitDist, 0, NRD_FP16_MAX );
    }

    return float4( radiance, hitDist );
}

// OUT_DIFF_RADIANCE_HITDIST, OUT_SPEC_RADIANCE_HITDIST => X
float4 RELAX_BackEnd_UnpackRadiance( float4 color )
{
    return color;
}

// --- The engine's use of the above ---

// A de-modulated signal and its hit distance (not including the primary hit) for the denoiser it goes to; unfiltered
// (NRD_SIGNAL_RAW) it stays linear with the raw distance. hitDist 0: no sample ("no data" for NRD). roughness: 1 for
// diffuse (NRD: "isDiffuse ? 1.0 : roughness").
float4 NRD_PackSignal(uint mode, float3 radiance, float hitDist, float viewZ, float roughness)
{
    if (mode == NRD_SIGNAL_REBLUR)
    {
        float normHitDist = hitDist > 0.0 ? REBLUR_FrontEnd_GetNormHitDist(hitDist, viewZ, NRD_REBLUR_HIT_DISTANCE_PARAMETERS, roughness) : 0.0;
        return REBLUR_FrontEnd_PackRadianceAndNormHitDist(radiance, normHitDist, true);
    }
    return RELAX_FrontEnd_PackRadianceAndHitDist(radiance, hitDist, true);
}

// The de-modulated radiance of a signal packed (and maybe denoised) with NRD_PackSignal.
float3 NRD_UnpackSignal(uint mode, float4 data)
{
    if (mode == NRD_SIGNAL_REBLUR)
        return REBLUR_BackEnd_UnpackRadianceAndNormHitDist(data).xyz;
    return RELAX_BackEnd_UnpackRadiance(data).xyz;
}

#endif
