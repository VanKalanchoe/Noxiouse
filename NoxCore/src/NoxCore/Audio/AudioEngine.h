#pragma once

#include <array>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <memory>
#include <unordered_map>

#include "AudioClip.h"
#include "AudioEffects.h"
#include "AudioTypes.h"
#include "NoxCore/NodeGraph/NodeGraphTypes.h"

namespace Nox
{
    class AudioGraphProgram;

    // How a 3D sound is heard by distance (docs/Audio_Architecture_Plan_2026.md §4; the shared Attenuation asset of A3 holds the
    // same fields). Distances in meters, whatever the project's world unit.
    struct AudioSpatialSettings
    {
        float MinDistance = 1.0f;  // full volume up to here
        float MaxDistance = 50.0f; // silent from here
        AudioAttenuation Attenuation = AudioAttenuation::Inverse;
        float DopplerLevel = 1.0f; // 0 = no Doppler shift, 1 = physical (speed of sound 343 m/s)
        // Steam Audio (§6): how it reaches the ears, the highs fading with distance, walls between it and the listener.
        AudioSpatialization Spatialization = AudioSpatialization::Default;
        bool AirAbsorption = true;
        bool Occlusion = true;     // muffled by geometry in the way, passing through by its acoustic material (transmission)
        float SourceRadius = 0.5f; // meters: the sound's size for occlusion -- partly hidden behind a corner is partly occluded
        bool Reflections = true;   // gets real-time reflections when it is among the nearest (the reflections' budget)
        bool Pathing = false;      // reaches around corners and through doors along the paths baked in Acoustic Probe Volumes:
                                   // what occlusion takes from the direct sound comes back along them (Steam Audio's pathing)
    };

    // Steam Audio's real-time reflections (§6, A4b): the mixer's Acoustics settings.
    struct AudioReflectionSettings
    {
        bool Enabled = true;
        int32_t Rays = 4096;
        int32_t Bounces = 4;
        float Duration = 1.0f;      // seconds
        int32_t MaxSources = 4;     // voices with reflections at once (the nearest; each convolution costs mixer time)
        uint64_t Bus = kAudioBusMaster; // where the reflections are heard
        float LevelDb = 0.0f;
        // A4c: the reverb at the listener the Acoustic Reverb effect applies, and how many sounds get pathing at once (the nearest
        // with Pathing on, inside the baked volume the listener is in).
        AudioListenerReverb ListenerReverb = AudioListenerReverb::Baked;
        int32_t PathingSources = 8;
        bool operator==(const AudioReflectionSettings&) const = default;
    };

    // Where a 3D voice is heard from (Audio Volumes, AudioMixer.h): an interior / exterior volume and low pass, and a send to a
    // reverb bus.
    struct AudioVoiceEnvironment
    {
        float Volume = 1.0f;          // linear, on top of the voice's own
        float LowPassHz = 20000.0f;   // 20 kHz and above: off
        uint64_t SendBus = 0;         // 0: none
        float SendLevel = 0.0f;       // linear
        bool operator==(const AudioVoiceEnvironment&) const = default;
    };

    struct AudioPlayDesc
    {
        // What plays: a clip, or an audio graph (AudioGraph.h; its program built from the asset, its parameters' values here).
        Ref<AudioClip> Clip;
        std::shared_ptr<const AudioGraphProgram> Graph;
        std::unordered_map<std::string, NodeGraphValue> GraphParameters;
        float Volume = 1.0f; // linear
        float Pitch = 1.0f;  // playback rate (2 = an octave up)
        bool Loop = false;
        uint64_t Bus = kAudioBusSFX; // the mixer bus's id (AudioMixer.h); one that is not in the mixer plays into Master
        AudioVoiceGroup Group = AudioVoiceGroup::Game;
        bool Spatial = false;           // 3D: panned and attenuated around the listener, with Doppler; 2D: as the file is
        glm::dvec3 Position{ 0.0 };     // world position (world units), for Spatial
        AudioSpatialSettings Spatialization;
        float StartTime = 0.0f; // seconds into the clip (clips only)
        float FadeIn = 0.0f;    // seconds
        AudioVoiceEnvironment Environment; // Audio Volumes' filtering and reverb send, from the first block (SetVoiceEnvironment after)
    };

    struct AudioStats
    {
        bool DeviceOpen = false;
        std::string DeviceName;
        uint32_t SampleRate = 0;
        uint32_t PeriodFrames = 0; // what the device asks for per callback
        uint32_t Voices = 0;
        uint32_t StreamingVoices = 0;
        uint64_t Underruns = 0;          // device callbacks the mixer had not filled (a click)
        uint64_t StreamStarvations = 0;  // blocks a stream had no decoded audio for (disk too slow)
        float MixBlockMilliseconds = 0.0f;    // one block's mix, averaged
        float MixBlockMaxMilliseconds = 0.0f; // the slowest since the last GetStats
        float BlockMilliseconds = 0.0f;       // a block's length: the mixer's deadline
    };

    // A bus's level after its fader (linear): peaks falling ~20 dB a second, RMS over ~300 ms; how far its compressors, limiters,
    // gates and ducking turned it down (dB, positive).
    struct AudioBusMeter
    {
        float PeakLeft = 0.0f, PeakRight = 0.0f;
        float RmsLeft = 0.0f, RmsRight = 0.0f;
        float GainReductionDb = 0.0f;
    };

    // The mixer's buses as the engine runs them (built by AudioMixer from the .nmixer asset, AudioMixer.h): Buses[0] is Master. Bus,
    // send and sidechain references are indices into Buses.
    struct AudioMixerLayoutDesc
    {
        struct Effect
        {
            uint64_t Id = 0; // with the bus's id, what keeps an effect's instance (a reverb's tail) across a layout change
            AudioEffectType Type = AudioEffectType::EQ;
            Ref<AudioClip> Impulse; // Convolution Reverb
            std::vector<float> Values;
            bool Bypass = false;
            int32_t Sidechain = -1; // the bus whose output is its key
        };
        struct Send
        {
            int32_t Target = -1;
            float Gain = 0.0f; // linear
            bool PreFader = false;
        };
        struct Bus
        {
            uint64_t Id = 0;
            int32_t Parent = -1; // -1: Master (the device)
            float Gain = 1.0f;   // linear: volume, mute / solo, snapshot and the game's own volume together
            bool PlaysWhilePaused = false; // keeps playing while the game is paused (UI)
            std::vector<Effect> Effects;
            std::vector<Send> Sends;
            // Ducking: while DuckSource's output is above DuckThreshold, this bus is turned down by DuckDepth dB.
            int32_t DuckSource = -1;
            float DuckDepthDb = -10.0f;
            float DuckThresholdDb = -40.0f;
            float DuckAttack = 0.05f;  // seconds
            float DuckRelease = 0.6f;
        };
        std::vector<Bus> Buses;
    };


    // The engine's audio (docs/Audio_Architecture_Plan_2026.md §2 and §8): miniaudio opens the output device; a dedicated mixer thread
    // (MMCSS "Pro Audio" on Windows -- not a JobSystem task: audio has a hard deadline every block) mixes 256-frame blocks a few
    // blocks ahead into a ring buffer the device callback only copies out of; a stream thread decodes compressed and streamed clips
    // into per-voice ring buffers (FMOD's and Wwise's stream threads). The game thread talks to the mixer through a lock-free command
    // queue and hears back through a lock-free event queue -- the mixer never locks or allocates.
    //
    // A voice plays a clip or an audio graph (an AudioGraphInstance of its program, run block by block on the mixer thread; its pitch
    // and Doppler reach the graph's Wave Players and oscillators, as a MetaSound's do not -- Unreal leaves them to the graph).
    //
    // Everything here is called from the main thread. Spatialization (distance attenuation, equal-power panning, Doppler) is worked
    // out on the main thread once per frame from the listener and each voice's position (Unreal's active sounds do the same) and
    // reaches the mixer as target gains and a rate, which it ramps across each block so nothing clicks.
    class AudioEngine
    {
    public:
        static constexpr uint32_t MaxVoices = 128;
        static constexpr uint32_t BlockFrames = 256;

        static void Init();
        static void Shutdown();
        // False when no output device could be opened: every call is then a no-op and Play returns 0.
        static bool IsAvailable();

        static AudioVoiceHandle Play(const AudioPlayDesc& desc);
    private:
        static AudioVoiceHandle PlayGraph(const AudioPlayDesc& desc);
    public:
        // Stops with a fade (at least a few milliseconds, so it never clicks).
        static void Stop(AudioVoiceHandle voice, float fadeOutSeconds = 0.0f);
        // Playing and not stopped (a voice fading out after Stop is no longer playing).
        static bool IsPlaying(AudioVoiceHandle voice);
        static void SetVolume(AudioVoiceHandle voice, float volume);
        static void SetPitch(AudioVoiceHandle voice, float pitch);
        static void SetPosition(AudioVoiceHandle voice, const glm::dvec3& position);
        static void SetSpatialization(AudioVoiceHandle voice, const AudioSpatialSettings& settings);
        static void SetLoop(AudioVoiceHandle voice, bool loop);
        static void SetPaused(AudioVoiceHandle voice, bool paused);
        // An audio graph voice's parameters (by their index in its program: AudioGraphProgram::FindParameter); a trigger parameter
        // fires at the start of the next block.
        static void SetGraphParameter(AudioVoiceHandle voice, int32_t index, float value);
        static void FireGraphParameter(AudioVoiceHandle voice, int32_t index);
        // A live literal edit (AudioGraphProgram::LiteralChanges): the value slot set in the playing graph.
        static void SetGraphConstant(AudioVoiceHandle voice, uint32_t slot, float value);
        // A voice's output level (linear peak, falling ~20 dB a second), left and right: the graph editor's meters.
        static glm::vec2 GetVoicePeak(AudioVoiceHandle voice);
        static uint32_t GetSampleRate();

        // Interior / exterior filtering and the reverb send of a voice (the scene works them out from Audio Volumes once a frame).
        static void SetVoiceEnvironment(AudioVoiceHandle voice, const AudioVoiceEnvironment& environment);

        static void StopGroup(AudioVoiceGroup group, float fadeOutSeconds = 0.0f);
        // Pausing the game pauses its voices, except those on a bus that plays while paused (UI: a pause menu's sounds keep playing).
        static void SetGroupPaused(AudioVoiceGroup group, bool paused);

        // The ears: world position (world units) and orientation (the camera's: -Z forward, +Y up, +X right).
        static void SetListener(const glm::dvec3& position, const glm::quat& orientation);

        // The mixer (AudioMixer drives these; docs/Audio_Architecture_Plan_2026.md §7). A new layout replaces the buses, effects and
        // routing at the start of a block; an effect whose bus id, effect id, type and impulse response are unchanged keeps its
        // instance, so a reverb's tail and a compressor's envelope carry on. The rest changes values in the running layout, by the
        // bus's index in it: ramped or smoothed by the mixer, never a click.
        // Headphones: 3D sounds whose Spatialization is Default use the HRTF (binaural); speakers: panning.
        static void SetHeadphones(bool headphones);
        static void SetReflections(const AudioReflectionSettings& settings);
        static float GetReflectionMilliseconds(); // the last reflection run's time on its thread
        struct ReflectionStats
        {
            float Milliseconds = 0.0f; // the last run's time on the reflection thread
            float Peak = 0.0f, Rms = 0.0f; // linear: what the reflections alone add to their bus (after Level)
            int32_t Voices = 0;            // voices with reflections now
            int32_t PathingVoices = 0;     // voices with pathing now (their paths found)
            bool ListenerReverb = false;   // a reverb at the listener now (the Acoustic Reverb effects have one to apply)
        };
        static ReflectionStats GetReflectionStats();
        static bool GetHeadphones();

        static void SetMixerLayout(const AudioMixerLayoutDesc& layout);
        static void SetBusGain(uint32_t bus, float gain); // linear
        static void SetEffectParameter(uint32_t bus, uint32_t effect, uint32_t parameter, float value);
        static void SetEffectBypass(uint32_t bus, uint32_t effect, bool bypass);
        static void SetSendGain(uint32_t bus, uint32_t send, float gain); // linear
        static AudioBusMeter GetBusMeter(uint32_t bus);
        static float GetEffectGainReduction(uint32_t bus, uint32_t effect); // dB, positive
        static uint32_t GetBusCount();
        static constexpr uint32_t MaxBuses = 32;

        // Once per frame (main thread): voices that ended are released, listener and voice velocities are measured, and every
        // voice's gains and rate are sent to the mixer.
        static void Update(float deltaSeconds);

        static AudioStats GetStats();
    };
}
