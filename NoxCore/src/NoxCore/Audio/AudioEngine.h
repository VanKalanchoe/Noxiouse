#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <NoxAudio/AudioEngine.h>

#include "NoxCore/NodeGraph/NodeGraphTypes.h"
#include "AudioClip.h"
#include <NoxAudio/AudioEffects.h>
#include <NoxAudio/AudioTypes.h>

namespace Nox
{
    using NoxAudio::AudioSpatialSettings;
    using NoxAudio::AudioReflectionSettings;
    using NoxAudio::AudioVoiceEnvironment;
    using NoxAudio::AudioStats;
    using NoxAudio::AudioBusMeter;
    using NoxAudio::AudioGraphProgram;

    struct AudioPlayDesc
    {
        Ref<AudioClip> Clip;
        std::shared_ptr<const AudioGraphProgram> Graph;
        std::unordered_map<std::string, NodeGraphValue> GraphParameters;
        float Volume = 1.0f;
        float Pitch = 1.0f;
        bool Loop = false;
        uint64_t Bus = NoxAudio::kAudioBusSFX;
        NoxAudio::AudioVoiceGroup Group = NoxAudio::AudioVoiceGroup::Game;
        bool Spatial = false;
        glm::dvec3 Position{ 0.0 };
        AudioSpatialSettings Spatialization;
        float StartTime = 0.0f;
        float FadeIn = 0.0f;
        AudioVoiceEnvironment Environment;
        uint64_t SoundAsset = 0;
        NoxAudio::AudioSoundSettings Sound;
    };

    class AudioEngine
    {
    public:
        static constexpr uint32_t MaxVoices = NoxAudio::AudioEngine::MaxVoices;
        static constexpr uint32_t BlockFrames = NoxAudio::AudioEngine::BlockFrames;
        static constexpr uint32_t MaxBuses = NoxAudio::AudioEngine::MaxBuses;
        using ReflectionStats = NoxAudio::AudioEngine::ReflectionStats;

        static void Init(bool openDevice = true);
        static void Shutdown() { NoxAudio::AudioEngine::Shutdown(); }
        static bool IsAvailable() { return NoxAudio::AudioEngine::IsAvailable(); }

        static NoxAudio::AudioVoiceHandle Play(const AudioPlayDesc& desc);
        static void Stop(NoxAudio::AudioVoiceHandle voice, float fadeOutSeconds = 0.0f) { NoxAudio::AudioEngine::Stop(voice, fadeOutSeconds); }
        static bool IsPlaying(NoxAudio::AudioVoiceHandle voice) { return NoxAudio::AudioEngine::IsPlaying(voice); }
        static void SetVolume(NoxAudio::AudioVoiceHandle voice, float volume) { NoxAudio::AudioEngine::SetVolume(voice, volume); }
        static void SetPitch(NoxAudio::AudioVoiceHandle voice, float pitch) { NoxAudio::AudioEngine::SetPitch(voice, pitch); }
        static void SetPosition(NoxAudio::AudioVoiceHandle voice, const glm::dvec3& position) { NoxAudio::AudioEngine::SetPosition(voice, position); }
        static void SetSpatialization(NoxAudio::AudioVoiceHandle voice, const AudioSpatialSettings& settings) { NoxAudio::AudioEngine::SetSpatialization(voice, settings); }
        static void SetLoop(NoxAudio::AudioVoiceHandle voice, bool loop) { NoxAudio::AudioEngine::SetLoop(voice, loop); }
        static void SetPaused(NoxAudio::AudioVoiceHandle voice, bool paused) { NoxAudio::AudioEngine::SetPaused(voice, paused); }
        static void SetGraphParameter(NoxAudio::AudioVoiceHandle voice, int32_t index, float value) { NoxAudio::AudioEngine::SetGraphParameter(voice, index, value); }
        static void FireGraphParameter(NoxAudio::AudioVoiceHandle voice, int32_t index) { NoxAudio::AudioEngine::FireGraphParameter(voice, index); }
        static void SetGraphConstant(NoxAudio::AudioVoiceHandle voice, uint32_t slot, float value) { NoxAudio::AudioEngine::SetGraphConstant(voice, slot, value); }
        static glm::vec2 GetVoicePeak(NoxAudio::AudioVoiceHandle voice) { return NoxAudio::AudioEngine::GetVoicePeak(voice); }
        static uint32_t GetSampleRate() { return NoxAudio::AudioEngine::GetSampleRate(); }
        static void SetVoiceEnvironment(NoxAudio::AudioVoiceHandle voice, const AudioVoiceEnvironment& environment) { NoxAudio::AudioEngine::SetVoiceEnvironment(voice, environment); }
        static void StopGroup(NoxAudio::AudioVoiceGroup group, float fadeOutSeconds = 0.0f) { NoxAudio::AudioEngine::StopGroup(group, fadeOutSeconds); }
        static void SetGroupPaused(NoxAudio::AudioVoiceGroup group, bool paused) { NoxAudio::AudioEngine::SetGroupPaused(group, paused); }
        static void SetListener(const glm::dvec3& position, const glm::quat& orientation) { NoxAudio::AudioEngine::SetListener(position, orientation); }
        static void SetHeadphones(bool headphones) { NoxAudio::AudioEngine::SetHeadphones(headphones); }
        static void SetReflections(const AudioReflectionSettings& settings) { NoxAudio::AudioEngine::SetReflections(settings); }
        static float GetReflectionMilliseconds() { return NoxAudio::AudioEngine::GetReflectionMilliseconds(); }
        static ReflectionStats GetReflectionStats() { return NoxAudio::AudioEngine::GetReflectionStats(); }
        static bool GetHeadphones() { return NoxAudio::AudioEngine::GetHeadphones(); }
        static void SetMaxRealVoices(uint32_t count) { NoxAudio::AudioEngine::SetMaxRealVoices(count); }
        static uint32_t GetMaxRealVoices() { return NoxAudio::AudioEngine::GetMaxRealVoices(); }
        static void SetBusGain(uint32_t bus, float gain) { NoxAudio::AudioEngine::SetBusGain(bus, gain); }
        static void SetEffectParameter(uint32_t bus, uint32_t effect, uint32_t parameter, float value) { NoxAudio::AudioEngine::SetEffectParameter(bus, effect, parameter, value); }
        static void SetEffectBypass(uint32_t bus, uint32_t effect, bool bypass) { NoxAudio::AudioEngine::SetEffectBypass(bus, effect, bypass); }
        static void SetSendGain(uint32_t bus, uint32_t send, float gain) { NoxAudio::AudioEngine::SetSendGain(bus, send, gain); }
        static AudioBusMeter GetBusMeter(uint32_t bus) { return NoxAudio::AudioEngine::GetBusMeter(bus); }
        static float GetEffectGainReduction(uint32_t bus, uint32_t effect) { return NoxAudio::AudioEngine::GetEffectGainReduction(bus, effect); }
        static uint32_t GetBusCount() { return NoxAudio::AudioEngine::GetBusCount(); }
        static void Update(float deltaSeconds);
        static AudioStats GetStats() { return NoxAudio::AudioEngine::GetStats(); }
    };
}
