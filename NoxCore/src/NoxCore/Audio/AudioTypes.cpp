#include "AudioTypes.h"

namespace Nox
{
    std::string_view AudioBusToString(AudioBus bus)
    {
        switch (bus)
        {
        case AudioBus::Master: return "Master";
        case AudioBus::Music: return "Music";
        case AudioBus::SFX: return "SFX";
        case AudioBus::Voice: return "Voice";
        case AudioBus::UI: return "UI";
        case AudioBus::Ambience: return "Ambience";
        default: return "Master";
        }
    }

    AudioBus AudioBusFromString(std::string_view name)
    {
        for (uint8_t i = 0; i < static_cast<uint8_t>(AudioBus::Count); ++i)
        {
            if (AudioBusToString(static_cast<AudioBus>(i)) == name)
                return static_cast<AudioBus>(i);
        }
        return AudioBus::Master;
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
}
