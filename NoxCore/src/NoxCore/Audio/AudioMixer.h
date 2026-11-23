#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include <NoxAudio/AudioMixer.h>

#include "NoxCore/Asset/Asset.h"
#include <NoxAudio/AudioEffects.h>
#include "AudioEngine.h"
#include <NoxAudio/AudioTypes.h>

namespace Nox
{
    using NoxAudio::AudioMixerEffect;
    using NoxAudio::AudioMixerSend;
    using NoxAudio::AudioMixerDuck;
    using NoxAudio::AudioMixerBus;
    using NoxAudio::AudioMixerValueKind;
    using NoxAudio::AudioMixerValueKey;
    using NoxAudio::AudioMixerSnapshot;
    using NoxAudio::AudioMixerData;
    using NoxAudio::CreateDefaultAudioMixer;
    using NoxAudio::DefaultEffectValues;
    using NoxAudio::SerializeAudioMixer;
    using NoxAudio::DeserializeAudioMixer;

    class AudioMixerAsset : public Asset
    {
    public:
        static AssetType GetStaticType() { return AssetType::AudioMixer; }
        AssetType GetType() const override { return GetStaticType(); }
        AudioMixerData Data;
    };

    class AudioMixer
    {
    public:
        static void Init() { NoxAudio::AudioMixer::Init(); }
        static void Shutdown() { NoxAudio::AudioMixer::Shutdown(); }
        static void LoadProjectMixer(bool create = false);
        static const std::filesystem::path& GetPath() { return NoxAudio::AudioMixer::GetPath(); }
        static bool Save() { return NoxAudio::AudioMixer::Save(); }
        static bool IsDirty() { return NoxAudio::AudioMixer::IsDirty(); }
        static AudioMixerData& GetData() { return NoxAudio::AudioMixer::GetData(); }
        static void NotifyStructureChanged() { NoxAudio::AudioMixer::NotifyStructureChanged(); }
        static void NotifyValuesChanged() { NoxAudio::AudioMixer::NotifyValuesChanged(); }
        static void MarkDirty() { NoxAudio::AudioMixer::MarkDirty(); }
        static void SetEditedSnapshot(int32_t index) { NoxAudio::AudioMixer::SetEditedSnapshot(index); }
        static int32_t GetEditedSnapshot() { return NoxAudio::AudioMixer::GetEditedSnapshot(); }
        static float GetValue(const AudioMixerValueKey& key) { return NoxAudio::AudioMixer::GetValue(key); }
        static void SetValue(const AudioMixerValueKey& key, float value) { NoxAudio::AudioMixer::SetValue(key, value); }
        static bool IsOverridden(const AudioMixerValueKey& key) { return NoxAudio::AudioMixer::IsOverridden(key); }
        static void ClearOverride(const AudioMixerValueKey& key) { NoxAudio::AudioMixer::ClearOverride(key); }
        static float GetLiveValue(const AudioMixerValueKey& key) { return NoxAudio::AudioMixer::GetLiveValue(key); }
        static void Update(float deltaSeconds) { NoxAudio::AudioMixer::Update(deltaSeconds); }
        static const AudioMixerBus* FindBus(std::string_view name) { return NoxAudio::AudioMixer::FindBus(name); }
        static const AudioMixerBus* FindBus(uint64_t id) { return NoxAudio::AudioMixer::FindBus(id); }
        static int32_t GetBusIndex(uint64_t id) { return NoxAudio::AudioMixer::GetBusIndex(id); }
        static void SetGameVolume(uint64_t bus, float volume) { NoxAudio::AudioMixer::SetGameVolume(bus, volume); }
        static float GetGameVolume(uint64_t bus) { return NoxAudio::AudioMixer::GetGameVolume(bus); }
        static void SetGameHeadphones(bool headphones) { NoxAudio::AudioMixer::SetGameHeadphones(headphones); }
        static bool GetHeadphones() { return NoxAudio::AudioMixer::GetHeadphones(); }
        static bool SetSnapshot(std::string_view name, float seconds) { return NoxAudio::AudioMixer::SetSnapshot(name, seconds); }
        static std::string GetSnapshot() { return NoxAudio::AudioMixer::GetSnapshot(); }
        static void ResetGameState() { NoxAudio::AudioMixer::ResetGameState(); }
        static AudioBusMeter GetMeter(uint64_t bus) { return NoxAudio::AudioMixer::GetMeter(bus); }
        static float GetEffectGainReduction(uint64_t bus, uint32_t effect) { return NoxAudio::AudioMixer::GetEffectGainReduction(bus, effect); }
    };
}
