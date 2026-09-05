module common;

    public enum TonemapMode : int
    {
        None = 0,
        ACES_Narkowicz = 1,
        ACES_Hill = 2,
        ACES_Hill_ExposureBoost = 3,
        KhronosPbrNeutral = 4
    };

    public struct TonemapParameters
    {
        public float exposure;
        public float gamma;
        public int mode;
    };

    // =============================================================================
    // COLOR SPACE HELPERS (Dynamic Gamma)
    // =============================================================================

    public float3 linearTosRGB(float3 color, float gamma = 2.2)
    {
        return pow(max(color, float3(0.0)), float3(1.0 / gamma));
    }

    public float3 sRGBToLinear(float3 srgbIn, float gamma = 2.2)
    {
        return pow(max(srgbIn, float3(0.0)), float3(gamma));
    }

    public float4 sRGBToLinear(float4 srgbIn, float gamma = 2.2)
    {
        return float4(sRGBToLinear(srgbIn.xyz, gamma), srgbIn.w);
    }

    // =============================================================================
    // FORWARD TONEMAPPING OPERATORS (from glTF-Sample-Viewer)
    // =============================================================================

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

    // Combined toneMap matching glTF-Sample-Viewer using TonemapParameters
    public float3 toneMap(float3 color, TonemapParameters params)
    {
        // 1. Exposure
        color *= params.exposure;

        // 2. Tonemapping curve
        float3 tonemapped;
        switch (params.mode)
        {

			case (int)TonemapMode::None:
                // Linear mapping clamped at 1.0 (exact glTF Sample Viewer "None" mode)
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

        case (int)TonemapMode::KhronosPbrNeutral:
        default:
            tonemapped = toneMap_KhronosPbrNeutral(color);
            break;
        }

        // 3. Dynamic Gamma (relative to 2.2 standard display)
        // If gamma == 2.2, factor is 1.0 (exact neutral)
        // If gamma changes via slider, it brightens/darkens midtones accordingly
        if (params.gamma > 0.0)
        {
            tonemapped = pow(max(tonemapped, float3(0.0)), float3(2.2 / params.gamma));
        }

        return tonemapped;
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
        // 1. Undo gamma
        if (params.gamma > 0.0)
        {
            toneMapped = pow(max(toneMapped, float3(0.0)), float3(params.gamma / 2.2));
        }

        // 2. Undo tonemapper
        float3 color = toneMapped;
        switch (params.mode)
        {
			case (int)TonemapMode::None:
                color = clamp(toneMapped, 0.0, 1.0);
                break;
        case (int)TonemapMode::ACES_Narkowicz:
            color = toneMapACES_NarkowiczInverse(color);
            break;
        case (int)TonemapMode::ACES_Hill:
            color = toneMapACES_HillInverse(color);
            break;
        case (int)TonemapMode::ACES_Hill_ExposureBoost:
            color = toneMapACES_HillInverse(color);
            color *= 0.6;
            break;
        case (int)TonemapMode::KhronosPbrNeutral:
        default:
            color = toneMap_KhronosPbrNeutralInverse(color);
            break;
        }

        // 3. Undo exposure
        if (params.exposure > 0.0)
        {
            color /= params.exposure;
        }
        return color;
    }