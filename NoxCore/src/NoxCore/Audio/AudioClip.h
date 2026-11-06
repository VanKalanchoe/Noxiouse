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
        // Compressed In Memory / Streaming: the first second decoded (Unreal's stream priming): an audio graph's Wave Player starts and
        // loops from it at once while a stream gets ready for what follows (AudioClipStream).
        const std::vector<float>& GetPrimeSamples() const { return m_Prime; }
        uint64_t GetPrimeFrames() const { return m_Channels ? m_Prime.size() / m_Channels : 0; }
        size_t GetMemoryBytes() const { return (m_Samples.size() + m_Prime.size()) * sizeof(float) + m_Encoded.size(); }

    private:
        void LoadPrime();

        std::filesystem::path m_Path;
        uint32_t m_SampleRate = 0;
        uint32_t m_Channels = 0;
        uint64_t m_FrameCount = 0;
        AudioLoadType m_LoadType = AudioLoadType::DecompressOnLoad;
        std::vector<float> m_Samples;
        std::vector<uint8_t> m_Encoded;
        std::vector<float> m_Prime;
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

    // A Compressed In Memory or Streaming clip read from any frame, for an audio graph's Wave Player (docs/Audio_Architecture_Plan_
    // 2026.md §5): the mixer asks for (clip, frame) and reads stereo frames from a ring the engine's stream thread fills. Each request
    // has a generation: the stream thread resets the ring and seeks only between the mixer's reads, and the mixer reads only once the
    // stream has caught up with its latest request -- neither ever waits on the other.
    class AudioClipStream
    {
    public:
        explicit AudioClipStream(uint32_t ringFrames);
        ~AudioClipStream();
        AudioClipStream(const AudioClipStream&) = delete;
        AudioClipStream& operator=(const AudioClipStream&) = delete;

        // Mixer thread. `clip` must outlive the stream's use of it (the graph program holds its clips).
        void Request(const AudioClip* clip, uint64_t frame);
        bool IsReady() const;              // the latest request is being served: Read gives its frames
        uint32_t Read(float* stereo, uint32_t frames); // up to `frames` interleaved stereo frames; 0 when not ready or empty
        bool HasEnded() const;             // ready, the clip's end reached and every frame of it read
        const AudioClip* GetRequestedClip() const { return m_ConsumerClip; }
        uint64_t GetRequestedFrame() const { return m_ConsumerFrame; }

        // Stream thread: serves a new request, then decodes until the ring is full or the clip ends.
        void Fill();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        const AudioClip* m_ConsumerClip = nullptr; // (mixer thread: what it last asked for)
        uint64_t m_ConsumerFrame = 0;
        uint32_t m_ConsumerGeneration = 0;
    };
}
