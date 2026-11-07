#include "AudioTypes.h"

namespace Nox
{
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
}
