#include "AudioTypes.h"

#include <algorithm>

#include <iterator>

namespace Nox
{
    float AudioAttenuationGain(AudioAttenuation attenuation, float minDistance, float maxDistance, float distance)
    {
        const float minimum = std::max(minDistance, 0.001f);
        const float maximum = std::max(maxDistance, minimum + 0.001f);
        if (distance <= minimum)
            return 1.0f;
        if (distance >= maximum)
            return 0.0f;
        switch (attenuation)
        {
        case AudioAttenuation::Inverse:
        {
            const float gain = minimum / distance;
            const float atMax = minimum / maximum;
            return (gain - atMax) / (1.0f - atMax);
        }
        case AudioAttenuation::InverseSquare:
        {
            const float gain = (minimum / distance) * (minimum / distance);
            const float atMax = (minimum / maximum) * (minimum / maximum);
            return (gain - atMax) / (1.0f - atMax);
        }
        case AudioAttenuation::Linear:
            return 1.0f - (distance - minimum) / (maximum - minimum);
        case AudioAttenuation::None:
            return 1.0f;
        }
        return 1.0f;
    }

    std::string_view AudioListenerReverbToString(AudioListenerReverb reverb)
    {
        switch (reverb)
        {
        case AudioListenerReverb::Off: return "Off";
        case AudioListenerReverb::Realtime: return "Realtime";
        case AudioListenerReverb::Baked: return "Baked";
        }
        return "Baked";
    }

    AudioListenerReverb AudioListenerReverbFromString(std::string_view name)
    {
        if (name == "Off") return AudioListenerReverb::Off;
        if (name == "Realtime") return AudioListenerReverb::Realtime;
        return AudioListenerReverb::Baked;
    }

    std::string_view AudioAttenuationToString(AudioAttenuation attenuation)
    {
        switch (attenuation)
        {
        case AudioAttenuation::Inverse: return "Inverse";
        case AudioAttenuation::InverseSquare: return "InverseSquare";
        case AudioAttenuation::Linear: return "Linear";
        case AudioAttenuation::None: return "None";
        }
        return "Inverse";
    }

    AudioAttenuation AudioAttenuationFromString(std::string_view name)
    {
        if (name == "InverseSquare") return AudioAttenuation::InverseSquare;
        if (name == "Linear") return AudioAttenuation::Linear;
        if (name == "None") return AudioAttenuation::None;
        return AudioAttenuation::Inverse;
    }

    std::string_view AudioLoadTypeToString(AudioLoadType type)
    {
        switch (type)
        {
        case AudioLoadType::Auto: return "Auto";
        case AudioLoadType::DecompressOnLoad: return "DecompressOnLoad";
        case AudioLoadType::CompressedInMemory: return "CompressedInMemory";
        case AudioLoadType::Streaming: return "Streaming";
        }
        return "Auto";
    }

    AudioLoadType AudioLoadTypeFromString(std::string_view name)
    {
        if (name == "DecompressOnLoad") return AudioLoadType::DecompressOnLoad;
        if (name == "CompressedInMemory") return AudioLoadType::CompressedInMemory;
        if (name == "Streaming") return AudioLoadType::Streaming;
        return AudioLoadType::Auto;
    }

    std::string_view AudioSpatializationToString(AudioSpatialization mode)
    {
        switch (mode)
        {
        case AudioSpatialization::Default: return "Default";
        case AudioSpatialization::Panning: return "Panning";
        case AudioSpatialization::Binaural: return "Binaural";
        }
        return "Default";
    }

    AudioSpatialization AudioSpatializationFromString(std::string_view name)
    {
        if (name == "Panning") return AudioSpatialization::Panning;
        if (name == "Binaural") return AudioSpatialization::Binaural;
        return AudioSpatialization::Default;
    }

    namespace
    {
        constexpr std::string_view kAcousticNames[] = { "Auto", "Generic", "Brick", "Concrete", "Ceramic", "Gravel", "Carpet", "Glass",
                                                        "Plaster", "Wood", "Metal", "Rock" };
        // Steam Audio's presets (its Unity / Unreal plugins' material assets): absorption low / mid / high, scattering, transmission.
        constexpr AcousticMaterialProperties kAcousticProperties[] = {
            { { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.100f, 0.050f, 0.030f } }, // (Auto: Generic)
            { { 0.10f, 0.20f, 0.30f }, 0.05f, { 0.100f, 0.050f, 0.030f } }, // Generic
            { { 0.03f, 0.04f, 0.07f }, 0.05f, { 0.015f, 0.015f, 0.015f } }, // Brick
            { { 0.05f, 0.07f, 0.08f }, 0.05f, { 0.015f, 0.002f, 0.001f } }, // Concrete
            { { 0.01f, 0.02f, 0.02f }, 0.05f, { 0.060f, 0.044f, 0.011f } }, // Ceramic
            { { 0.60f, 0.70f, 0.80f }, 0.05f, { 0.031f, 0.012f, 0.008f } }, // Gravel
            { { 0.24f, 0.69f, 0.73f }, 0.05f, { 0.020f, 0.005f, 0.003f } }, // Carpet
            { { 0.06f, 0.03f, 0.02f }, 0.05f, { 0.060f, 0.044f, 0.011f } }, // Glass
            { { 0.12f, 0.06f, 0.04f }, 0.05f, { 0.056f, 0.056f, 0.004f } }, // Plaster
            { { 0.11f, 0.07f, 0.06f }, 0.05f, { 0.070f, 0.014f, 0.005f } }, // Wood
            { { 0.20f, 0.07f, 0.06f }, 0.05f, { 0.200f, 0.025f, 0.010f } }, // Metal
            { { 0.13f, 0.20f, 0.24f }, 0.05f, { 0.015f, 0.002f, 0.001f } }, // Rock
        };
        static_assert(std::size(kAcousticNames) == static_cast<size_t>(AcousticMaterial::Count));
        static_assert(std::size(kAcousticProperties) == static_cast<size_t>(AcousticMaterial::Count));
    }

    std::string_view AcousticMaterialToString(AcousticMaterial material)
    {
        return material < AcousticMaterial::Count ? kAcousticNames[static_cast<size_t>(material)] : kAcousticNames[0];
    }

    AcousticMaterial AcousticMaterialFromString(std::string_view name)
    {
        for (size_t i = 0; i < std::size(kAcousticNames); ++i)
            if (kAcousticNames[i] == name)
                return static_cast<AcousticMaterial>(i);
        return AcousticMaterial::Auto;
    }

    const AcousticMaterialProperties& GetAcousticMaterialProperties(AcousticMaterial material)
    {
        return kAcousticProperties[material < AcousticMaterial::Count ? static_cast<size_t>(material) : 0];
    }
}
