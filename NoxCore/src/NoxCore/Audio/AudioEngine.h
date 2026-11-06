#pragma once

#include <array>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "AudioClip.h"
#include "AudioTypes.h"

namespace Nox
{
    // How a 3D sound is heard by distance (docs/Audio_Architecture_Plan_2026.md §4; the shared Attenuation asset of A3 holds the
    // same fields). Distances in meters, whatever the project's world unit.
    struct AudioSpatialSettings
    {
        float MinDistance = 1.0f;  // full volume up to here
        float MaxDistance = 50.0f; // silent from here
        AudioAttenuation Attenuation = AudioAttenuation::Inverse;
        float DopplerLevel = 1.0f; // 0 = no Doppler shift, 1 = physical (speed of sound 343 m/s)
    };

    struct AudioPlayDesc
    {
        Ref<AudioClip> Clip;
        float Volume = 1.0f; // linear
        float Pitch = 1.0f;  // playback rate (2 = an octave up)
        bool Loop = false;
        AudioBus Bus = AudioBus::SFX;
        AudioVoiceGroup Group = AudioVoiceGroup::Game;
        bool Spatial = false;           // 3D: panned and attenuated around the listener, with Doppler; 2D: as the file is
        glm::dvec3 Position{ 0.0 };     // world position (world units), for Spatial
        AudioSpatialSettings Spatialization;
        float StartTime = 0.0f; // seconds into the clip
        float FadeIn = 0.0f;    // seconds
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
        std::array<float, static_cast<size_t>(AudioBus::Count)> BusPeak{}; // linear peak per bus over the last blocks
    };

    // The engine's audio (docs/Audio_Architecture_Plan_2026.md §2 and §8): miniaudio opens the output device; a dedicated mixer thread
    // (MMCSS "Pro Audio" on Windows -- not a JobSystem task: audio has a hard deadline every block) mixes 256-frame blocks a few
    // blocks ahead into a ring buffer the device callback only copies out of; a stream thread decodes compressed and streamed clips
    // into per-voice ring buffers (FMOD's and Wwise's stream threads). The game thread talks to the mixer through a lock-free command
    // queue and hears back through a lock-free event queue -- the mixer never locks or allocates.
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

        static void StopGroup(AudioVoiceGroup group, float fadeOutSeconds = 0.0f);
        // Pausing the game pauses its voices, except those on the UI bus (a pause menu's sounds keep playing).
        static void SetGroupPaused(AudioVoiceGroup group, bool paused);

        // The ears: world position (world units) and orientation (the camera's: -Z forward, +Y up, +X right).
        static void SetListener(const glm::dvec3& position, const glm::quat& orientation);

        static void SetBusVolume(AudioBus bus, float volume); // linear, 0..1 (more amplifies)
        static float GetBusVolume(AudioBus bus);
        static void SetBusMuted(AudioBus bus, bool muted);
        static bool IsBusMuted(AudioBus bus);

        // Once per frame (main thread): voices that ended are released, listener and voice velocities are measured, and every
        // voice's gains and rate are sent to the mixer.
        static void Update(float deltaSeconds);

        static AudioStats GetStats();
    };
}
