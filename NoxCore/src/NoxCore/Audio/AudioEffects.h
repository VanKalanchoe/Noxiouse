#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Nox
{
    class AudioClip;

    // The effects a mixer bus runs in its chain and an audio graph runs as nodes (docs/Audio_Architecture_Plan_2026.md §7, phase
    // A3): one implementation each, stereo, block by block on the mixer thread. Unreal's submix effects and MetaSounds' nodes, Unity's
    // mixer effects, Godot's bus effects cover the same set.
    enum class AudioEffectType : uint8_t
    {
        EQ = 0,            // four bands: low shelf, two peaks, high shelf
        Filter,            // low / high / band pass (a snapshot's muffle: Pause, Underwater)
        Compressor,        // feed-forward, soft knee, optional sidechain (another bus as the key: ducking)
        Limiter,           // look-ahead brickwall: nothing passes its ceiling (Master's last effect)
        Gate,              // closes below a threshold, with hold
        Delay,             // stereo delay: normal / ping pong / cross feedback, damped
        Reverb,            // algorithmic: early reflections + an 8-line feedback delay network
        ConvolutionReverb, // a recorded space: an impulse response clip, partitioned FFT convolution
        Chorus,
        Flanger,
        Phaser,
        Distortion,        // soft / hard clip, foldback, tube; oversampled 2x
        Bitcrusher,
        Count
    };

    struct AudioEffectParameterInfo
    {
        std::string Name;
        float Min = 0.0f;
        float Max = 1.0f;
        float Default = 0.0f;
        std::string Unit;                  // "dB", "Hz", "ms", "s", "" ...
        bool Logarithmic = false;          // a slider in octaves / decades (frequencies, times)
        bool Toggle = false;               // 0 / 1, a checkbox
        std::vector<std::string> Options;  // a choice: the value is the option's index
    };

    struct AudioEffectInfo
    {
        AudioEffectType Type = AudioEffectType::EQ;
        std::string Name;        // as saved and shown ("Convolution Reverb")
        std::string Description; // a tooltip line
        std::vector<AudioEffectParameterInfo> Parameters;
        bool Sidechain = false;           // takes a key signal (a bus's sidechain source, a graph node's Sidechain input)
        bool ImpulseResponse = false;     // takes an audio clip as its impulse response
        bool ReportsGainReduction = false;
    };

    const AudioEffectInfo& GetAudioEffectInfo(AudioEffectType type);
    std::string_view AudioEffectTypeToString(AudioEffectType type);
    // Count when the name is not an effect.
    AudioEffectType AudioEffectTypeFromString(std::string_view name);
    // -1 when the effect has no such parameter.
    int32_t FindAudioEffectParameter(AudioEffectType type, std::string_view name);

    // One effect instance. Created on the game thread (CreateAudioEffect: it allocates its delay lines, FFT tables, impulse response
    // partitions), run on the mixer thread: SetParameter and Process never allocate or lock. Parameters are raw values, as
    // AudioEffectInfo describes them; each effect smooths them itself (gains ramped across a block, frequencies in octaves, delay
    // times slewed), so a fader or a snapshot moving them never clicks.
    class AudioEffect
    {
    public:
        static constexpr uint32_t MaxFrames = 256; // a block (the engine's and the audio graph's)

        virtual ~AudioEffect() = default;
        AudioEffect(const AudioEffect&) = delete;
        AudioEffect& operator=(const AudioEffect&) = delete;

        AudioEffectType GetType() const { return m_Type; }
        void SetParameter(uint32_t index, float value)
        {
            if (index < m_Values.size())
                m_Values[index] = value;
        }
        float GetParameter(uint32_t index) const { return index < m_Values.size() ? m_Values[index] : 0.0f; }

        // In place, `frames` <= MaxFrames (the Convolution Reverb needs exactly MaxFrames). `keyLeft` / `keyRight`: the sidechain's
        // signal for effects that take one (null: the input itself).
        virtual void Process(float* left, float* right, uint32_t frames, const float* keyLeft, const float* keyRight) = 0;
        // Clears delay lines, envelopes and filter states (a tail cut off).
        virtual void Reset() = 0;

        // Compressor / Limiter / Gate: how far the gain was turned down in the last block, dB (positive). Read from any thread.
        float GetGainReductionDb() const { return m_GainReduction.load(std::memory_order_relaxed); }

    protected:
        AudioEffect(AudioEffectType type, float sampleRate);
        float Value(uint32_t index) const { return m_Values[index]; }

        AudioEffectType m_Type;
        float m_SampleRate;
        std::vector<float> m_Values;
        std::atomic<float> m_GainReduction{ 0.0f };
    };

    // Game thread: an effect running at `sampleRate`, its parameters at their defaults. `impulse`: the Convolution Reverb's impulse
    // response (mono or stereo, any rate -- resampled; up to 10 seconds used); without one it is silent.
    std::unique_ptr<AudioEffect> CreateAudioEffect(AudioEffectType type, float sampleRate, const AudioClip* impulse = nullptr);
}
