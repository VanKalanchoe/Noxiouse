#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "NoxCore/Asset/Asset.h"
#include "AudioEffects.h"
#include "AudioEngine.h"
#include "AudioTypes.h"

namespace Nox
{
    // The mixer (docs/Audio_Architecture_Plan_2026.md §7, phase A3): where every sound goes. A tree of buses under Master, each with
    // a fader, an effect chain, sends to other buses and ducking; snapshots that blend the mix into another state (Pause, Underwater).
    // One per project, an asset (.nmixer, AssetType::AudioMixer) at the project's Audio Mixer path; edited in the Mixer panel.
    // Unity's Audio Mixer, Godot's audio buses, Unreal's submixes and FMOD's / Wwise's mixers are this.

    struct AudioMixerEffect
    {
        uint64_t Id = 0;
        AudioEffectType Type = AudioEffectType::EQ;
        bool Bypass = false;
        std::vector<float> Values;   // by parameter index (AudioEffectInfo), what the Default mix has
        AssetHandle Impulse = 0;     // Convolution Reverb: its impulse response clip
        uint64_t Sidechain = 0;      // Compressor / Gate: the bus whose output is the key (0: its own input)
    };

    struct AudioMixerSend
    {
        uint64_t Target = 0;
        float LevelDb = 0.0f;
        bool PreFader = false;       // taken before the bus's fader (still sends when the bus is turned down)
    };

    // "Lower this bus while that one plays" (Wwise's auto-ducking, Unreal's submix sidechain): while Source's output is above the
    // threshold, this bus goes down by Depth dB in Attack seconds, back up in Release seconds.
    struct AudioMixerDuck
    {
        uint64_t Source = 0;         // 0: off
        float DepthDb = -10.0f;
        float ThresholdDb = -40.0f;
        float Attack = 0.05f;
        float Release = 0.6f;
    };

    struct AudioMixerBus
    {
        uint64_t Id = 0;
        std::string Name;
        uint64_t Parent = kAudioBusMaster; // Output To (Master's own is 0: the speakers)
        float VolumeDb = 0.0f;
        bool Mute = false;
        bool Solo = false;
        bool Bypass = false;               // every effect of the bus off
        bool PlaysWhilePaused = false;     // keeps playing while the game is paused (the UI bus: a pause menu's sounds)
        std::vector<AudioMixerEffect> Effects;
        std::vector<AudioMixerSend> Sends;
        AudioMixerDuck Duck;
    };

    // What a snapshot can change: a bus's fader, a send's level, an effect's number parameter.
    enum class AudioMixerValueKind : uint8_t
    {
        Volume = 0,
        Send,
        Effect
    };

    struct AudioMixerValueKey
    {
        uint64_t Bus = 0;
        AudioMixerValueKind Kind = AudioMixerValueKind::Volume;
        uint64_t Item = 0;       // Send: the target bus; Effect: the effect's id
        uint32_t Parameter = 0;  // Effect: the parameter's index
        bool operator==(const AudioMixerValueKey&) const = default;
    };

    // A named mix state (FMOD's overriding snapshots, Unity's mixer snapshots): only the values it changes; the rest are the Default
    // mix's. Scripts and timelines blend to it (Audio.SetSnapshot).
    struct AudioMixerSnapshot
    {
        std::string Name;
        std::vector<std::pair<AudioMixerValueKey, float>> Values;
    };

    struct AudioMixerData
    {
        std::vector<AudioMixerBus> Buses;          // [0] is Master
        std::vector<AudioMixerSnapshot> Snapshots; // besides the Default mix
        // 3D sounds whose Spatialization is Default: binaural (HRTF, headphones) or panned (speakers). A game's options menu changes
        // it at run time (Audio.SetHeadphones).
        bool Headphones = true;
        // Steam Audio's real-time reflections (A4b): budget, quality and where they are heard.
        AudioReflectionSettings Reflections;

        AudioMixerBus* FindBus(uint64_t id);
        const AudioMixerBus* FindBus(uint64_t id) const;
        int32_t IndexOf(uint64_t id) const;
        // `ancestor` is `bus` or above it (Output To, all the way up).
        bool IsUnder(uint64_t bus, uint64_t ancestor) const;
        // Would routing `from` into `to` (Output To, a send, a sidechain or ducking source) close a loop?
        bool WouldLoop(uint64_t from, uint64_t to) const;
    };

    // A new mixer: Master (a Limiter), Music (ducked by Voice), SFX, Voice, UI (plays while paused), Ambience, Reverb (a Reverb
    // effect, fed by sends) -- the fixed ids of AudioTypes.h.
    AudioMixerData CreateDefaultAudioMixer();
    std::vector<float> DefaultEffectValues(AudioEffectType type);
    bool SerializeAudioMixer(const std::filesystem::path& path, const AudioMixerData& data);
    // Reads through the VFS (a loose file or a pack's). False when missing or unreadable.
    bool DeserializeAudioMixer(const std::filesystem::path& path, AudioMixerData& outData);

    // The asset as the asset manager sees it (Content Browser, packaging, references). The running mixer is AudioMixer's.
    class AudioMixerAsset : public Asset
    {
    public:
        static AssetType GetStaticType() { return AssetType::AudioMixer; }
        AssetType GetType() const override { return GetStaticType(); }
        AudioMixerData Data;
    };

    // The project's mixer, running: built into the engine's bus layout, its values pushed as they change, snapshots blended. Game
    // thread only.
    class AudioMixer
    {
    public:
        // Startup (after AudioEngine::Init): the default mixer, until a project's is loaded.
        static void Init();
        static void Shutdown();
        // The active project's (its Audio Mixer path); created when there is none yet (the editor: `create`).
        static void LoadProjectMixer(bool create);
        static const std::filesystem::path& GetPath(); // empty: no project's
        static bool Save();
        static bool IsDirty();

        // Editing (the Mixer panel). Change GetData() and say what changed: the structure (buses, effects, routing, ducking, an
        // impulse response, added or removed) rebuilds the engine's layout; values (faders, parameters, sends, mute / solo / bypass)
        // are sent as they are.
        static AudioMixerData& GetData();
        static void NotifyStructureChanged();
        static void NotifyValuesChanged();
        static void MarkDirty();

        // The snapshot the panel edits (-1: the Default mix): heard at once while not playing; Get / SetValue read and write it (an
        // override in a snapshot, the mix itself in Default).
        static void SetEditedSnapshot(int32_t index);
        static int32_t GetEditedSnapshot();
        static float GetValue(const AudioMixerValueKey& key);
        static void SetValue(const AudioMixerValueKey& key, float value);
        static bool IsOverridden(const AudioMixerValueKey& key); // in the edited snapshot
        static void ClearOverride(const AudioMixerValueKey& key);
        // What the engine has now (blended), for the panel's display.
        static float GetLiveValue(const AudioMixerValueKey& key);

        // Once a frame, before AudioEngine::Update: snapshot blending, values to the engine, a pending rebuild.
        static void Update(float deltaSeconds);

        // The game.
        static const AudioMixerBus* FindBus(std::string_view name); // letter case ignored
        static const AudioMixerBus* FindBus(uint64_t id);
        static int32_t GetBusIndex(uint64_t id);                     // in the engine's layout (meters); -1: none
        static void SetGameVolume(uint64_t bus, float volume);       // linear, on top of the mix (a game's options menu)
        static float GetGameVolume(uint64_t bus);
        // The game's own choice of output (an options menu), over the mixer's Headphones; reset when Play stops.
        static void SetGameHeadphones(bool headphones);
        static bool GetHeadphones();
        // Blends to a snapshot ("Default": the mix itself) over `seconds`. False when there is no such snapshot.
        static bool SetSnapshot(std::string_view name, float seconds);
        static std::string GetSnapshot();
        // Play stopped: the edited snapshot again at once, game volumes back to 1.
        static void ResetGameState();

        static AudioBusMeter GetMeter(uint64_t bus);
        static float GetEffectGainReduction(uint64_t bus, uint32_t effect);
    };
}
