#pragma once

#include <filesystem>
#include <vector>

#include <NoxAudio/AudioClip.h>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/core.h"
#include <NoxAudio/AudioTypes.h>

namespace Nox
{
    // Thin Nox::Asset wrapper around NoxAudio::AudioClip so clips live in AssetManager.
    class AudioClip : public Asset
    {
    public:
        explicit AudioClip(NoxAudio::Ref<NoxAudio::AudioClip> inner) : m_Inner(std::move(inner)) {}

        static AssetType GetStaticType() { return AssetType::AudioClip; }
        AssetType GetType() const override { return GetStaticType(); }

        static Ref<AudioClip> Load(const std::filesystem::path& path, NoxAudio::AudioLoadType loadType);

        const NoxAudio::Ref<NoxAudio::AudioClip>& Inner() const { return m_Inner; }

        const std::filesystem::path& GetPath() const { return m_Inner->GetPath(); }
        uint32_t GetSampleRate() const { return m_Inner->GetSampleRate(); }
        uint32_t GetChannels() const { return m_Inner->GetChannels(); }
        uint64_t GetFrameCount() const { return m_Inner->GetFrameCount(); }
        float GetDuration() const { return m_Inner->GetDuration(); }
        NoxAudio::AudioLoadType GetLoadType() const { return m_Inner->GetLoadType(); }
        bool IsDecoded() const { return m_Inner->IsDecoded(); }
        const std::vector<float>& GetSamples() const { return m_Inner->GetSamples(); }
        const std::vector<uint8_t>& GetEncodedBytes() const { return m_Inner->GetEncodedBytes(); }
        const std::vector<float>& GetPrimeSamples() const { return m_Inner->GetPrimeSamples(); }
        uint64_t GetPrimeFrames() const { return m_Inner->GetPrimeFrames(); }
        size_t GetMemoryBytes() const { return m_Inner->GetMemoryBytes(); }

    private:
        NoxAudio::Ref<NoxAudio::AudioClip> m_Inner;
    };

    using NoxAudio::AudioClipDecoder;
    using NoxAudio::AudioClipStream;
}
