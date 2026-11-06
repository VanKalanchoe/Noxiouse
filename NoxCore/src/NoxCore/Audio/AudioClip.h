#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "NoxCore/Core/core.h"
#include "AudioTypes.h"

namespace Nox
{
    // A sound file as an asset (docs/Audio_Architecture_Plan_2026.md §3): the .wav / .ogg / .flac / .mp3 itself, as Godot keeps
    // them -- packaged as it is (Ogg, MP3 and FLAC are already compressed; WAV is the uncompressed choice for short sounds). Its Load
    // Type (an import setting in the asset registry, like a texture's compression) decides how it is held while it plays.
    class AudioClip : public Asset
    {
    public:
        static AssetType GetStaticType() { return AssetType::AudioClip; }
        AssetType GetType() const override { return GetStaticType(); }

        // Reads `path` through the VFS (a loose file, or the entry of a mounted pack). Null when it is not a sound miniaudio decodes.
        static Ref<AudioClip> Load(const std::filesystem::path& path, AudioLoadType loadType);

        const std::filesystem::path& GetPath() const { return m_Path; }
        uint32_t GetSampleRate() const { return m_SampleRate; }
        uint32_t GetChannels() const { return m_Channels; } // 1 or 2 (more are downmixed to stereo)
        uint64_t GetFrameCount() const { return m_FrameCount; }
        float GetDuration() const { return m_SampleRate ? static_cast<float>(static_cast<double>(m_FrameCount) / m_SampleRate) : 0.0f; }
        // Never Auto: what Auto chose for this clip.
        AudioLoadType GetLoadType() const { return m_LoadType; }
        bool IsDecoded() const { return m_LoadType == AudioLoadType::DecompressOnLoad; }

        // Decompress On Load: the samples, interleaved floats (GetFrameCount() * GetChannels()).
        const std::vector<float>& GetSamples() const { return m_Samples; }
        // Compressed In Memory: the file's bytes.
        const std::vector<uint8_t>& GetEncodedBytes() const { return m_Encoded; }
        size_t GetMemoryBytes() const { return m_Samples.size() * sizeof(float) + m_Encoded.size(); }

    private:
        std::filesystem::path m_Path;
        uint32_t m_SampleRate = 0;
        uint32_t m_Channels = 0;
        uint64_t m_FrameCount = 0;
        AudioLoadType m_LoadType = AudioLoadType::DecompressOnLoad;
        std::vector<float> m_Samples;
        std::vector<uint8_t> m_Encoded;
    };

    // Decodes a Compressed In Memory or Streaming clip as it plays (AudioEngine's stream thread, one per playing voice): floats at
    // the clip's own rate and channel count. Holds a reference to the clip.
    class AudioClipDecoder
    {
    public:
        AudioClipDecoder();
        ~AudioClipDecoder();
        AudioClipDecoder(const AudioClipDecoder&) = delete;
        AudioClipDecoder& operator=(const AudioClipDecoder&) = delete;

        bool Open(const Ref<AudioClip>& clip);
        // Up to `frames` frames into `out`; fewer only at the end of the sound.
        uint64_t Read(float* out, uint64_t frames);
        bool Seek(uint64_t frame);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
