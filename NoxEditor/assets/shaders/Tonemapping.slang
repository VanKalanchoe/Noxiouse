module common;

    public enum TonemapMode : int
    {
        None = 0,
        ACES_Narkowicz = 1,
        ACES_Hill = 2,
        ACES_Hill_ExposureBoost = 3,
        KhronosPbrNeutral = 4,
        Reinhard = 5,
        ReinhardModified = 6,
        HejiHableAlu = 7,
        HableUc2 = 8,
        ACES_RTXPT = 9
    };

    public struct TonemapParameters
    {
        public float exposure;
        public float gamma;
        public int mode;
    };

    // =============================================================================
    // COLOR SPACE HELPERS (IEC 61966-2-1 Standard sRGB Transfer Function)
    // =============================================================================

    public float linearTosRGB(float lin)
    {
        return (lin <= 0.0031308) ? (lin * 12.92) : (pow(max(lin, 0.0), 1.0 / 2.4) * 1.055 - 0.055);
    }

    public float3 linearTosRGB(float3 color)
    {
        return float3(
            linearTosRGB(color.r),
            linearTosRGB(color.g),
            linearTosRGB(color.b)
        );
    }

    public float sRGBToLinear(float srgb)
    {
        return (srgb <= 0.04045) ? (srgb / 12.92) : pow((max(srgb, 0.0) + 0.055) / 1.055, 2.4);
    }

    public float3 sRGBToLinear(float3 color)
    {
        return float3(
            sRGBToLinear(color.r),
            sRGBToLinear(color.g),
            sRGBToLinear(color.b)
        );
    }

    public float4 sRGBToLinear(float4 srgbIn)
    {
        return float4(sRGBToLinear(srgbIn.xyz), srgbIn.w);
    }

    // =============================================================================
    // TONEMAPPING OPERATORS (from glTF-Sample-Viewer & NVIDIA RTXPT)
    // =============================================================================

    float calcLuminance(float3 color)
    {
        return dot(color, float3(0.299, 0.587, 0.114));
    }

    // Reinhard
    public float3 toneMapReinhard(float3 color)
    {
        float luminance = calcLuminance(color);
        float reinhard = luminance / (luminance + 1.0);
        return color * (reinhard / max(luminance, 1e-6));
    }

    // Reinhard with maximum luminance
    public float3 toneMapReinhardModified(float3 color, float whiteMaxLuminance = 1.0)
    {
        float luminance = calcLuminance(color);
        float reinhard = luminance * (1.0 + luminance / (whiteMaxLuminance * whiteMaxLuminance)) / (1.0 + luminance);
        return color * (reinhard / max(luminance, 1e-6));
    }

    // John Hable's ALU approximation of Jim Heji's operator (filmic)
    public float3 toneMapHejiHableAlu(float3 color)
    {
        color = max(float3(0.0), color - 0.004);
        color = (color * (6.2 * color + 0.5)) / (color * (6.2 * color + 1.7) + 0.06);
        return pow(color, float3(2.2));
    }

    // John Hable's Uncharted 2 filmic tone map curve
    float3 applyUc2Curve(float3 color)
    {
        float A = 0.22; // Shoulder Strength
        float B = 0.3;  // Linear Strength
        float C = 0.1;  // Linear Angle
        float D = 0.2;  // Toe Strength
        float E = 0.01; // Toe Numerator
        float F = 0.3;  // Toe Denominator

        color = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - (E / F);
        return color;
    }

    // Hable UC2 (NVIDIA RTXPT Uncharted 2 filmic operator)
    public float3 toneMapHableUc2(float3 color, float whiteScale = 5.1)
    {
        float exposureBias = 2.0;
        color = applyUc2Curve(exposureBias * color);
        float ws = 1.0 / applyUc2Curve(float3(whiteScale)).x;
        color = color * ws;
        return color;
    }

    // ACES (NVIDIA RTXPT standard - Narkowicz with 0.6 pre-exposure cancellation)
    public float3 toneMapACES_RTXPT(float3 color)
    {
        color *= 0.6;
        const float A = 2.51;
        const float B = 0.03;
        const float C = 2.43;
        const float D = 0.59;
        const float E = 0.14;
        return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
    }

    // ACES tone map (faster approximation)
    public float3 toneMapACES_Narkowicz(float3 color)
    {
        const float A = 2.51;
        const float B = 0.03;
        const float C = 2.43;
        const float D = 0.59;
        const float E = 0.14;
        return clamp((color * (A * color + B)) / (color * (C * color + D) + E), 0.0, 1.0);
    }

    // ACES filmic tone map approximation
    float3 RRTAndODTFit(float3 color)
    {
        float3 a = color * (color + 0.0245786) - 0.000090537;
        float3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
        return a / b;
    }

    // sRGB => XYZ => D65_2_D60 => AP1 => RRT_SAT
    static const float3x3 ACESInputMat = float3x3(
        0.59719, 0.35458, 0.04823,
        0.07600, 0.90834, 0.01566,
        0.02840, 0.13383, 0.83777
    );

    // ODT_SAT => XYZ => D60_2_D65 => sRGB
    static const float3x3 ACESOutputMat = float3x3(
        1.60475, -0.53108, -0.07367,
       -0.10208,  1.10813, -0.00605,
       -0.00327, -0.07276,  1.07602
    );

    public float3 toneMapACES_Hill(float3 color)
    {
        color = mul(ACESInputMat, color);
        color = RRTAndODTFit(color);
        color = mul(ACESOutputMat, color);
        return clamp(color, 0.0, 1.0);
    }

    // Khronos PBR neutral tone mapping
    public float3 toneMap_KhronosPbrNeutral(float3 color)
    {
        const float startCompression = 0.8 - 0.04;
        const float desaturation = 0.15;

        float x = min(color.r, min(color.g, color.b));
        float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
        color -= offset;

        float peak = max(color.r, max(color.g, color.b));
        if (peak < startCompression) return color;

        const float d = 1.0 - startCompression;
        float newPeak = 1.0 - d * d / (peak + d - startCompression);
        color *= newPeak / peak;

        float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
        return lerp(color, float3(newPeak), g);
    }

    // Combined toneMap using TonemapParameters
    public float3 toneMap(float3 color, TonemapParameters params)
    {
        // 1. Exposure
        color *= params.exposure;

        // 2. Tonemapping curve
        float3 tonemapped;

        switch (params.mode)
        {
        case (int)TonemapMode::None:
            // Linear mapping clamped at 1.0
            tonemapped = clamp(color, 0.0, 1.0);
            break;

        case (int)TonemapMode::ACES_Narkowicz:
            tonemapped = toneMapACES_Narkowicz(color);
            break;

        case (int)TonemapMode::ACES_Hill:
            tonemapped = toneMapACES_Hill(color);
            break;

        case (int)TonemapMode::ACES_Hill_ExposureBoost:
            tonemapped = toneMapACES_Hill(color / 0.6);
            break;

        case (int)TonemapMode::Reinhard:
            tonemapped = toneMapReinhard(color);
            break;

        case (int)TonemapMode::ReinhardModified:
            tonemapped = toneMapReinhardModified(color);
            break;

        case (int)TonemapMode::HejiHableAlu:
            tonemapped = toneMapHejiHableAlu(color);
            break;

        case (int)TonemapMode::HableUc2:
            tonemapped = toneMapHableUc2(color);
            break;

        case (int)TonemapMode::ACES_RTXPT:
            tonemapped = toneMapACES_RTXPT(color);
            break;

        case (int)TonemapMode::KhronosPbrNeutral:
        default:
            tonemapped = toneMap_KhronosPbrNeutral(color);
            break;
        }

        // 3. Dynamic Gamma adjustment (relative to 2.2 standard display)
        // Since ImageFormat::Surface / swapchain is VK_FORMAT_B8G8R8A8_SRGB, the hardware ROP converts
        // linear float values to sRGB automatically.
        // If gamma == 2.2, factor is 1.0 (exact neutral). If slider changes, it brightens/darkens midtones.
        if (abs(params.gamma - 2.2) > 0.01 && params.gamma > 0.0)
        {
            tonemapped = pow(max(tonemapped, float3(0.0)), float3(2.2 / params.gamma));
        }

        return saturate(tonemapped);
    }

    // =============================================================================
    // INVERSE TONEMAPPING FUNCTIONS (from glTF-Sample-Viewer)
    // =============================================================================

    // Precomputed exact inverse matrices (replaces missing inverse() in Slang)
    static const float3x3 ACESInputMatInv = float3x3(
         1.764736, -0.675776, -0.088963,
        -0.147027,  1.160249, -0.013224,
        -0.036337, -0.162436,  1.198770
    );

    static const float3x3 ACESOutputMatInv = float3x3(
        0.643035, 0.311184, 0.045776,
        0.059268, 0.931431, 0.009295,
        0.005961, 0.063930, 0.930114
    );

    // Inverse ACES tone map (Narkowicz approximation)
    public float3 toneMapACES_NarkowiczInverse(float3 toneMapped)
    {
        const float A = 2.51;
        const float B = 0.03;
        const float C = 2.43;
        const float D = 0.59;
        const float E = 0.14;

        float3 y = toneMapped;
        float3 a = y * C - A;
        float3 b = y * D - B;
        float3 c = y * E;

        float3 discriminant = b * b - 4.0 * a * c;
        return (-b - sqrt(max(discriminant, float3(0.0)))) / (2.0 * a);
    }

    // Inverse RRT and ODT fit
    float3 RRTAndODTFitInverse(float3 toneMapped)
    {
        float3 y = toneMapped;
        float3 a = y * 0.983729 - 1.0;
        float3 b = y * 0.4329510 - 0.0245786;
        float3 c = y * 0.238081 + 0.000090537;

        float3 discriminant = b * b - 4.0 * a * c;
        return (-b - sqrt(max(discriminant, float3(0.0)))) / (2.0 * a);
    }

    // Inverse ACES Hill tone mapping (using precomputed inverse matrices)
    public float3 toneMapACES_HillInverse(float3 toneMapped)
    {
        float3 color = mul(ACESOutputMatInv, toneMapped);
        color = RRTAndODTFitInverse(color);
        color = mul(ACESInputMatInv, color);
        return color;
    }

    // Inverse Khronos PBR neutral tone mapping
    public float3 toneMap_KhronosPbrNeutralInverse(float3 toneMapped)
    {
        const float startCompression = 0.8 - 0.04;
        float3 color = toneMapped;
        float peak = max(color.r, max(color.g, color.b));

        if (peak >= startCompression)
        {
            float originalPeak = peak / (1.0 - peak + startCompression);
            color *= originalPeak / peak;
        }

        float x = min(color.r, min(color.g, color.b));
        float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
        color += offset;

        return color;
    }

    // Complete inverse tone mapping
    public float3 toneMapInverse(float3 toneMapped, TonemapParameters params)
    {
        // 1. Undo dynamic gamma (if modified from 2.2)
        if (abs(params.gamma - 2.2) > 0.01 && params.gamma > 0.0)
        {
            toneMapped = pow(max(toneMapped, float3(0.0)), float3(params.gamma / 2.2));
        }

        // 2. Undo sRGB to get linear LDR
        float3 color = (params.mode == (int)TonemapMode::HejiHableAlu) ? toneMapped : sRGBToLinear(toneMapped);

        // 3. Undo tonemapper
        switch (params.mode)
        {
        case (int)TonemapMode::None:
            color = clamp(color, 0.0, 1.0);
            break;
        case (int)TonemapMode::ACES_Narkowicz:
            color = toneMapACES_NarkowiczInverse(color);
            break;
        case (int)TonemapMode::ACES_Hill:
            color = toneMapACES_HillInverse(color);
            break;
        case (int)TonemapMode::ACES_Hill_ExposureBoost:
            color = toneMapACES_HillInverse(color) * 0.6;
            break;
        case (int)TonemapMode::ACES_RTXPT:
            color = toneMapACES_NarkowiczInverse(color) / 0.6;
            break;
        case (int)TonemapMode::KhronosPbrNeutral:
        default:
            color = toneMap_KhronosPbrNeutralInverse(color);
            break;
        }

        // 4. Undo exposure
        if (params.exposure > 0.0)
        {
            color /= params.exposure;
        }
        return color;
    }