#include "AudioEngine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <xmmintrin.h>
    #define NOX_AUDIO_SSE 1
#endif

#include <miniaudio.h>

#include "AudioDSP.h"
#include "AudioGraph.h"
#include "SpatialAudio.h"
#include "NoxCore/Core/Log.h"
#include "NoxCore/Core/WorldUnits.h"
#include "NoxCore/Profiling/Profiler.h"

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <avrt.h>
#endif

namespace Nox
{
    namespace
    {
        constexpr uint32_t kBlock = AudioEngine::BlockFrames;
        constexpr uint32_t kGroupCount = static_cast<uint32_t>(AudioVoiceGroup::Count);
        constexpr uint32_t kOutputRingFrames = 8192;  // the mixer's lead over the device (it keeps a period + 2 blocks in it)
        constexpr uint32_t kStagingFrames = 4096;     // a streamed voice's decoded frames at hand for one block's resampling
        constexpr float kMaxRate = 8.0f;              // source frames per output frame (pitch x Doppler x rate ratio)
        constexpr float kMinStopFade = 0.005f;        // every stop fades at least this long (no click)
        constexpr float kSpeedOfSound = 343.0f;       // m/s
        constexpr float kVelocitySmoothing = 0.1f;    // seconds: frame-time jitter does not wobble the Doppler shift
        constexpr float kTeleportSpeed = 500.0f;      // m/s: a jump faster than this is a teleport, not motion
        constexpr float kStreamBufferSeconds = 1.0f;  // decoded ahead per streamed voice

        // Single producer, single consumer, fixed size: the game thread pushes, the mixer pops (and the other way for events).
        template <typename T, size_t N>
        class SpscQueue
        {
            static_assert((N & (N - 1)) == 0, "a power of two");

        public:
            bool Push(const T& item)
            {
                const size_t head = m_Head.load(std::memory_order_relaxed);
                if (head - m_Tail.load(std::memory_order_acquire) >= N)
                    return false;
                m_Items[head & (N - 1)] = item;
                m_Head.store(head + 1, std::memory_order_release);
                return true;
            }

            bool Pop(T& item)
            {
                const size_t tail = m_Tail.load(std::memory_order_relaxed);
                if (tail == m_Head.load(std::memory_order_acquire))
                    return false;
                item = m_Items[tail & (N - 1)];
                m_Tail.store(tail + 1, std::memory_order_release);
                return true;
            }

        private:
            std::array<T, N> m_Items{};
            alignas(64) std::atomic<size_t> m_Head{ 0 };
            alignas(64) std::atomic<size_t> m_Tail{ 0 };
        };

        // A Compressed In Memory or Streaming voice's decoder: the stream thread fills Ring, the mixer drains it.
        struct StreamState
        {
            Ref<AudioClip> Clip;
            AudioClipDecoder Decoder;
            ma_pcm_rb Ring{};
            bool RingReady = false;
            uint32_t Channels = 1;
            std::atomic<bool> Loop{ false };
            std::atomic<bool> Ended{ false };  // every frame is in Ring (written before this is set)
            std::atomic<bool> Failed{ false };

            ~StreamState()
            {
                if (RingReady)
                    ma_pcm_rb_uninit(&Ring);
            }
        };

        enum class CommandType : uint8_t
        {
            Start,
            Stop,
            Update,
            SetLoop,
            PauseVoice,
            PauseGroup,
            StopGroup,
            BusGain,
            SetParameter,
            FireParameter,
            SetConstant,
            SetLayout,
            VoiceBus,
            EffectParameter,
            EffectBypass,
            SendGain,
            VoiceReflections,
            ListenerBasis,
            ReflectionsOutput,
            VoicePathing,
            ListenerReverb
        };

        struct MixerLayout;

        struct Command
        {
            CommandType Type = CommandType::Update;
            uint32_t Slot = 0;
            // Start
            const AudioClip* Clip = nullptr;
            StreamState* Stream = nullptr;
            AudioGraphInstance* Graph = nullptr;
            MixerLayout* Layout = nullptr; // SetLayout
            int32_t Parameter = -1; // SetParameter / FireParameter; EffectParameter / EffectBypass: the effect; SendGain: the send
            uint32_t Index = 0;     // EffectParameter: the parameter
            uint64_t StartFrame = 0;
            uint16_t Bus = 0;       // a bus index (Start, VoiceBus, BusGain and the bus commands)
            int16_t SendBus = -1;   // Start / Update / VoiceBus: the voice's environment send (-1: none)
            uint8_t Group = 0;
            bool Downmix = false; // a 3D stereo clip is heard as one point: its channels summed before panning
            // Start / Update
            float GainL = 0.0f;
            float GainR = 0.0f;
            float Rate = 1.0f;
            float LowPass = 20000.0f;
            float SendGain = 0.0f;
            // Start / Update: Steam Audio spatialization (Spatial 0: none -- 2D, or the engine's own panning).
            uint8_t Spatial = 0;      // 1 panning, 2 binaural
            uint8_t DirectFlags = 0;  // IPLDirectEffectFlags
            float Direction[3] = { 0.0f, 0.0f, -1.0f }; // listener space
            float Occlusion = 1.0f;
            float Transmission[3] = { 1.0f, 1.0f, 1.0f };
            float AirAbsorption[3] = { 1.0f, 1.0f, 1.0f };
            float Attenuation = 1.0f;     // the distance curve (Steam Audio voices: their gains are without it)
            float ReflectionGain = 0.0f;  // what goes into its reflections
            float PathingGain = 0.0f;     // what goes along its paths (what occlusion takes from the direct sound)
            // VoicePathing (Flag: valid): the paths' EQ and directions (order-1 ambisonics, world space).
            float PathEq[3] = { 1.0f, 1.0f, 1.0f };
            float PathSh[SpatialAudio::ReflectionChannels] = {};
            // VoiceReflections: the pool slot (Parameter, -1: none) and its effect parameters; ListenerBasis: right, up, ahead.
            IPLReflectionEffectParams Reflection{};
            float Basis[9] = {};
            // Start (fade in), Stop / StopGroup (fade out), BusGain (gain)
            float Value = 0.0f;
            bool Flag = false; // Loop, paused
            // Start: a voice whose occlusion is traced waits silent (its playhead held) until an Update says it was (Simulated):
            // its first blocks are never heard through a wall.
            bool WaitForSimulation = false;
            bool Simulated = false;
        };

        struct FinishedEvent
        {
            uint32_t Slot = 0;
        };

        // Mixer thread only.
        struct MixVoice
        {
            bool Active = false;
            bool Paused = false;
            bool Stopping = false;
            bool Waiting = false; // for its first occlusion (Command::WaitForSimulation)
            const AudioClip* Clip = nullptr;
            StreamState* Stream = nullptr;
            AudioGraphInstance* Graph = nullptr;
            uint32_t Channels = 1;
            bool Loop = false;
            bool Downmix = false;
            uint16_t Bus = 0;
            uint8_t Group = 0;
            // The environment (Audio Volumes): a low pass and a send.
            int32_t SendBus = -1;
            float SendGain = 0.0f, SendApplied = 0.0f;
            float LowPass = 20000.0f;
            bool LowPassActive = false;
            AudioDSP::FrequencyRamp LowPassRamp;
            AudioDSP::Biquad LowPassFilter[2];
            // Steam Audio (§6).
            uint8_t Spatial = 0;
            IPLDirectEffectParams Direct{};
            float Direction[3] = { 0.0f, 0.0f, -1.0f };
            float Attenuation = 1.0f, AttenuationApplied = 1.0f;
            float ReflectionGain = 0.0f, ReflectionApplied = 0.0f;
            int32_t ReflectionSlot = -1;
            IPLReflectionEffectParams Reflection{};
            bool PathingOn = false;
            float PathEq[3] = { 1.0f, 1.0f, 1.0f };
            float PathSh[SpatialAudio::ReflectionChannels] = {};
            float PathingGain = 0.0f, PathingApplied = 0.0f;
            int64_t Frame = 0; // the playhead: decoded clip frame, or a streamed voice's frame counted from its start
            double Fraction = 0.0;
            float Rate = 1.0f;
            float GainL = 0.0f, GainR = 0.0f;
            float TargetL = 0.0f, TargetR = 0.0f;
            float Fade = 1.0f;
            float FadeStep = 0.0f; // per output frame
            std::vector<float> Staging; // streamed voices: decoded frames from StagingStart
            int64_t StagingStart = 0;
            uint32_t StagingCount = 0;
        };

        // Main thread only.
        struct GameVoice
        {
            bool Alive = false;
            bool StopSent = false;
            bool Paused = false;
            uint32_t Generation = 0;
            AudioPlayDesc Desc;
            std::shared_ptr<StreamState> Stream;
            std::unique_ptr<AudioGraphInstance> Graph; // destroyed here, after the mixer reported the voice ended
            glm::dvec3 PreviousPosition{ 0.0 };
            glm::vec3 Velocity{ 0.0f };
            bool HasPrevious = false;
            AudioVoiceEnvironment Environment;
            IPLSource Source = nullptr; // Steam Audio's simulation of it (3D voices with occlusion or air absorption)
            int32_t ReflectionSlot = -1; // its reflection pool slot (the nearest voices get one)
            int32_t PathingSlot = -1;    // its pathing pool slot (the nearest with Pathing on get one)
            bool PathingValid = false;   // its paths were found (the last run's)
            bool Simulated = false;      // its occlusion has been traced once (the mixer holds it until then)
            SpatialAudio::SourceOutputs Direct;
        };

        // ---- The mixer's buses as they run (built on the game thread from an AudioMixerLayoutDesc, swapped in by a command) -----

        constexpr float kLowPassOff = 19999.0f; // a voice's low pass at or above this is not run

        struct RuntimeEffect
        {
            std::shared_ptr<AudioEffect> Instance; // (shared with the game side's cache: kept across layouts; never released here)
            bool Bypass = false;
            int32_t Sidechain = -1;
        };

        struct RuntimeSend
        {
            int32_t Target = -1;
            float Gain = 0.0f;
            float Applied = 0.0f;
            bool PreFader = false;
        };

        struct RuntimeBus
        {
            uint64_t Id = 0;
            int32_t Parent = -1;
            bool PlaysWhilePaused = false;
            std::vector<RuntimeEffect> Effects;
            std::vector<RuntimeSend> Sends;
            int32_t DuckSource = -1;
            float DuckDepthDb = 0.0f;
            float DuckThreshold = 0.0f;  // linear
            float DuckAttack = 0.0f;     // per-block smoothing coefficients
            float DuckRelease = 0.0f;
            float DuckDb = 0.0f;         // the ducking now (<= 0)
            float Target = 1.0f;         // the fader (linear)
            float Applied = 1.0f;        // fader x ducking at the end of the last block
            float PowerLeft = 0.0f, PowerRight = 0.0f; // the RMS meters' mean squares
            alignas(16) float Left[kBlock] = {};
            alignas(16) float Right[kBlock] = {};
        };

        struct MixerLayout
        {
            std::vector<RuntimeBus> Buses;  // [0]: Master
            std::vector<uint32_t> Order;    // every bus after its children, its send sources and its sidechain / ducking sources
        };

        struct MixParams
        {
            float GainL = 0.0f;
            float GainR = 0.0f;
            float Rate = 1.0f;
            uint8_t Spatial = 0;    // Steam Audio: 1 panning, 2 binaural (the gains are then one, unpanned, without the distance curve)
            glm::vec3 Direction{ 0.0f, 0.0f, -1.0f };
            float Attenuation = 1.0f;
            float ReflectionGain = 0.0f;
        };

        struct State
        {
            ma_device Device{};
            bool DeviceOpen = false;
            uint32_t SampleRate = 48000;
            uint32_t PeriodFrames = 0;
            uint32_t DeviceBufferFrames = 0; // all its periods
            std::string DeviceName;
            ma_pcm_rb Output{};
            ma_event MixerWake{};

            std::atomic<bool> Running{ false };
            std::thread MixerThread;
            std::thread StreamThread;

            SpscQueue<Command, 8192> Commands;
            SpscQueue<FinishedEvent, 256> Finished;
            SpscQueue<MixerLayout*, 64> Retired; // layouts the mixer replaced, back to the game thread to be freed

            // Mixer side.
            std::array<MixVoice, AudioEngine::MaxVoices> MixVoices;
            MixerLayout* Layout = nullptr;
            std::array<bool, kGroupCount> GroupPaused{};
            std::array<float, kBlock * 2> VoiceBuffer{}; // one voice's block, interleaved
            std::array<float, kBlock * 2> Block{};
            // Steam Audio, per voice slot (created at start: nothing allocates while mixing).
            std::array<IPLDirectEffect, AudioEngine::MaxVoices> DirectEffects{};
            std::array<IPLBinauralEffect, AudioEngine::MaxVoices> BinauralEffects{};
            std::array<IPLPanningEffect, AudioEngine::MaxVoices> PanningEffects{};
            alignas(16) float SpatialMono[kBlock] = {};
            alignas(16) float SpatialDirect[kBlock] = {};
            alignas(16) float SpatialLeft[kBlock] = {};
            alignas(16) float SpatialRight[kBlock] = {};
            bool Headphones = true; // (game side)
            bool SteamAudio = false; // its context, HRTF and every slot's effects are there
            // Reflections (mixer side): the pool's effects, which voice each serves, their ambisonic sum and its decoder.
            std::array<IPLReflectionEffect, SpatialAudio::MaxReflectionSources> ReflectionEffects{};
            std::array<int32_t, SpatialAudio::MaxReflectionSources> ReflectionOwner{};
            // A voice that ended: its reflections ring on (silence in) until the response has passed.
            std::array<uint32_t, SpatialAudio::MaxReflectionSources> ReflectionTailBlocks{};
            std::array<IPLReflectionEffectParams, SpatialAudio::MaxReflectionSources> ReflectionTailParams{};
            IPLAmbisonicsDecodeEffect ReflectionDecode = nullptr;
            IPLReflectionMixer ReflectionMixer = nullptr; // (the voices' convolutions summed in the frequency domain: one inverse FFT)
            IPLint32 ReflectionIrSize = 48000;
            alignas(16) float Ambisonics[SpatialAudio::ReflectionChannels][kBlock] = {};
            alignas(16) float AmbisonicsVoice[SpatialAudio::ReflectionChannels][kBlock] = {};
            alignas(16) float ReflectionInput[kBlock] = {};
            float ListenerBasis[9] = { 1, 0, 0, 0, 1, 0, 0, 0, -1 };
            bool ReflectionsBinaural = true;
            bool ReflectionsOn = false;
            int32_t ReflectionsBus = 0;
            float ReflectionsLevel = 1.0f, ReflectionsApplied = 1.0f;
            float ReflectionsPower = 0.0f; // (the reflections' meter: mean power over ~300 ms)
            // Pathing (A4c): every voice slot's path effect (paths to stereo, HRTF or panning).
            std::array<IPLPathEffect, AudioEngine::MaxVoices> PathEffects{};
            alignas(16) float PathInput[kBlock] = {};
            alignas(16) float PathLeft[kBlock] = {};
            alignas(16) float PathRight[kBlock] = {};
            // The reverb at the listener (A4c), for the Acoustic Reverb effects (mixer side).
            IPLReflectionEffectParams ListenerReverbParams{};
            bool ListenerReverbValid = false;
            // Reflections (game side).
            AudioReflectionSettings Reflections;
            bool ReflectionsSent = false;
            std::array<int32_t, SpatialAudio::MaxReflectionSources> ReflectionVoice{}; // pool slot -> voice slot (-1: free)
            double LastReflectionStart = -1e9;
            std::array<double, SpatialAudio::MaxReflectionSources> ReflectionFreedAt{}; // (new voices take the slot free longest: tails ring out)
            // Each slot's last results and where they were simulated: a new voice near there uses them at once (a footstep has ended
            // before a new run could finish).
            std::array<IPLReflectionEffectParams, SpatialAudio::MaxReflectionSources> ReflectionParams{};
            std::array<glm::dvec3, SpatialAudio::MaxReflectionSources> ReflectionRunPosition{};
            std::array<glm::dvec3, SpatialAudio::MaxReflectionSources> ReflectionResultPosition{};
            std::array<bool, SpatialAudio::MaxReflectionSources> ReflectionValid{};
            // Pathing and the listener's reverb (game side).
            std::array<int32_t, SpatialAudio::MaxPathingSources> PathingVoice{}; // pathing slot -> voice slot (-1: free)
            bool HasAcousticReverb = false;   // the layout has an Acoustic Reverb effect (else the listener's reverb is not simulated)
            bool ListenerReverbSent = false;  // the last ListenerReverb command said valid

            // Game side.
            std::array<GameVoice, AudioEngine::MaxVoices> GameVoices;
            std::vector<uint32_t> FreeSlots;
            std::array<bool, kGroupCount> GamePaused{};
            MixerLayout* GameLayout = nullptr;   // the last layout sent (the game thread reads only its ids and effect instances)
            std::unordered_map<uint64_t, uint32_t> BusIndex; // bus id -> index in it
            struct CachedEffect
            {
                AudioEffectType Type = AudioEffectType::EQ;
                const AudioClip* Impulse = nullptr;
                Ref<AudioClip> ImpulseRef;
                std::shared_ptr<AudioEffect> Instance;
            };
            std::unordered_map<uint64_t, CachedEffect> Effects; // by bus id ^ effect id hash
            glm::dvec3 ListenerPosition{ 0.0 };
            glm::quat ListenerOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
            glm::dvec3 ListenerPrevious{ 0.0 };
            bool ListenerHasPrevious = false;
            glm::vec3 ListenerVelocity{ 0.0f };
            double LastVoiceLimitWarning = -1e9;

            // Stream thread.
            std::mutex StreamMutex;
            std::condition_variable StreamWake;
            std::vector<std::shared_ptr<StreamState>> Streams;
            std::vector<std::shared_ptr<AudioClipStream>> ClipStreams; // audio graph Wave Players'
            uint32_t NextGraphSeed = 1;

            // Statistics (written by the mixer and the device callback).
            std::atomic<uint64_t> Underruns{ 0 };
            std::atomic<uint64_t> Starvations{ 0 };
            std::atomic<float> MixAverage{ 0.0f };
            std::atomic<float> MixMax{ 0.0f };
            std::array<std::atomic<float>, AudioEngine::MaxBuses * 5> BusMeters{}; // per bus: peak L / R, RMS L / R, gain reduction
            std::array<std::atomic<float>, AudioEngine::MaxVoices * 2> VoicePeak{};
            std::array<std::atomic<float>, 2> ReflectionsMeter{}; // the reflections' output alone: peak, RMS
            std::atomic<bool> MixerReady{ false };
        };

        State* s_State = nullptr;

        // ---- Mixer thread -------------------------------------------------------------------------------------------------------

#ifdef _WIN32
        // MMCSS "Pro Audio": the scheduler class Windows gives audio threads (what WASAPI's own threads and FMOD's mixer use).
        HANDLE RaiseMixerPriority()
        {
            DWORD taskIndex = 0;
            HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
            if (task)
                AvSetMmThreadPriority(task, AVRT_PRIORITY_HIGH);
            else
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            return task;
        }
#endif

        // A voice's Steam Audio parameters from a Start / Update command.
        void ApplySpatial(MixVoice& voice, const Command& command)
        {
            voice.Spatial = command.Spatial;
            voice.Direct.flags = static_cast<IPLDirectEffectFlags>(command.DirectFlags);
            voice.Direct.transmissionType = IPL_TRANSMISSIONTYPE_FREQDEPENDENT;
            voice.Direct.distanceAttenuation = 1.0f; // (the engine's own curve is in the voice's gain)
            voice.Direct.directivity = 1.0f;
            voice.Direct.occlusion = command.Occlusion;
            for (int band = 0; band < 3; ++band)
            {
                voice.Direct.transmission[band] = command.Transmission[band];
                voice.Direct.airAbsorption[band] = command.AirAbsorption[band];
                voice.Direction[band] = command.Direction[band];
            }
            voice.Attenuation = command.Attenuation;
            voice.ReflectionGain = command.ReflectionGain;
            voice.PathingGain = command.PathingGain;
        }

        void ApplyCommand(State& state, const Command& command)
        {
            switch (command.Type)
            {
            case CommandType::Start:
            {
                MixVoice& voice = state.MixVoices[command.Slot];
                voice.Active = true;
                voice.Paused = false;
                voice.Stopping = false;
                voice.Waiting = command.WaitForSimulation;
                voice.Clip = command.Clip;
                voice.Stream = command.Stream;
                voice.Graph = command.Graph;
                voice.Channels = command.Graph ? 2 : command.Clip->GetChannels();
                state.VoicePeak[command.Slot * 2].store(0.0f, std::memory_order_relaxed);
                state.VoicePeak[command.Slot * 2 + 1].store(0.0f, std::memory_order_relaxed);
                voice.Loop = command.Flag;
                voice.Downmix = command.Downmix;
                voice.Bus = command.Bus;
                voice.Group = command.Group;
                voice.SendBus = command.SendBus;
                voice.SendGain = voice.SendApplied = command.SendGain;
                voice.LowPass = command.LowPass;
                voice.LowPassRamp = {};
                voice.LowPassFilter[0].Reset();
                voice.LowPassFilter[1].Reset();
                voice.LowPassActive = command.LowPass < kLowPassOff;
                ApplySpatial(voice, command);
                voice.AttenuationApplied = voice.Attenuation;
                voice.ReflectionApplied = voice.ReflectionGain;
                voice.ReflectionSlot = -1;
                voice.PathingOn = false;
                voice.PathingApplied = 0.0f;
                if (state.PathEffects[command.Slot])
                    iplPathEffectReset(state.PathEffects[command.Slot]);
                if (state.DirectEffects[command.Slot])
                {
                    iplDirectEffectReset(state.DirectEffects[command.Slot]);
                    iplBinauralEffectReset(state.BinauralEffects[command.Slot]);
                    iplPanningEffectReset(state.PanningEffects[command.Slot]);
                }
                if (voice.LowPassActive)
                {
                    float from, to;
                    voice.LowPassRamp.Begin(command.LowPass, static_cast<float>(state.SampleRate), from, to); // (filtered from its first frame)
                }
                voice.Frame = command.Stream ? 0 : static_cast<int64_t>(command.StartFrame);
                voice.Fraction = 0.0;
                voice.Rate = command.Rate;
                voice.GainL = voice.TargetL = command.GainL;
                voice.GainR = voice.TargetR = command.GainR;
                voice.Fade = command.Value > 0.0f ? 0.0f : 1.0f;
                voice.FadeStep = command.Value > 0.0f ? 1.0f / (command.Value * static_cast<float>(state.SampleRate)) : 0.0f;
                voice.StagingStart = 0;
                voice.StagingCount = 0;
                break;
            }
            case CommandType::Stop:
            {
                MixVoice& voice = state.MixVoices[command.Slot];
                if (voice.Active && !voice.Stopping)
                {
                    voice.Stopping = true;
                    voice.FadeStep = -std::max(voice.Fade, 1e-3f) / (std::max(command.Value, kMinStopFade) * static_cast<float>(state.SampleRate));
                }
                break;
            }
            case CommandType::Update:
            {
                MixVoice& voice = state.MixVoices[command.Slot];
                voice.TargetL = command.GainL;
                voice.TargetR = command.GainR;
                voice.Rate = command.Rate;
                voice.LowPass = command.LowPass;
                voice.SendBus = command.SendBus;
                voice.SendGain = command.SendGain;
                ApplySpatial(voice, command);
                if (command.Simulated)
                {
                    voice.Waiting = false;
                    voice.AttenuationApplied = voice.Attenuation; // (it starts at its traced level: no ramp from the start's guess)
                    voice.ReflectionApplied = voice.ReflectionGain;
                }
                break;
            }
            case CommandType::SetLoop:
                state.MixVoices[command.Slot].Loop = command.Flag;
                break;
            case CommandType::PauseVoice:
                state.MixVoices[command.Slot].Paused = command.Flag;
                break;
            case CommandType::PauseGroup:
                state.GroupPaused[command.Group] = command.Flag;
                break;
            case CommandType::StopGroup:
                for (MixVoice& voice : state.MixVoices)
                {
                    if (voice.Active && voice.Group == command.Group && !voice.Stopping)
                    {
                        voice.Stopping = true;
                        voice.FadeStep = -std::max(voice.Fade, 1e-3f) / (std::max(command.Value, kMinStopFade) * static_cast<float>(state.SampleRate));
                    }
                }
                break;
            case CommandType::BusGain:
                if (state.Layout && command.Bus < state.Layout->Buses.size())
                    state.Layout->Buses[command.Bus].Target = command.Value;
                break;
            case CommandType::SetLayout:
            {
                // What carries on from the old layout: a bus's fader and ducking where they were (no jump), its meters' memory, its
                // sends' gains. (The effect instances the two share are the same objects.)
                MixerLayout* old = state.Layout;
                MixerLayout* layout = command.Layout;
                if (old)
                {
                    for (RuntimeBus& bus : layout->Buses)
                    {
                        for (const RuntimeBus& previous : old->Buses)
                        {
                            if (previous.Id != bus.Id)
                                continue;
                            bus.Applied = previous.Applied;
                            bus.DuckDb = previous.DuckDb;
                            bus.PowerLeft = previous.PowerLeft;
                            bus.PowerRight = previous.PowerRight;
                            for (RuntimeSend& send : bus.Sends)
                            {
                                for (const RuntimeSend& before : previous.Sends)
                                {
                                    if (before.Target >= 0 && send.Target >= 0 && old->Buses[before.Target].Id == layout->Buses[send.Target].Id)
                                        send.Applied = before.Applied;
                                }
                            }
                            break;
                        }
                    }
                }
                state.Layout = layout;
                if (old && !state.Retired.Push(old))
                    state.Layout = old, state.Retired.Push(layout); // (never: the game frees retired layouts every frame)
                break;
            }
            case CommandType::VoiceBus:
                state.MixVoices[command.Slot].Bus = command.Bus;
                state.MixVoices[command.Slot].SendBus = command.SendBus;
                break;
            case CommandType::EffectParameter:
            case CommandType::EffectBypass:
                if (state.Layout && command.Bus < state.Layout->Buses.size())
                {
                    RuntimeBus& bus = state.Layout->Buses[command.Bus];
                    if (command.Parameter >= 0 && static_cast<size_t>(command.Parameter) < bus.Effects.size())
                    {
                        RuntimeEffect& effect = bus.Effects[command.Parameter];
                        if (command.Type == CommandType::EffectBypass)
                        {
                            if (effect.Bypass && !command.Flag)
                                effect.Instance->Reset(); // back in: no stale tail from before it was bypassed
                            effect.Bypass = command.Flag;
                        }
                        else
                            effect.Instance->SetParameter(command.Index, command.Value);
                    }
                }
                break;
            case CommandType::VoiceReflections:
            {
                MixVoice& voice = state.MixVoices[command.Slot];
                const int32_t slot = command.Parameter;
                voice.ReflectionSlot = slot >= 0 && slot < SpatialAudio::MaxReflectionSources && state.ReflectionEffects[slot] ? slot : -1;
                voice.Reflection = command.Reflection;
                if (voice.ReflectionSlot >= 0 && state.ReflectionOwner[slot] != static_cast<int32_t>(command.Slot))
                {
                    // A new voice in the slot: the effect goes on (the last voice's tail rings out under the new input -- the next
                    // footstep keeps the one before's reverb), its own tail pass ends.
                    state.ReflectionTailBlocks[slot] = 0;
                    state.ReflectionOwner[slot] = static_cast<int32_t>(command.Slot);
                }
                break;
            }
            case CommandType::ListenerBasis:
                std::memcpy(state.ListenerBasis, command.Basis, sizeof(state.ListenerBasis));
                state.ReflectionsBinaural = command.Flag;
                break;
            case CommandType::ReflectionsOutput:
                state.ReflectionsOn = command.Flag && state.ReflectionDecode;
                state.ReflectionsBus = command.Bus;
                state.ReflectionsLevel = command.Value;
                break;
            case CommandType::VoicePathing:
            {
                MixVoice& voice = state.MixVoices[command.Slot];
                voice.PathingOn = command.Flag && state.PathEffects[command.Slot];
                std::memcpy(voice.PathEq, command.PathEq, sizeof(voice.PathEq));
                std::memcpy(voice.PathSh, command.PathSh, sizeof(voice.PathSh));
                break;
            }
            case CommandType::ListenerReverb:
                state.ListenerReverbValid = command.Flag;
                if (command.Flag)
                    state.ListenerReverbParams = command.Reflection;
                break;
            case CommandType::SendGain:
                if (state.Layout && command.Bus < state.Layout->Buses.size())
                {
                    RuntimeBus& bus = state.Layout->Buses[command.Bus];
                    if (command.Parameter >= 0 && static_cast<size_t>(command.Parameter) < bus.Sends.size())
                        bus.Sends[command.Parameter].Gain = command.Value;
                }
                break;
            case CommandType::SetParameter:
                if (AudioGraphInstance* graph = state.MixVoices[command.Slot].Graph)
                    graph->SetParameter(command.Parameter, command.Value);
                break;
            case CommandType::FireParameter:
                if (AudioGraphInstance* graph = state.MixVoices[command.Slot].Graph)
                    graph->FireParameter(command.Parameter);
                break;
            case CommandType::SetConstant:
                if (AudioGraphInstance* graph = state.MixVoices[command.Slot].Graph)
                    graph->SetConstant(static_cast<uint32_t>(command.Parameter), command.Value);
                break;
            }
        }

        // Tops a streamed voice's staging up from its ring: drops what the playhead has passed, appends what is decoded.
        void RefillStaging(MixVoice& voice)
        {
            const uint32_t channels = voice.Channels;
            const int64_t keepFrom = std::max<int64_t>(voice.Frame, voice.StagingStart);
            const int64_t drop = std::min<int64_t>(keepFrom - voice.StagingStart, voice.StagingCount);
            if (drop > 0)
            {
                const uint32_t remaining = voice.StagingCount - static_cast<uint32_t>(drop);
                std::memmove(voice.Staging.data(), voice.Staging.data() + drop * channels, sizeof(float) * remaining * channels);
                voice.StagingCount = remaining;
                voice.StagingStart += drop;
            }
            while (voice.StagingCount < kStagingFrames)
            {
                ma_uint32 frames = kStagingFrames - voice.StagingCount;
                void* source = nullptr;
                if (ma_pcm_rb_acquire_read(&voice.Stream->Ring, &frames, &source) != MA_SUCCESS || frames == 0)
                    break;
                std::memcpy(voice.Staging.data() + voice.StagingCount * channels, source, sizeof(float) * frames * channels);
                ma_pcm_rb_commit_read(&voice.Stream->Ring, frames);
                voice.StagingCount += frames;
            }
        }

        // Mixes one voice into its bus for this block. False when it has ended.
        bool MixVoiceBlock(State& state, MixVoice& voice, float* out)
        {
            const uint32_t channels = voice.Channels;
            bool streamEnded = false;
            if (voice.Stream)
            {
                if (voice.Stream->Failed.load(std::memory_order_acquire))
                    return false;
                // Read before draining: when it says ended, everything the stream will ever write is in the ring already.
                streamEnded = voice.Stream->Ended.load(std::memory_order_acquire);
                RefillStaging(voice);
            }

            const float* samples = voice.Stream ? voice.Staging.data() : voice.Clip->GetSamples().data();
            const int64_t clipFrames = voice.Stream ? 0 : static_cast<int64_t>(voice.Clip->GetFrameCount());
            const float rate = std::clamp(voice.Rate, 0.0f, kMaxRate);

            const float stepL = (voice.TargetL - voice.GainL) / static_cast<float>(kBlock);
            const float stepR = (voice.TargetR - voice.GainR) / static_cast<float>(kBlock);

            for (uint32_t i = 0; i < kBlock; ++i)
            {
                // The two frames either side of the playhead (linear interpolation, as Unreal's mixer source does).
                int64_t index0 = 0;
                int64_t index1 = 0;
                bool haveNext = true;
                if (voice.Stream)
                {
                    const int64_t local = voice.Frame - voice.StagingStart;
                    if (local >= voice.StagingCount)
                    {
                        if (streamEnded)
                            return false;
                        state.Starvations.fetch_add(1, std::memory_order_relaxed); // not decoded yet: silence, playhead held
                        break;
                    }
                    index0 = local;
                    index1 = local + 1;
                    if (index1 >= voice.StagingCount)
                    {
                        if (!streamEnded)
                        {
                            state.Starvations.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        haveNext = false;
                    }
                }
                else
                {
                    index0 = voice.Frame;
                    index1 = voice.Frame + 1;
                    if (index1 >= clipFrames)
                    {
                        if (voice.Loop)
                            index1 -= clipFrames;
                        else
                            haveNext = false;
                    }
                }

                const float t = static_cast<float>(voice.Fraction);
                float left;
                float right;
                if (channels == 2)
                {
                    const float l0 = samples[index0 * 2], r0 = samples[index0 * 2 + 1];
                    const float l1 = haveNext ? samples[index1 * 2] : 0.0f;
                    const float r1 = haveNext ? samples[index1 * 2 + 1] : 0.0f;
                    left = l0 + (l1 - l0) * t;
                    right = r0 + (r1 - r0) * t;
                    if (voice.Downmix)
                        left = right = 0.5f * (left + right);
                }
                else
                {
                    const float s0 = samples[index0];
                    const float s1 = haveNext ? samples[index1] : 0.0f;
                    left = right = s0 + (s1 - s0) * t;
                }

                const float fade = voice.Fade;
                out[i * 2] += left * voice.GainL * fade;
                out[i * 2 + 1] += right * voice.GainR * fade;
                voice.GainL += stepL;
                voice.GainR += stepR;

                if (voice.FadeStep != 0.0f)
                {
                    voice.Fade += voice.FadeStep;
                    if (voice.Fade >= 1.0f)
                    {
                        voice.Fade = 1.0f;
                        voice.FadeStep = 0.0f;
                    }
                    else if (voice.Fade <= 0.0f)
                        return false; // stopped: faded out
                }

                voice.Fraction += rate;
                const double whole = std::floor(voice.Fraction);
                voice.Frame += static_cast<int64_t>(whole);
                voice.Fraction -= whole;
                if (!voice.Stream && voice.Frame >= clipFrames)
                {
                    if (!voice.Loop)
                        return false;
                    voice.Frame %= clipFrames;
                }
            }
            // A block cut short (a starved stream) still lands on its targets.
            voice.GainL = voice.TargetL;
            voice.GainR = voice.TargetR;
            return true;
        }

        // An audio graph voice's block: the graph runs once, its stereo out gets the voice's gains and fade. False when it has ended.
        bool MixGraphBlock(State& state, MixVoice& voice, float* out, uint32_t slot)
        {
            AudioGraphInstance& graph = *voice.Graph;
            graph.SetVoicePitch(std::clamp(voice.Rate, 0.0f, kMaxRate));
            const bool running = graph.Process();
            if (const uint64_t starved = graph.TakeStarvations())
                state.Starvations.fetch_add(starved, std::memory_order_relaxed);
            const float* left = graph.Left();
            const float* right = graph.Right();
            const float stepL = (voice.TargetL - voice.GainL) / static_cast<float>(kBlock);
            const float stepR = (voice.TargetR - voice.GainR) / static_cast<float>(kBlock);
            float peakL = 0.0f, peakR = 0.0f;
            bool faded = false;
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                float l = left[i], r = right[i];
                if (voice.Downmix)
                    l = r = 0.5f * (l + r);
                peakL = std::max(peakL, std::fabs(left[i]));
                peakR = std::max(peakR, std::fabs(right[i]));
                out[i * 2] += l * voice.GainL * voice.Fade;
                out[i * 2 + 1] += r * voice.GainR * voice.Fade;
                voice.GainL += stepL;
                voice.GainR += stepR;
                if (voice.FadeStep != 0.0f && !faded)
                {
                    voice.Fade += voice.FadeStep;
                    if (voice.Fade >= 1.0f)
                    {
                        voice.Fade = 1.0f;
                        voice.FadeStep = 0.0f;
                    }
                    else if (voice.Fade <= 0.0f)
                    {
                        voice.Fade = 0.0f;
                        faded = true; // stopped: the rest of the block silent
                    }
                }
            }
            voice.GainL = voice.TargetL;
            voice.GainR = voice.TargetR;
            for (uint32_t c = 0; c < 2; ++c)
            {
                const float held = state.VoicePeak[slot * 2 + c].load(std::memory_order_relaxed) * 0.985f;
                state.VoicePeak[slot * 2 + c].store(std::max(held, c == 0 ? peakL : peakR), std::memory_order_relaxed);
            }
            return running && !faded;
        }

        // Steam Audio on a 3D voice's block (in state.VoiceBuffer, its channels equal: summed and unpanned): the direct effect (air
        // absorption, occlusion, transmission -- a 3-band EQ and gain), then the HRTF (binaural) or panning into stereo.
        void SpatializeVoice(State& state, MixVoice& voice, uint32_t slot)
        {
            if (voice.Spatial == 0 || !state.DirectEffects[slot])
                return;
            float* buffer = state.VoiceBuffer.data();
            // Its reflections first (without the distance curve: the simulated response carries the distance), into the
            // ambisonic sum the decoder turns into stereo once all voices are in.
            if (voice.ReflectionSlot >= 0 && voice.Reflection.ir && state.ReflectionsOn)
            {
                const float step = (voice.ReflectionGain - voice.ReflectionApplied) / static_cast<float>(kBlock);
                float gain = voice.ReflectionApplied;
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    gain += step;
                    state.ReflectionInput[i] = buffer[i * 2] * gain;
                }
                float* input = state.ReflectionInput;
                float* channels[SpatialAudio::ReflectionChannels];
                for (int c = 0; c < SpatialAudio::ReflectionChannels; ++c)
                    channels[c] = state.AmbisonicsVoice[c];
                IPLAudioBuffer reflectionIn{ 1, static_cast<IPLint32>(kBlock), &input };
                IPLAudioBuffer reflectionOut{ SpatialAudio::ReflectionChannels, static_cast<IPLint32>(kBlock), channels };
                iplReflectionEffectApply(state.ReflectionEffects[voice.ReflectionSlot], &voice.Reflection, &reflectionIn, &reflectionOut, state.ReflectionMixer);
                state.ReflectionIrSize = voice.Reflection.irSize;
            }
            voice.ReflectionApplied = voice.ReflectionGain;
            // The direct sound: the distance curve, ramped.
            {
                const float step = (voice.Attenuation - voice.AttenuationApplied) / static_cast<float>(kBlock);
                float gain = voice.AttenuationApplied;
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    gain += step;
                    state.SpatialMono[i] = buffer[i * 2] * gain;
                }
                voice.AttenuationApplied = voice.Attenuation;
            }
            float* mono = state.SpatialMono;
            float* direct = state.SpatialDirect;
            float* stereo[2] = { state.SpatialLeft, state.SpatialRight };
            IPLAudioBuffer in{ 1, static_cast<IPLint32>(kBlock), &mono };
            IPLAudioBuffer filtered{ 1, static_cast<IPLint32>(kBlock), &direct };
            IPLAudioBuffer out{ 2, static_cast<IPLint32>(kBlock), stereo };
            IPLAudioBuffer* source = &in;
            if (voice.Direct.flags != 0)
            {
                iplDirectEffectApply(state.DirectEffects[slot], &voice.Direct, &in, &filtered);
                source = &filtered;
            }
            const IPLVector3 direction{ voice.Direction[0], voice.Direction[1], voice.Direction[2] };
            if (voice.Spatial == 2)
            {
                IPLBinauralEffectParams params{};
                params.direction = direction;
                params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
                params.spatialBlend = 1.0f;
                params.hrtf = SpatialAudio::Hrtf();
                iplBinauralEffectApply(state.BinauralEffects[slot], &params, source, &out);
            }
            else
            {
                IPLPanningEffectParams params{};
                params.direction = direction;
                iplPanningEffectApply(state.PanningEffects[slot], &params, source, &out);
            }
            // Its paths (A4c): what occlusion took from the direct sound, coming around corners and through doors along the baked
            // paths -- without the distance curve (the paths carry it, measured along their length), spatialized from where they
            // arrive.
            if (voice.PathingOn && state.PathEffects[slot] && (voice.PathingGain > 0.0f || voice.PathingApplied > 0.0f))
            {
                const float step = (voice.PathingGain - voice.PathingApplied) / static_cast<float>(kBlock);
                float gain = voice.PathingApplied;
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    gain += step;
                    state.PathInput[i] = buffer[i * 2] * gain;
                }
                IPLPathEffectParams params{};
                std::memcpy(params.eqCoeffs, voice.PathEq, sizeof(voice.PathEq));
                params.shCoeffs = voice.PathSh;
                params.order = SpatialAudio::ReflectionOrder;
                params.binaural = voice.Spatial == 2 ? IPL_TRUE : IPL_FALSE;
                params.hrtf = SpatialAudio::Hrtf();
                const float* b = state.ListenerBasis;
                params.listener.right = { b[0], b[1], b[2] };
                params.listener.up = { b[3], b[4], b[5] };
                params.listener.ahead = { b[6], b[7], b[8] };
                params.normalizeEQ = IPL_FALSE;
                float* pathInput = state.PathInput;
                float* pathStereo[2] = { state.PathLeft, state.PathRight };
                IPLAudioBuffer pathIn{ 1, static_cast<IPLint32>(kBlock), &pathInput };
                IPLAudioBuffer pathOut{ 2, static_cast<IPLint32>(kBlock), pathStereo };
                iplPathEffectApply(state.PathEffects[slot], &params, &pathIn, &pathOut);
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    state.SpatialLeft[i] += state.PathLeft[i];
                    state.SpatialRight[i] += state.PathRight[i];
                }
            }
            voice.PathingApplied = voice.PathingGain;
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                buffer[i * 2] = state.SpatialLeft[i];
                buffer[i * 2 + 1] = state.SpatialRight[i];
            }
        }

        // A voice's block (in state.VoiceBuffer) into its bus and its environment send: the low pass first (an interior / exterior
        // muffle, its cutoff moving in octaves), the send's gain ramped.
        void RouteVoice(State& state, MixVoice& voice, MixerLayout& layout)
        {
            float* buffer = state.VoiceBuffer.data();
            const float sampleRate = static_cast<float>(state.SampleRate);
            if (voice.LowPass < kLowPassOff || voice.LowPassActive)
            {
                if (!voice.LowPassActive)
                {
                    voice.LowPassActive = true;
                    voice.LowPassFilter[0].Reset();
                    voice.LowPassFilter[1].Reset();
                    voice.LowPassRamp = {};
                    float open, unused;
                    voice.LowPassRamp.Begin(kLowPassOff, sampleRate, open, unused); // (it closes from fully open)
                }
                float from, to;
                voice.LowPassRamp.Begin(voice.LowPass, sampleRate, from, to);
                for (uint32_t start = 0; start < kBlock; start += AudioDSP::kCoefficientStep)
                {
                    const float frequency = AudioDSP::FrequencyRamp::At(from, to, static_cast<float>(start + AudioDSP::kCoefficientStep) / static_cast<float>(kBlock));
                    voice.LowPassFilter[0].Set(AudioDSP::BiquadType::LowPass, frequency, 0.707f, 0.0f, sampleRate);
                    AudioDSP::Biquad& right = voice.LowPassFilter[1];
                    right.B0 = voice.LowPassFilter[0].B0; right.B1 = voice.LowPassFilter[0].B1; right.B2 = voice.LowPassFilter[0].B2;
                    right.A1 = voice.LowPassFilter[0].A1; right.A2 = voice.LowPassFilter[0].A2;
                    for (uint32_t i = start; i < start + AudioDSP::kCoefficientStep; ++i)
                    {
                        buffer[i * 2] = voice.LowPassFilter[0].Process(buffer[i * 2]);
                        buffer[i * 2 + 1] = right.Process(buffer[i * 2 + 1]);
                    }
                }
                if (voice.LowPass >= kLowPassOff && to >= kLowPassOff)
                    voice.LowPassActive = false; // fully open again: off
            }
            RuntimeBus& bus = layout.Buses[voice.Bus < layout.Buses.size() ? voice.Bus : 0];
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                bus.Left[i] += buffer[i * 2];
                bus.Right[i] += buffer[i * 2 + 1];
            }
            if (voice.SendBus >= 0 && static_cast<size_t>(voice.SendBus) < layout.Buses.size() && (voice.SendGain > 0.0f || voice.SendApplied > 0.0f))
            {
                RuntimeBus& send = layout.Buses[voice.SendBus];
                const float step = (voice.SendGain - voice.SendApplied) / static_cast<float>(kBlock);
                float gain = voice.SendApplied;
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    gain += step;
                    send.Left[i] += buffer[i * 2] * gain;
                    send.Right[i] += buffer[i * 2 + 1] * gain;
                }
            }
            voice.SendApplied = voice.SendGain;
        }

        // A bus's output into another bus, its gain ramped.
        void AddInto(RuntimeBus& target, const float* left, const float* right, float from, float to)
        {
            const float step = (to - from) / static_cast<float>(kBlock);
            float gain = from;
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                gain += step;
                target.Left[i] += left[i] * gain;
                target.Right[i] += right[i] * gain;
            }
        }

        // One bus: its effects, pre-fader sends, ducking and fader, meters, post-fader sends, into its parent. Its buffer is its
        // output afterwards (sidechains and ducking read it).
        void ProcessBus(State& state, MixerLayout& layout, uint32_t index)
        {
            RuntimeBus& bus = layout.Buses[index];
            float reduction = 0.0f;
            for (RuntimeEffect& effect : bus.Effects)
            {
                if (effect.Bypass)
                    continue;
                const RuntimeBus* key = effect.Sidechain >= 0 ? &layout.Buses[effect.Sidechain] : nullptr;
                effect.Instance->Process(bus.Left, bus.Right, kBlock, key ? key->Left : nullptr, key ? key->Right : nullptr);
                reduction = std::max(reduction, effect.Instance->GetGainReductionDb());
            }
            for (RuntimeSend& send : bus.Sends)
            {
                if (send.PreFader && send.Target >= 0)
                {
                    AddInto(layout.Buses[send.Target], bus.Left, bus.Right, send.Applied, send.Gain);
                    send.Applied = send.Gain;
                }
            }
            // Ducking: the source's output level this block against the threshold, smoothed in dB.
            if (bus.DuckSource >= 0)
            {
                const RuntimeBus& source = layout.Buses[bus.DuckSource];
                float level = 0.0f;
                for (uint32_t i = 0; i < kBlock; ++i)
                    level = std::max(level, std::max(std::fabs(source.Left[i]), std::fabs(source.Right[i])));
                const float target = level > bus.DuckThreshold ? bus.DuckDepthDb : 0.0f;
                const float coefficient = target < bus.DuckDb ? bus.DuckAttack : bus.DuckRelease;
                bus.DuckDb = target + (bus.DuckDb - target) * coefficient;
                reduction = std::max(reduction, -bus.DuckDb);
            }
            else
                bus.DuckDb = 0.0f;
            const float gain = bus.Target * (bus.DuckDb < -0.001f ? AudioDSP::DbToLinear(bus.DuckDb) : 1.0f);
            const float step = (gain - bus.Applied) / static_cast<float>(kBlock);
            float g = bus.Applied;
            float peakL = 0.0f, peakR = 0.0f, sumL = 0.0f, sumR = 0.0f;
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                g += step;
                const float l = bus.Left[i] * g;
                const float r = bus.Right[i] * g;
                bus.Left[i] = l;
                bus.Right[i] = r;
                peakL = std::max(peakL, std::fabs(l));
                peakR = std::max(peakR, std::fabs(r));
                sumL += l * l;
                sumR += r * r;
            }
            bus.Applied = gain;
            for (RuntimeSend& send : bus.Sends)
            {
                if (!send.PreFader && send.Target >= 0)
                {
                    AddInto(layout.Buses[send.Target], bus.Left, bus.Right, send.Applied, send.Gain);
                    send.Applied = send.Gain;
                }
            }
            if (bus.Parent >= 0)
                AddInto(layout.Buses[bus.Parent], bus.Left, bus.Right, 1.0f, 1.0f);

            // Meters: peaks falling ~20 dB a second, RMS over ~300 ms.
            if (index < AudioEngine::MaxBuses)
            {
                std::atomic<float>* meter = &state.BusMeters[index * 5];
                const float rmsCoefficient = std::exp(-static_cast<float>(kBlock) / (0.3f * static_cast<float>(state.SampleRate)));
                bus.PowerLeft = rmsCoefficient * bus.PowerLeft + (1.0f - rmsCoefficient) * sumL / static_cast<float>(kBlock);
                bus.PowerRight = rmsCoefficient * bus.PowerRight + (1.0f - rmsCoefficient) * sumR / static_cast<float>(kBlock);
                meter[0].store(std::max(meter[0].load(std::memory_order_relaxed) * 0.985f, peakL), std::memory_order_relaxed);
                meter[1].store(std::max(meter[1].load(std::memory_order_relaxed) * 0.985f, peakR), std::memory_order_relaxed);
                meter[2].store(std::sqrt(bus.PowerLeft), std::memory_order_relaxed);
                meter[3].store(std::sqrt(bus.PowerRight), std::memory_order_relaxed);
                meter[4].store(reduction, std::memory_order_relaxed);
            }
        }

        void MixBlock(State& state)
        {
            NOX_PROFILE_SCOPE("Audio Mix Block");
            Command command;
            while (state.Commands.Pop(command))
                ApplyCommand(state, command);

            MixerLayout& layout = *state.Layout;
            for (RuntimeBus& bus : layout.Buses)
            {
                std::memset(bus.Left, 0, sizeof(bus.Left));
                std::memset(bus.Right, 0, sizeof(bus.Right));
            }

            for (uint32_t slot = 0; slot < AudioEngine::MaxVoices; ++slot)
            {
                MixVoice& voice = state.MixVoices[slot];
                if (!voice.Active)
                    continue;
                const bool playsWhilePaused = voice.Bus < layout.Buses.size() && layout.Buses[voice.Bus].PlaysWhilePaused;
                if (voice.Paused || (state.GroupPaused[voice.Group] && !playsWhilePaused))
                    continue;
                if (voice.Waiting && !voice.Stopping)
                    continue;
                bool running = true;
                state.VoiceBuffer.fill(0.0f);
                if (voice.Waiting)
                    running = false; // (stopped before it was ever heard)
                else
                {
                    running = voice.Graph ? MixGraphBlock(state, voice, state.VoiceBuffer.data(), slot)
                                          : MixVoiceBlock(state, voice, state.VoiceBuffer.data());
                    SpatializeVoice(state, voice, slot);
                    RouteVoice(state, voice, layout);
                }
                if (!running)
                {
                    if (voice.ReflectionSlot >= 0 && voice.Reflection.ir && state.ReflectionsOn)
                    {
                        state.ReflectionTailBlocks[voice.ReflectionSlot] =
                            static_cast<uint32_t>(SpatialAudio::MaxReflectionDuration * static_cast<float>(state.SampleRate)) / kBlock + 1;
                        state.ReflectionTailParams[voice.ReflectionSlot] = voice.Reflection;
                    }
                    voice.ReflectionSlot = -1;
                    voice.Active = false;
                    voice.Clip = nullptr;
                    voice.Stream = nullptr;
                    voice.Graph = nullptr;
                    state.Finished.Push({ slot }); // (room for every slot: a slot is reused only after its event is read)
                }
            }

            // Every voice's reflections, decoded once (HRTF or panning, turned with the listener) into the reflections' bus.
            if (state.ReflectionsOn)
            {
                // The tails of voices that ended.
                std::memset(state.ReflectionInput, 0, sizeof(state.ReflectionInput));
                for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                {
                    if (state.ReflectionTailBlocks[slot] == 0)
                        continue;
                    --state.ReflectionTailBlocks[slot];
                    float* input = state.ReflectionInput;
                    float* channels[SpatialAudio::ReflectionChannels];
                    for (int c = 0; c < SpatialAudio::ReflectionChannels; ++c)
                        channels[c] = state.AmbisonicsVoice[c];
                    IPLAudioBuffer silence{ 1, static_cast<IPLint32>(kBlock), &input };
                    IPLAudioBuffer tail{ SpatialAudio::ReflectionChannels, static_cast<IPLint32>(kBlock), channels };
                    iplReflectionEffectApply(state.ReflectionEffects[slot], &state.ReflectionTailParams[slot], &silence, &tail, state.ReflectionMixer);
                }
                float* channels[SpatialAudio::ReflectionChannels];
                for (int c = 0; c < SpatialAudio::ReflectionChannels; ++c)
                    channels[c] = state.Ambisonics[c];
                float* stereo[2] = { state.SpatialLeft, state.SpatialRight };
                IPLAudioBuffer ambisonics{ SpatialAudio::ReflectionChannels, static_cast<IPLint32>(kBlock), channels };
                IPLAudioBuffer out{ 2, static_cast<IPLint32>(kBlock), stereo };
                IPLReflectionEffectParams mixParams{};
                mixParams.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
                mixParams.numChannels = SpatialAudio::ReflectionChannels;
                mixParams.irSize = state.ReflectionIrSize;
                iplReflectionMixerApply(state.ReflectionMixer, &mixParams, &ambisonics);
                IPLAmbisonicsDecodeEffectParams params{};
                params.order = SpatialAudio::ReflectionOrder;
                params.hrtf = SpatialAudio::Hrtf();
                const float* b = state.ListenerBasis;
                params.orientation.right = { b[0], b[1], b[2] };
                params.orientation.up = { b[3], b[4], b[5] };
                params.orientation.ahead = { b[6], b[7], b[8] };
                params.binaural = state.ReflectionsBinaural ? IPL_TRUE : IPL_FALSE;
                iplAmbisonicsDecodeEffectApply(state.ReflectionDecode, &params, &ambisonics, &out);
                RuntimeBus& target = layout.Buses[static_cast<size_t>(state.ReflectionsBus) < layout.Buses.size() ? state.ReflectionsBus : 0];
                AddInto(target, state.SpatialLeft, state.SpatialRight, state.ReflectionsApplied, state.ReflectionsLevel);
                state.ReflectionsApplied = state.ReflectionsLevel;
            }
            // The reflections' own meter (what they add to their bus, alone): peak and RMS like a bus's.
            {
                float sum = 0.0f, peak = 0.0f;
                if (state.ReflectionsOn)
                    for (uint32_t i = 0; i < kBlock; ++i)
                    {
                        const float l = state.SpatialLeft[i] * state.ReflectionsLevel, r = state.SpatialRight[i] * state.ReflectionsLevel;
                        sum += 0.5f * (l * l + r * r);
                        peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
                    }
                const float rmsCoefficient = std::exp(-static_cast<float>(kBlock) / (0.3f * static_cast<float>(state.SampleRate)));
                state.ReflectionsPower = rmsCoefficient * state.ReflectionsPower + (1.0f - rmsCoefficient) * sum / static_cast<float>(kBlock);
                state.ReflectionsMeter[0].store(std::max(state.ReflectionsMeter[0].load(std::memory_order_relaxed) * 0.985f, peak), std::memory_order_relaxed);
                state.ReflectionsMeter[1].store(std::sqrt(state.ReflectionsPower), std::memory_order_relaxed);
            }

            for (uint32_t index : layout.Order)
                ProcessBus(state, layout, index);

            // The device takes floats in [-1, 1] (Master's Limiter keeps it there without distortion; this only guards).
            const RuntimeBus& master = layout.Buses[0];
            for (uint32_t i = 0; i < kBlock; ++i)
            {
                state.Block[i * 2] = std::clamp(master.Left[i], -1.0f, 1.0f);
                state.Block[i * 2 + 1] = std::clamp(master.Right[i], -1.0f, 1.0f);
            }
        }

        void WriteOutput(State& state)
        {
            uint32_t written = 0;
            while (written < kBlock)
            {
                ma_uint32 frames = kBlock - written;
                void* destination = nullptr;
                if (ma_pcm_rb_acquire_write(&state.Output, &frames, &destination) != MA_SUCCESS || frames == 0)
                    break;
                std::memcpy(destination, state.Block.data() + written * 2, sizeof(float) * frames * 2);
                ma_pcm_rb_commit_write(&state.Output, frames);
                written += frames;
            }
        }

        void MixerMain(State& state)
        {
            Profiler::Get().SetThreadName("Audio Mixer");
#ifdef _WIN32
            HANDLE task = RaiseMixerPriority();
#endif
#ifdef NOX_AUDIO_SSE
            // Flush denormals to zero: a reverb's or a filter's tail decaying toward silence would otherwise run at a fraction of the
            // speed (what every audio engine's mixer thread sets).
            _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
            const float blockMilliseconds = 1000.0f * static_cast<float>(kBlock) / static_cast<float>(state.SampleRate);
            auto lastSlowWarning = std::chrono::steady_clock::now() - std::chrono::seconds(10);
            // A device starts by filling its whole buffer (all its periods at once); after that it asks for a period at a time.
            bool started = false;
            while (state.Running.load(std::memory_order_acquire))
            {
                // A device period plus two blocks ahead: one slow block never reaches the speakers as a gap.
                const uint32_t lead = (started ? state.PeriodFrames : state.DeviceBufferFrames) + 2 * kBlock;
                const uint32_t target = std::min(kOutputRingFrames - kBlock, lead);
                started = true;
                while (ma_pcm_rb_available_read(&state.Output) < target && state.Running.load(std::memory_order_relaxed))
                {
                    const auto begin = std::chrono::steady_clock::now();
                    MixBlock(state);
                    WriteOutput(state);
                    const float milliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
                    const float average = state.MixAverage.load(std::memory_order_relaxed);
                    state.MixAverage.store(average + (milliseconds - average) * 0.02f, std::memory_order_relaxed);
                    if (milliseconds > state.MixMax.load(std::memory_order_relaxed))
                        state.MixMax.store(milliseconds, std::memory_order_relaxed);
                    if (milliseconds > blockMilliseconds && begin - lastSlowWarning > std::chrono::seconds(5))
                    {
                        NOX_CORE_WARN("Audio: a block took {:.2f} ms to mix, longer than the block ({:.2f} ms)", milliseconds, blockMilliseconds);
                        lastSlowWarning = begin;
                    }
                }
                state.MixerReady.store(true, std::memory_order_release);
                ma_event_wait(&state.MixerWake);
            }
#ifdef _WIN32
            if (task)
                AvRevertMmThreadCharacteristics(task);
#endif
        }

        // The device's callback: copies what the mixer has ready and wakes it. Never mixes.
        void DeviceCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount)
        {
            State& state = *static_cast<State*>(device->pUserData);
            auto* out = static_cast<float*>(output);
            ma_uint32 done = 0;
            while (done < frameCount)
            {
                ma_uint32 frames = frameCount - done;
                void* source = nullptr;
                if (ma_pcm_rb_acquire_read(&state.Output, &frames, &source) != MA_SUCCESS || frames == 0)
                    break;
                std::memcpy(out + done * 2, source, sizeof(float) * frames * 2);
                ma_pcm_rb_commit_read(&state.Output, frames);
                done += frames;
            }
            if (done < frameCount)
            {
                std::memset(out + done * 2, 0, sizeof(float) * (frameCount - done) * 2);
                state.Underruns.fetch_add(1, std::memory_order_relaxed);
            }
            ma_event_signal(&state.MixerWake);
        }

        // ---- Stream thread ------------------------------------------------------------------------------------------------------

        void FillStream(StreamState& stream)
        {
            if (stream.Ended.load(std::memory_order_relaxed) || stream.Failed.load(std::memory_order_relaxed))
                return;
            bool justLooped = false;
            for (;;)
            {
                ma_uint32 frames = ma_pcm_rb_available_write(&stream.Ring);
                if (frames < 512)
                    return;
                frames = std::min<ma_uint32>(frames, 4096);
                void* destination = nullptr;
                if (ma_pcm_rb_acquire_write(&stream.Ring, &frames, &destination) != MA_SUCCESS || frames == 0)
                    return;
                const uint64_t read = stream.Decoder.Read(static_cast<float*>(destination), frames);
                ma_pcm_rb_commit_write(&stream.Ring, static_cast<ma_uint32>(read));
                if (read == frames)
                {
                    justLooped = false;
                    continue;
                }
                // The end of the file.
                if (!stream.Loop.load(std::memory_order_relaxed))
                {
                    stream.Ended.store(true, std::memory_order_release);
                    return;
                }
                if ((read == 0 && justLooped) || !stream.Decoder.Seek(0)) // (an empty file would loop forever)
                {
                    stream.Failed.store(true, std::memory_order_release);
                    return;
                }
                justLooped = read == 0;
            }
        }

        void StreamMain(State& state)
        {
            Profiler::Get().SetThreadName("Audio Streams");
            std::vector<std::shared_ptr<StreamState>> streams;
            std::vector<std::shared_ptr<AudioClipStream>> clipStreams;
            while (state.Running.load(std::memory_order_acquire))
            {
                {
                    std::unique_lock lock(state.StreamMutex);
                    streams = state.Streams;
                    clipStreams = state.ClipStreams;
                }
                for (const std::shared_ptr<StreamState>& stream : streams)
                    FillStream(*stream);
                for (const std::shared_ptr<AudioClipStream>& stream : clipStreams)
                    stream->Fill();
                streams.clear();
                clipStreams.clear(); // (a stream released meanwhile is destroyed here, off the game thread's path)
                std::unique_lock lock(state.StreamMutex);
                state.StreamWake.wait_for(lock, std::chrono::milliseconds(10));
            }
        }

        // ---- Game side ----------------------------------------------------------------------------------------------------------

        bool Push(State& state, const Command& command)
        {
            if (state.Commands.Push(command))
                return true;
            NOX_CORE_ERROR("Audio: the command queue is full");
            return false;
        }

        GameVoice* Find(State* state, AudioVoiceHandle handle)
        {
            if (!state || handle == 0)
                return nullptr;
            const uint32_t slot = static_cast<uint32_t>(handle & 0xffffffffu);
            const uint32_t generation = static_cast<uint32_t>(handle >> 32);
            if (slot >= AudioEngine::MaxVoices)
                return nullptr;
            GameVoice& voice = state->GameVoices[slot];
            return voice.Alive && voice.Generation == generation ? &voice : nullptr;
        }

        uint32_t SlotOf(AudioVoiceHandle handle) { return static_cast<uint32_t>(handle & 0xffffffffu); }

        float Attenuate(float distance, const AudioSpatialSettings& settings)
        {
            return AudioAttenuationGain(settings.Attenuation, settings.MinDistance, settings.MaxDistance, distance);
        }

        MixParams ComputeMix(const State& state, const GameVoice& voice)
        {
            const AudioPlayDesc& desc = voice.Desc;
            MixParams params;
            const float volume = std::max(desc.Volume, 0.0f) * std::max(voice.Environment.Volume, 0.0f);
            float doppler = 1.0f;
            if (!desc.Spatial)
            {
                params.GainL = params.GainR = volume;
            }
            else
            {
                const float metersPerUnit = WorldUnits::ToMeters(1.0f);
                const glm::vec3 offset = glm::vec3(desc.Position - state.ListenerPosition) * metersPerUnit; // (listener-relative: float is enough)
                const float distance = glm::length(offset);
                const float gain = volume * Attenuate(distance, desc.Spatialization);
                // Equal-power panning (-3 dB in the middle) by how far to the side the sound is.
                float pan = 0.0f;
                if (distance > 1e-4f)
                {
                    const glm::vec3 local = glm::conjugate(state.ListenerOrientation) * offset;
                    pan = std::clamp(local.x / distance, -1.0f, 1.0f);
                }
                const float angle = (pan + 1.0f) * 0.25f * glm::pi<float>();
                params.GainL = std::cos(angle) * gain;
                params.GainR = std::sin(angle) * gain;
                // Steam Audio pans it instead (HRTF or its panning, on the mixer thread): one gain, and the direction it comes from.
                if (state.SteamAudio)
                {
                    const AudioSpatialization mode = desc.Spatialization.Spatialization;
                    params.Spatial = mode == AudioSpatialization::Binaural || (mode == AudioSpatialization::Default && state.Headphones) ? 2 : 1;
                    // Its gain without the distance curve (the mixer applies the curve to the direct sound only); its reflections
                    // fade out over the last fifth of Max Distance.
                    params.GainL = params.GainR = volume;
                    params.Attenuation = Attenuate(distance, desc.Spatialization);
                    const float maximum = std::max(desc.Spatialization.MaxDistance, 0.01f);
                    params.ReflectionGain = desc.Spatialization.Reflections ? std::clamp((1.0f - distance / maximum) * 5.0f, 0.0f, 1.0f) : 0.0f;
                    if (distance > 1e-4f)
                        params.Direction = glm::normalize(glm::conjugate(state.ListenerOrientation) * offset);
                }

                // Doppler (OpenAL's model): speeds along the line from the source to the listener.
                const float level = std::max(desc.Spatialization.DopplerLevel, 0.0f);
                if (level > 0.0f && distance > 1e-3f)
                {
                    const glm::vec3 toListener = -offset / distance;
                    const float limit = 0.5f * kSpeedOfSound;
                    const float source = std::clamp(level * glm::dot(voice.Velocity, toListener), -limit, limit);
                    const float listener = std::clamp(level * glm::dot(state.ListenerVelocity, toListener), -limit, limit);
                    doppler = (kSpeedOfSound - listener) / (kSpeedOfSound - source);
                }
            }
            const float clipRate = desc.Clip ? static_cast<float>(desc.Clip->GetSampleRate()) : static_cast<float>(state.SampleRate);
            params.Rate = std::clamp(std::max(desc.Pitch, 0.0f) * doppler * clipRate / static_cast<float>(state.SampleRate), 0.0f, kMaxRate);
            return params;
        }

        // A new voice will be traced by the direct simulation (AudioEngine::Update): it then waits for its first result.
        bool WillSimulate(const State& state, const AudioPlayDesc& desc)
        {
            const AudioSpatialSettings& settings = desc.Spatialization;
            return state.SteamAudio && desc.Spatial && (settings.Occlusion || settings.AirAbsorption) &&
                   glm::length(desc.Position - state.ListenerPosition) * WorldUnits::ToMeters(1.0f) <= settings.MaxDistance;
        }

        // A command's Steam Audio part: how it is panned, where from, and the simulation's occlusion / transmission / air absorption.
        void FillSpatial(Command& command, const MixParams& params, const GameVoice& voice)
        {
            command.Spatial = params.Spatial;
            command.Direction[0] = params.Direction.x;
            command.Direction[1] = params.Direction.y;
            command.Direction[2] = params.Direction.z;
            const AudioSpatialSettings& settings = voice.Desc.Spatialization;
            uint32_t flags = 0;
            if (voice.Source && settings.AirAbsorption)
                flags |= IPL_DIRECTEFFECTFLAGS_APPLYAIRABSORPTION;
            if (voice.Source && settings.Occlusion)
                flags |= IPL_DIRECTEFFECTFLAGS_APPLYOCCLUSION | IPL_DIRECTEFFECTFLAGS_APPLYTRANSMISSION;
            command.DirectFlags = static_cast<uint8_t>(flags);
            command.Attenuation = params.Attenuation;
            command.ReflectionGain = params.ReflectionGain;
            command.Occlusion = voice.Direct.Occlusion;
            for (int band = 0; band < 3; ++band)
            {
                command.Transmission[band] = voice.Direct.Transmission[band];
                command.AirAbsorption[band] = voice.Direct.AirAbsorption[band];
            }
            // Pathing fills what occlusion takes away (the occluded share of the direct sound comes along the paths instead).
            command.PathingGain = voice.PathingSlot >= 0 && voice.PathingValid && voice.Source && settings.Occlusion
                                      ? std::clamp(1.0f - voice.Direct.Occlusion, 0.0f, 1.0f)
                                      : 0.0f;
        }

        // Velocity from the position change, smoothed, with teleports ignored.
        void TrackVelocity(const glm::dvec3& position, glm::dvec3& previous, bool& hasPrevious, glm::vec3& velocity, float deltaSeconds)
        {
            if (!hasPrevious)
            {
                previous = position;
                hasPrevious = true;
                velocity = glm::vec3(0.0f);
                return;
            }
            if (deltaSeconds <= 0.0f)
                return;
            const glm::vec3 raw = glm::vec3(position - previous) * (WorldUnits::ToMeters(1.0f) / deltaSeconds);
            previous = position;
            if (glm::length(raw) > kTeleportSpeed)
            {
                velocity = glm::vec3(0.0f);
                return;
            }
            const float blend = 1.0f - std::exp(-deltaSeconds / kVelocitySmoothing);
            velocity += (raw - velocity) * blend;
        }

        void ReleaseVoice(State& state, uint32_t slot)
        {
            GameVoice& voice = state.GameVoices[slot];
            SpatialAudio::ReleaseSource(voice.Source);
            if (voice.ReflectionSlot >= 0)
            {
                state.ReflectionVoice[voice.ReflectionSlot] = -1;
                state.ReflectionFreedAt[voice.ReflectionSlot] = static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) * 1e-9;
            }
            voice.ReflectionSlot = -1;
            if (voice.PathingSlot >= 0)
                state.PathingVoice[voice.PathingSlot] = -1;
            voice.PathingSlot = -1;
            voice.PathingValid = false;
            if (voice.Stream)
            {
                std::scoped_lock lock(state.StreamMutex);
                std::erase(state.Streams, voice.Stream);
            }
            if (voice.Graph && !voice.Graph->GetStreams().empty())
            {
                std::scoped_lock lock(state.StreamMutex);
                for (const std::shared_ptr<AudioClipStream>& stream : voice.Graph->GetStreams())
                    std::erase(state.ClipStreams, stream);
            }
            voice.Stream.reset();
            voice.Graph.reset();
            voice.Desc = {};
            voice.Alive = false;
            state.FreeSlots.push_back(slot);
        }
    }

    namespace
    {
        uint64_t EffectKey(uint64_t bus, uint64_t effect) { return bus * 0x9E3779B97F4A7C15ull ^ (effect + 0x632BE59BD9B4E019ull); }

        // The Acoustic Reverb (A4c; Steam Audio's own reverb effect, as its Unity plugin's mixer effect runs it): what reaches its
        // bus, summed to mono, convolved with the reverb at the listener (order-1 ambisonics: from the directions it comes back
        // from), decoded to stereo turned with the listener (HRTF or panning). The response comes from the simulation (baked probes
        // or traced, Mixer > Acoustics > Listener Reverb); without one -- outside every baked volume -- silence goes in and the last
        // reverb rings out. Mixer thread: the response and the listener's orientation are the engine's, read where they are written.
        class AcousticReverbEffect final : public AudioEffect
        {
        public:
            explicit AcousticReverbEffect(const State& state)
                : AudioEffect(AudioEffectType::AcousticReverb, static_cast<float>(state.SampleRate)), m_State(state)
            {
                IPLAudioSettings audio{ static_cast<IPLint32>(state.SampleRate), static_cast<IPLint32>(kBlock) };
                IPLReflectionEffectSettings reflection{};
                reflection.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
                reflection.irSize = static_cast<IPLint32>(SpatialAudio::MaxReflectionDuration * static_cast<float>(state.SampleRate));
                reflection.numChannels = SpatialAudio::ReflectionChannels;
                IPLAmbisonicsDecodeEffectSettings decode{};
                decode.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
                decode.hrtf = SpatialAudio::Hrtf();
                decode.maxOrder = SpatialAudio::ReflectionOrder;
                if (iplReflectionEffectCreate(SpatialAudio::Context(), &audio, &reflection, &m_Reflection) != IPL_STATUS_SUCCESS ||
                    iplAmbisonicsDecodeEffectCreate(SpatialAudio::Context(), &audio, &decode, &m_Decode) != IPL_STATUS_SUCCESS)
                    Release();
            }
            ~AcousticReverbEffect() override { Release(); }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const float wet = AudioDSP::DbToGain(Value(0)), dry = AudioDSP::DbToGain(Value(1));
                if (!m_Reflection || frames != kBlock)
                {
                    for (uint32_t i = 0; i < frames; ++i)
                    {
                        left[i] *= dry;
                        right[i] *= dry;
                    }
                    return;
                }
                const bool valid = m_State.ListenerReverbValid;
                if (valid)
                {
                    m_Params = m_State.ListenerReverbParams;
                    m_HasParams = true;
                }
                for (uint32_t i = 0; i < kBlock; ++i)
                    m_Mono[i] = valid ? 0.5f * (left[i] + right[i]) : 0.0f;
                if (m_HasParams)
                {
                    float* mono = m_Mono;
                    float* channels[SpatialAudio::ReflectionChannels];
                    for (int c = 0; c < SpatialAudio::ReflectionChannels; ++c)
                        channels[c] = m_Ambisonics[c];
                    float* stereo[2] = { m_Left, m_Right };
                    IPLAudioBuffer in{ 1, static_cast<IPLint32>(kBlock), &mono };
                    IPLAudioBuffer ambisonics{ SpatialAudio::ReflectionChannels, static_cast<IPLint32>(kBlock), channels };
                    IPLAudioBuffer out{ 2, static_cast<IPLint32>(kBlock), stereo };
                    iplReflectionEffectApply(m_Reflection, &m_Params, &in, &ambisonics, nullptr);
                    IPLAmbisonicsDecodeEffectParams params{};
                    params.order = SpatialAudio::ReflectionOrder;
                    params.hrtf = SpatialAudio::Hrtf();
                    const float* b = m_State.ListenerBasis;
                    params.orientation.right = { b[0], b[1], b[2] };
                    params.orientation.up = { b[3], b[4], b[5] };
                    params.orientation.ahead = { b[6], b[7], b[8] };
                    params.binaural = Value(2) > 0.5f ? IPL_TRUE : IPL_FALSE;
                    iplAmbisonicsDecodeEffectApply(m_Decode, &params, &ambisonics, &out);
                }
                else
                {
                    std::memset(m_Left, 0, sizeof(m_Left));
                    std::memset(m_Right, 0, sizeof(m_Right));
                }
                // Wet and dry ramped over the block (a fader move never clicks).
                const float wetStep = (wet - m_Wet) / static_cast<float>(kBlock), dryStep = (dry - m_Dry) / static_cast<float>(kBlock);
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    m_Wet += wetStep;
                    m_Dry += dryStep;
                    left[i] = left[i] * m_Dry + m_Left[i] * m_Wet;
                    right[i] = right[i] * m_Dry + m_Right[i] * m_Wet;
                }
                m_Wet = wet;
                m_Dry = dry;
            }

            void Reset() override
            {
                if (m_Reflection)
                    iplReflectionEffectReset(m_Reflection);
                if (m_Decode)
                    iplAmbisonicsDecodeEffectReset(m_Decode);
                m_Wet = AudioDSP::DbToGain(Value(0));
                m_Dry = AudioDSP::DbToGain(Value(1));
            }

        private:
            void Release()
            {
                if (m_Reflection)
                    iplReflectionEffectRelease(&m_Reflection);
                if (m_Decode)
                    iplAmbisonicsDecodeEffectRelease(&m_Decode);
                m_Reflection = nullptr;
                m_Decode = nullptr;
            }

            const State& m_State;
            IPLReflectionEffect m_Reflection = nullptr;
            IPLAmbisonicsDecodeEffect m_Decode = nullptr;
            IPLReflectionEffectParams m_Params{};
            bool m_HasParams = false;
            float m_Wet = 1.0f, m_Dry = 0.0f;
            alignas(16) float m_Mono[kBlock] = {};
            alignas(16) float m_Ambisonics[SpatialAudio::ReflectionChannels][kBlock] = {};
            alignas(16) float m_Left[kBlock] = {};
            alignas(16) float m_Right[kBlock] = {};
        };

        std::shared_ptr<AudioEffect> MakeEffect(const State& state, const AudioMixerLayoutDesc::Effect& effect)
        {
            if (effect.Type == AudioEffectType::AcousticReverb)
            {
                if (!state.SteamAudio)
                    return nullptr; // (without Steam Audio there is no reverb at the listener: the bus runs without it)
                auto reverb = std::make_shared<AcousticReverbEffect>(state);
                reverb->Reset();
                return reverb;
            }
            return CreateAudioEffect(effect.Type, static_cast<float>(state.SampleRate), effect.Impulse.get());
        }

        int32_t BusOf(const State& state, uint64_t id)
        {
            const auto found = state.BusIndex.find(id);
            return found != state.BusIndex.end() ? static_cast<int32_t>(found->second) : -1;
        }

        // The order the buses run in: each after what feeds it (its children and send sources) and what it listens to (its effects'
        // sidechains and its ducking source). Edges that would close a loop are dropped (the Mixer panel does not offer them).
        std::vector<uint32_t> ProcessingOrder(MixerLayout& layout)
        {
            const uint32_t count = static_cast<uint32_t>(layout.Buses.size());
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                std::vector<std::vector<uint32_t>> after(count); // a -> the buses that must run after a
                std::vector<uint32_t> incoming(count, 0);
                auto edge = [&](int32_t from, uint32_t to)
                {
                    if (from < 0 || static_cast<uint32_t>(from) == to)
                        return;
                    after[from].push_back(to);
                    ++incoming[to];
                };
                for (uint32_t b = 0; b < count; ++b)
                {
                    const RuntimeBus& bus = layout.Buses[b];
                    if (bus.Parent >= 0)
                        edge(static_cast<int32_t>(b), static_cast<uint32_t>(bus.Parent));
                    for (const RuntimeSend& send : bus.Sends)
                        if (send.Target >= 0)
                            edge(static_cast<int32_t>(b), static_cast<uint32_t>(send.Target));
                    for (const RuntimeEffect& effect : bus.Effects)
                        edge(effect.Sidechain, b);
                    edge(bus.DuckSource, b);
                }
                std::vector<uint32_t> order;
                std::vector<uint32_t> ready;
                for (uint32_t b = 0; b < count; ++b)
                    if (incoming[b] == 0)
                        ready.push_back(b);
                while (!ready.empty())
                {
                    const uint32_t b = ready.back();
                    ready.pop_back();
                    order.push_back(b);
                    for (uint32_t next : after[b])
                        if (--incoming[next] == 0)
                            ready.push_back(next);
                }
                if (order.size() == count)
                    return order;
                // A loop: drop the sends, sidechains and ducking among the buses left over, then order again.
                NOX_CORE_WARN("Audio: the mixer's sends / sidechains form a loop; the ones in it are ignored");
                for (uint32_t b = 0; b < count; ++b)
                {
                    if (incoming[b] == 0)
                        continue;
                    RuntimeBus& bus = layout.Buses[b];
                    for (RuntimeSend& send : bus.Sends)
                        send.Target = -1;
                    for (RuntimeEffect& effect : bus.Effects)
                        effect.Sidechain = -1;
                    bus.DuckSource = -1;
                }
            }
            std::vector<uint32_t> order(count);
            for (uint32_t b = 0; b < count; ++b)
                order[b] = count - 1 - b; // (only a broken parent chain gets here)
            return order;
        }

        // A reused effect's parameters: sent as commands after the new layout (the mixer is running that instance now).
        struct PendingParameter
        {
            uint16_t Bus = 0;
            int32_t Effect = 0;
            uint32_t Parameter = 0;
            float Value = 0.0f;
        };

        MixerLayout* BuildLayout(State& state, const AudioMixerLayoutDesc& desc, std::vector<PendingParameter>* reusedParameters = nullptr)
        {
            auto layout = std::make_unique<MixerLayout>();
            const uint32_t count = static_cast<uint32_t>(std::min<size_t>(std::max<size_t>(desc.Buses.size(), 1), AudioEngine::MaxBuses));
            layout->Buses.resize(count);
            auto valid = [&](int32_t index) { return index >= 0 && static_cast<uint32_t>(index) < count ? index : -1; };
            const float blocksPerSecond = static_cast<float>(state.SampleRate) / static_cast<float>(kBlock);
            std::unordered_map<uint64_t, State::CachedEffect> effects;
            for (uint32_t b = 0; b < count; ++b)
            {
                RuntimeBus& bus = layout->Buses[b];
                if (b >= desc.Buses.size())
                {
                    bus.Id = kAudioBusMaster;
                    continue;
                }
                const AudioMixerLayoutDesc::Bus& source = desc.Buses[b];
                bus.Id = source.Id;
                bus.Parent = b == 0 ? -1 : (valid(source.Parent) >= 0 && source.Parent != static_cast<int32_t>(b) ? source.Parent : 0);
                bus.PlaysWhilePaused = source.PlaysWhilePaused;
                bus.Target = bus.Applied = std::max(source.Gain, 0.0f);
                for (const AudioMixerLayoutDesc::Effect& effect : source.Effects)
                {
                    const uint64_t key = EffectKey(source.Id, effect.Id);
                    State::CachedEffect cached;
                    bool reused = false;
                    if (const auto found = state.Effects.find(key);
                        found != state.Effects.end() && found->second.Type == effect.Type && found->second.Impulse == effect.Impulse.get())
                    {
                        cached = found->second;
                        reused = true;
                    }
                    else
                    {
                        cached.Type = effect.Type;
                        cached.Impulse = effect.Impulse.get();
                        cached.ImpulseRef = effect.Impulse;
                        cached.Instance = MakeEffect(state, effect);
                    }
                    if (!cached.Instance)
                        continue;
                    for (size_t p = 0; p < effect.Values.size(); ++p)
                    {
                        if (!reused)
                            cached.Instance->SetParameter(static_cast<uint32_t>(p), effect.Values[p]); // (not running yet: set here)
                        else if (reusedParameters)
                            reusedParameters->push_back({ static_cast<uint16_t>(b), static_cast<int32_t>(bus.Effects.size()), static_cast<uint32_t>(p), effect.Values[p] });
                    }
                    RuntimeEffect runtime;
                    runtime.Instance = cached.Instance;
                    runtime.Bypass = effect.Bypass;
                    runtime.Sidechain = valid(effect.Sidechain) == static_cast<int32_t>(b) ? -1 : valid(effect.Sidechain);
                    bus.Effects.push_back(runtime);
                    effects[key] = std::move(cached);
                }
                for (const AudioMixerLayoutDesc::Send& send : source.Sends)
                {
                    RuntimeSend runtime;
                    runtime.Target = valid(send.Target) == static_cast<int32_t>(b) ? -1 : valid(send.Target);
                    runtime.Gain = runtime.Applied = std::max(send.Gain, 0.0f);
                    runtime.PreFader = send.PreFader;
                    bus.Sends.push_back(runtime);
                }
                bus.DuckSource = valid(source.DuckSource) == static_cast<int32_t>(b) ? -1 : valid(source.DuckSource);
                bus.DuckDepthDb = std::min(source.DuckDepthDb, 0.0f);
                bus.DuckThreshold = AudioDSP::DbToLinear(source.DuckThresholdDb);
                bus.DuckAttack = std::exp(-1.0f / std::max(source.DuckAttack * blocksPerSecond, 1e-3f));
                bus.DuckRelease = std::exp(-1.0f / std::max(source.DuckRelease * blocksPerSecond, 1e-3f));
            }
            layout->Order = ProcessingOrder(*layout);
            state.Effects = std::move(effects); // (instances no longer used live on in the old layout until it is retired)
            return layout.release();
        }

        void FreeRetiredLayouts(State& state)
        {
            MixerLayout* retired = nullptr;
            while (state.Retired.Pop(retired))
                delete retired;
        }

        // Reflections (game thread, every frame): the listener's orientation to the decoder; when the reflection thread is idle,
        // the last run's results to their voices, then a new run for the nearest voices (a voice keeps its pool slot while it stays
        // among them, so its response is not reset).
        void UpdateReflections(State& state, float metersPerUnit)
        {
            const glm::quat& q = state.ListenerOrientation;
            const glm::vec3 right = q * glm::vec3(1.0f, 0.0f, 0.0f), up = q * glm::vec3(0.0f, 1.0f, 0.0f), ahead = q * glm::vec3(0.0f, 0.0f, -1.0f);
            Command basis;
            basis.Type = CommandType::ListenerBasis;
            const float values[9] = { right.x, right.y, right.z, up.x, up.y, up.z, ahead.x, ahead.y, ahead.z };
            std::memcpy(basis.Basis, values, sizeof(values));
            basis.Flag = state.Headphones;
            Push(state, basis);

            const AudioReflectionSettings& settings = state.Reflections;
            const bool enabled = settings.Enabled && settings.MaxSources > 0 && state.ReflectionDecode;
            if (!state.ReflectionsSent)
            {
                Command output;
                output.Type = CommandType::ReflectionsOutput;
                output.Flag = enabled;
                output.Bus = static_cast<uint16_t>(std::max(BusOf(state, settings.Bus), 0));
                output.Value = AudioDSP::DbToGain(settings.LevelDb);
                Push(state, output);
                state.ReflectionsSent = true;
            }
            auto release = [&](int32_t slot)
            {
                const int32_t voiceSlot = state.ReflectionVoice[slot];
                if (voiceSlot < 0)
                    return;
                state.GameVoices[voiceSlot].ReflectionSlot = -1;
                state.ReflectionVoice[slot] = -1;
                state.ReflectionFreedAt[slot] = static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) * 1e-9;
                Command command;
                command.Type = CommandType::VoiceReflections;
                command.Slot = static_cast<uint32_t>(voiceSlot);
                command.Parameter = -1;
                Push(state, command);
            };
            auto sendPathing = [&](uint32_t voiceSlot, const SpatialAudio::PathingOutputs* outputs)
            {
                GameVoice& voice = state.GameVoices[voiceSlot];
                voice.PathingValid = outputs && outputs->Valid;
                Command command;
                command.Type = CommandType::VoicePathing;
                command.Slot = voiceSlot;
                command.Flag = voice.PathingValid;
                if (voice.PathingValid)
                {
                    std::memcpy(command.PathEq, outputs->Eq, sizeof(command.PathEq));
                    std::memcpy(command.PathSh, outputs->Sh, sizeof(command.PathSh));
                }
                Push(state, command);
            };
            auto releasePathing = [&](int32_t slot)
            {
                const int32_t voiceSlot = state.PathingVoice[slot];
                if (voiceSlot < 0)
                    return;
                state.GameVoices[voiceSlot].PathingSlot = -1;
                state.PathingVoice[slot] = -1;
                if (state.GameVoices[voiceSlot].PathingValid)
                    sendPathing(static_cast<uint32_t>(voiceSlot), nullptr);
            };
            auto sendListenerReverb = [&](const IPLReflectionEffectParams* params)
            {
                if (!params && !state.ListenerReverbSent)
                    return;
                Command command;
                command.Type = CommandType::ListenerReverb;
                command.Flag = params != nullptr;
                if (params)
                    command.Reflection = *params;
                Push(state, command);
                state.ListenerReverbSent = params != nullptr;
            };
            if (!enabled)
                for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                    release(slot);
            // The listener's reverb is simulated only when an Acoustic Reverb effect is there to apply it.
            {
                SpatialAudio::ReflectionSettings simulation = SpatialAudio::GetReflectionSettings();
                const AudioListenerReverb reverb = state.HasAcousticReverb ? settings.ListenerReverb : AudioListenerReverb::Off;
                if (simulation.Reverb != reverb)
                {
                    simulation.Reverb = reverb;
                    SpatialAudio::SetReflectionSettings(simulation);
                }
                if (reverb == AudioListenerReverb::Off)
                    sendListenerReverb(nullptr);
            }

            const bool idle = SpatialAudio::ReflectionsIdle();
            if (idle && SpatialAudio::TakeReflectionResults())
            {
                for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                {
                    const int32_t voiceSlot = state.ReflectionVoice[slot];
                    Command command;
                    if (!SpatialAudio::GetReflectionParams(slot, command.Reflection))
                        continue;
                    state.ReflectionParams[slot] = command.Reflection;
                    state.ReflectionResultPosition[slot] = state.ReflectionRunPosition[slot];
                    state.ReflectionValid[slot] = true;
                    if (voiceSlot < 0)
                        continue;
                    command.Type = CommandType::VoiceReflections;
                    command.Slot = static_cast<uint32_t>(voiceSlot);
                    command.Parameter = slot;
                    Push(state, command);
                }
                IPLReflectionEffectParams reverb{};
                sendListenerReverb(SpatialAudio::GetListenerReverbParams(reverb) ? &reverb : nullptr);
                for (int32_t slot = 0; slot < SpatialAudio::MaxPathingSources; ++slot)
                {
                    const int32_t voiceSlot = state.PathingVoice[slot];
                    if (voiceSlot < 0)
                        continue;
                    const SpatialAudio::PathingOutputs outputs = SpatialAudio::GetPathingOutputs(slot);
                    if (outputs.Valid || state.GameVoices[voiceSlot].PathingValid)
                        sendPathing(static_cast<uint32_t>(voiceSlot), &outputs);
                }
            }

            // The nearest audible 3D voices that want reflections: chosen every frame, so a sound started this frame takes a slot (and
            // the response already simulated near it) from its first block -- a footstep or a shot is over before the next run.
            std::vector<std::pair<float, uint32_t>> candidates;
            std::vector<std::pair<float, uint32_t>> pathCandidates;
            for (uint32_t slot = 0; slot < AudioEngine::MaxVoices; ++slot)
            {
                const GameVoice& voice = state.GameVoices[slot];
                if (!voice.Alive || voice.StopSent || !voice.Desc.Spatial)
                    continue;
                const float distance = static_cast<float>(glm::length(voice.Desc.Position - state.ListenerPosition)) * metersPerUnit;
                if (distance > voice.Desc.Spatialization.MaxDistance)
                    continue;
                if (enabled && voice.Desc.Spatialization.Reflections)
                    candidates.emplace_back(distance, slot);
                if (voice.Desc.Spatialization.Pathing && voice.Source && voice.Desc.Spatialization.Occlusion)
                    pathCandidates.emplace_back(distance, slot);
            }
            std::sort(candidates.begin(), candidates.end());
            const size_t budget = std::min<size_t>(static_cast<size_t>(std::min(settings.MaxSources, SpatialAudio::MaxReflectionSources)), candidates.size());
            std::array<bool, AudioEngine::MaxVoices> chosen{};
            for (size_t i = 0; i < budget; ++i)
                chosen[candidates[i].second] = true;
            for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                if (state.ReflectionVoice[slot] >= 0 && !chosen[state.ReflectionVoice[slot]])
                    release(slot);
            for (size_t i = 0; i < budget; ++i)
            {
                GameVoice& voice = state.GameVoices[candidates[i].second];
                if (voice.ReflectionSlot >= 0)
                    continue;
                // The free slot last simulated nearest to it (its response is used at once), else the one free longest.
                constexpr float kReuseMeters = 8.0f;
                int32_t best = -1;
                float bestDistance = kReuseMeters;
                for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                {
                    if (state.ReflectionVoice[slot] >= 0 || !state.ReflectionValid[slot])
                        continue;
                    const float distance = static_cast<float>(glm::length(state.ReflectionResultPosition[slot] - voice.Desc.Position)) * metersPerUnit;
                    if (distance < bestDistance)
                    {
                        best = slot;
                        bestDistance = distance;
                    }
                }
                const bool reuse = best >= 0;
                if (!reuse)
                    for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
                        if (state.ReflectionVoice[slot] < 0 && (best < 0 || state.ReflectionFreedAt[slot] < state.ReflectionFreedAt[best]))
                            best = slot;
                if (best < 0)
                    break;
                state.ReflectionVoice[best] = static_cast<int32_t>(candidates[i].second);
                voice.ReflectionSlot = best;
                if (reuse)
                {
                    Command command;
                    command.Type = CommandType::VoiceReflections;
                    command.Slot = candidates[i].second;
                    command.Parameter = best;
                    command.Reflection = state.ReflectionParams[best];
                    Push(state, command);
                }
            }
            // Pathing: the nearest voices with Pathing on (their paths come with the next run's results).
            std::sort(pathCandidates.begin(), pathCandidates.end());
            const size_t pathBudget = std::min<size_t>(static_cast<size_t>(std::clamp(settings.PathingSources, 0, SpatialAudio::MaxPathingSources)), pathCandidates.size());
            std::array<bool, AudioEngine::MaxVoices> pathChosen{};
            for (size_t i = 0; i < pathBudget; ++i)
                pathChosen[pathCandidates[i].second] = true;
            for (int32_t slot = 0; slot < SpatialAudio::MaxPathingSources; ++slot)
                if (state.PathingVoice[slot] >= 0 && !pathChosen[state.PathingVoice[slot]])
                    releasePathing(slot);
            for (size_t i = 0; i < pathBudget; ++i)
            {
                GameVoice& voice = state.GameVoices[pathCandidates[i].second];
                if (voice.PathingSlot >= 0)
                    continue;
                for (int32_t slot = 0; slot < SpatialAudio::MaxPathingSources; ++slot)
                {
                    if (state.PathingVoice[slot] >= 0)
                        continue;
                    state.PathingVoice[slot] = static_cast<int32_t>(pathCandidates[i].second);
                    voice.PathingSlot = slot;
                    voice.PathingValid = false;
                    break;
                }
            }

            // A new run (the voices' positions now) once the last has finished, at most every Interval.
            const double now = static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) * 1e-9;
            if (!idle || now - state.LastReflectionStart < static_cast<double>(SpatialAudio::GetReflectionSettings().Interval))
                return;
            state.LastReflectionStart = now;
            std::vector<std::pair<int32_t, glm::dvec3>> sources;
            for (int32_t slot = 0; slot < SpatialAudio::MaxReflectionSources; ++slot)
            {
                if (state.ReflectionVoice[slot] < 0)
                    continue;
                state.ReflectionRunPosition[slot] = state.GameVoices[state.ReflectionVoice[slot]].Desc.Position;
                sources.emplace_back(slot, state.ReflectionRunPosition[slot]);
            }
            std::vector<SpatialAudio::PathingRequest> pathing;
            for (int32_t slot = 0; slot < SpatialAudio::MaxPathingSources; ++slot)
            {
                if (state.PathingVoice[slot] < 0)
                    continue;
                const AudioPlayDesc& desc = state.GameVoices[state.PathingVoice[slot]].Desc;
                SpatialAudio::PathingRequest request;
                request.Slot = slot;
                request.Position = desc.Position;
                request.Attenuation = desc.Spatialization.Attenuation;
                request.MinDistance = desc.Spatialization.MinDistance;
                request.MaxDistance = desc.Spatialization.MaxDistance;
                pathing.push_back(request);
            }
            if (!SpatialAudio::StartSimulation(sources, pathing))
            {
                // Nothing to simulate (no run): the listener's reverb and the paths are gone (left every baked volume).
                sendListenerReverb(nullptr);
                for (int32_t slot = 0; slot < SpatialAudio::MaxPathingSources; ++slot)
                    if (state.PathingVoice[slot] >= 0 && state.GameVoices[state.PathingVoice[slot]].PathingValid)
                        sendPathing(static_cast<uint32_t>(state.PathingVoice[slot]), nullptr);
            }
        }

        AudioMixerLayoutDesc MasterOnlyLayout()
        {
            AudioMixerLayoutDesc desc;
            desc.Buses.push_back({});
            desc.Buses[0].Id = kAudioBusMaster;
            return desc;
        }
    }

    void AudioEngine::Init()
    {
        if (s_State)
            return;
        s_State = new State();
        State& state = *s_State;
        for (auto& meter : state.BusMeters)
            meter.store(0.0f);
        for (MixVoice& voice : state.MixVoices)
            voice.Staging.resize(static_cast<size_t>(kStagingFrames) * 2);
        state.FreeSlots.reserve(MaxVoices);
        for (uint32_t slot = MaxVoices; slot-- > 0;)
            state.FreeSlots.push_back(slot);

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 0; // the device's own rate: no resampling in miniaudio (each voice resamples itself)
        config.dataCallback = DeviceCallback;
        config.pUserData = &state;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.noClip = MA_TRUE; // (clamped by the mixer)
        if (ma_device_init(nullptr, &config, &state.Device) != MA_SUCCESS)
        {
            NOX_CORE_WARN("Audio: no output device could be opened -- sound is off");
            return;
        }
        state.SampleRate = state.Device.sampleRate;
        state.PeriodFrames = state.Device.playback.internalPeriodSizeInFrames;
        state.DeviceBufferFrames = state.PeriodFrames * std::max<uint32_t>(state.Device.playback.internalPeriods, 1);
        state.DeviceName = state.Device.playback.name;
        // Until the project's mixer arrives (AudioMixer): Master alone.
        state.Layout = BuildLayout(state, MasterOnlyLayout());
        state.GameLayout = state.Layout;
        state.BusIndex = { { kAudioBusMaster, 0u } };
        // Steam Audio: its context and HRTF, and every voice slot's direct, binaural and panning effects (made once: nothing
        // allocates while mixing). Without it, 3D voices are panned by the engine itself.
        if (SpatialAudio::Init(state.SampleRate, kBlock))
        {
            IPLAudioSettings audio{ static_cast<IPLint32>(state.SampleRate), static_cast<IPLint32>(kBlock) };
            IPLDirectEffectSettings direct{ 1 };
            IPLBinauralEffectSettings binaural{ SpatialAudio::Hrtf() };
            IPLPanningEffectSettings panning{};
            panning.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            bool created = true;
            for (uint32_t slot = 0; slot < MaxVoices && created; ++slot)
            {
                created = iplDirectEffectCreate(SpatialAudio::Context(), &audio, &direct, &state.DirectEffects[slot]) == IPL_STATUS_SUCCESS &&
                          iplBinauralEffectCreate(SpatialAudio::Context(), &audio, &binaural, &state.BinauralEffects[slot]) == IPL_STATUS_SUCCESS &&
                          iplPanningEffectCreate(SpatialAudio::Context(), &audio, &panning, &state.PanningEffects[slot]) == IPL_STATUS_SUCCESS;
            }
            // Reflections: the pool's convolution effects, the mixer they sum into, and one decoder.
            IPLReflectionEffectSettings reflection{};
            reflection.type = IPL_REFLECTIONEFFECTTYPE_CONVOLUTION;
            reflection.irSize = static_cast<IPLint32>(SpatialAudio::MaxReflectionDuration * static_cast<float>(state.SampleRate));
            reflection.numChannels = SpatialAudio::ReflectionChannels;
            for (IPLReflectionEffect& effect : state.ReflectionEffects)
                if (created && iplReflectionEffectCreate(SpatialAudio::Context(), &audio, &reflection, &effect) != IPL_STATUS_SUCCESS)
                    effect = nullptr;
            IPLAmbisonicsDecodeEffectSettings decode{};
            decode.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            decode.hrtf = SpatialAudio::Hrtf();
            decode.maxOrder = SpatialAudio::ReflectionOrder;
            if (created && (iplAmbisonicsDecodeEffectCreate(SpatialAudio::Context(), &audio, &decode, &state.ReflectionDecode) != IPL_STATUS_SUCCESS ||
                            iplReflectionMixerCreate(SpatialAudio::Context(), &audio, &reflection, &state.ReflectionMixer) != IPL_STATUS_SUCCESS))
            {
                if (state.ReflectionDecode)
                    iplAmbisonicsDecodeEffectRelease(&state.ReflectionDecode);
                state.ReflectionDecode = nullptr; // (no reflections)
            }
            // Pathing: every slot's path effect (order-1 paths, spatialized to stereo with the HRTF or panning).
            IPLPathEffectSettings path{};
            path.maxOrder = SpatialAudio::ReflectionOrder;
            path.spatialize = IPL_TRUE;
            path.speakerLayout.type = IPL_SPEAKERLAYOUTTYPE_STEREO;
            path.hrtf = SpatialAudio::Hrtf();
            for (IPLPathEffect& effect : state.PathEffects)
                if (created && iplPathEffectCreate(SpatialAudio::Context(), &audio, &path, &effect) != IPL_STATUS_SUCCESS)
                    effect = nullptr;
            state.ReflectionOwner.fill(-1);
            state.ReflectionVoice.fill(-1);
            state.PathingVoice.fill(-1);
            state.SteamAudio = created;
            if (!created)
                NOX_CORE_ERROR("Steam Audio: the voices' effects could not be created -- 3D sounds are panned without it");
        }
        if (ma_pcm_rb_init(ma_format_f32, 2, kOutputRingFrames, nullptr, nullptr, &state.Output) != MA_SUCCESS ||
            ma_event_init(&state.MixerWake) != MA_SUCCESS)
        {
            ma_device_uninit(&state.Device);
            NOX_CORE_ERROR("Audio: could not create the mixer's buffers -- sound is off");
            return;
        }
        state.DeviceOpen = true;
        state.Running.store(true, std::memory_order_release);
        state.MixerThread = std::thread(MixerMain, std::ref(state));
        state.StreamThread = std::thread(StreamMain, std::ref(state));
        // The device starts once the mixer has its lead in the ring and waits for the callbacks: a device's first callbacks can
        // come back to back, before a thread just created has run.
        for (int waited = 0; !state.MixerReady.load(std::memory_order_acquire) && waited < 500; ++waited)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (ma_device_start(&state.Device) != MA_SUCCESS)
            NOX_CORE_ERROR("Audio: the output device did not start");
        NOX_CORE_INFO("Audio: {} at {} Hz, {}-frame periods, {}-frame blocks ({:.1f} ms)", state.DeviceName, state.SampleRate,
                      state.PeriodFrames, kBlock, 1000.0 * kBlock / state.SampleRate);
    }

    void AudioEngine::Shutdown()
    {
        if (!s_State)
            return;
        State& state = *s_State;
        if (state.DeviceOpen)
        {
            ma_device_stop(&state.Device);
            state.Running.store(false, std::memory_order_release);
            ma_event_signal(&state.MixerWake);
            state.StreamWake.notify_all();
            state.MixerThread.join();
            state.StreamThread.join();
            ma_device_uninit(&state.Device);
            ma_event_uninit(&state.MixerWake);
            ma_pcm_rb_uninit(&state.Output);
            // The mixer has stopped: the layout it ran, and every one it handed back, are the game thread's to free.
            FreeRetiredLayouts(state);
            Command command;
            while (state.Commands.Pop(command))
                if (command.Type == CommandType::SetLayout && command.Layout != state.Layout)
                    delete command.Layout;
            delete state.Layout;
            for (uint32_t slot = 0; slot < MaxVoices; ++slot)
            {
                if (state.DirectEffects[slot]) iplDirectEffectRelease(&state.DirectEffects[slot]);
                if (state.BinauralEffects[slot]) iplBinauralEffectRelease(&state.BinauralEffects[slot]);
                if (state.PanningEffects[slot]) iplPanningEffectRelease(&state.PanningEffects[slot]);
                if (state.PathEffects[slot]) iplPathEffectRelease(&state.PathEffects[slot]);
                SpatialAudio::ReleaseSource(state.GameVoices[slot].Source);
            }
            state.Effects.clear(); // (the Acoustic Reverbs' Steam Audio effects go before its context)
            for (IPLReflectionEffect& effect : state.ReflectionEffects)
                if (effect)
                    iplReflectionEffectRelease(&effect);
            if (state.ReflectionDecode)
                iplAmbisonicsDecodeEffectRelease(&state.ReflectionDecode);
            if (state.ReflectionMixer)
                iplReflectionMixerRelease(&state.ReflectionMixer);
            SpatialAudio::Shutdown();
        }
        delete s_State;
        s_State = nullptr;
    }

    bool AudioEngine::IsAvailable()
    {
        return s_State && s_State->DeviceOpen;
    }

    AudioVoiceHandle AudioEngine::Play(const AudioPlayDesc& desc)
    {
        if (!IsAvailable())
            return 0;
        if (desc.Graph)
            return PlayGraph(desc);
        if (!desc.Clip || desc.Clip->GetChannels() == 0)
            return 0;
        State& state = *s_State;
        if (state.FreeSlots.empty())
        {
            const double now = static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) * 1e-9;
            if (now - state.LastVoiceLimitWarning > 2.0)
            {
                NOX_CORE_WARN("Audio: all {} voices are playing; a new sound was not started", MaxVoices);
                state.LastVoiceLimitWarning = now;
            }
            return 0;
        }

        const uint32_t slot = state.FreeSlots.back();
        GameVoice& voice = state.GameVoices[slot];
        const Ref<AudioClip>& clip = desc.Clip;
        const uint64_t startFrame = static_cast<uint64_t>(std::max(desc.StartTime, 0.0f) * static_cast<float>(clip->GetSampleRate()));

        std::shared_ptr<StreamState> stream;
        if (!clip->IsDecoded())
        {
            stream = std::make_shared<StreamState>();
            stream->Clip = clip;
            stream->Channels = clip->GetChannels();
            stream->Loop.store(desc.Loop, std::memory_order_relaxed);
            const uint32_t ringFrames = static_cast<uint32_t>(kStreamBufferSeconds * static_cast<float>(clip->GetSampleRate()));
            if (!stream->Decoder.Open(clip) || (startFrame > 0 && !stream->Decoder.Seek(startFrame)) ||
                ma_pcm_rb_init(ma_format_f32, stream->Channels, std::max(ringFrames, kStagingFrames), nullptr, nullptr, &stream->Ring) != MA_SUCCESS)
            {
                NOX_CORE_ERROR("Audio: could not open {} to stream it", clip->GetPath().generic_string());
                return 0;
            }
            stream->RingReady = true;
        }
        else if (startFrame >= clip->GetFrameCount())
            return 0;

        state.FreeSlots.pop_back();
        const uint32_t generation = voice.Generation + 1; // a handle of the slot's previous voice no longer finds it
        voice = GameVoice{};
        voice.Alive = true;
        voice.Generation = generation;
        voice.Desc = desc;
        voice.Stream = stream;
        voice.Environment = desc.Environment;
        TrackVelocity(desc.Position, voice.PreviousPosition, voice.HasPrevious, voice.Velocity, 0.0f);

        const MixParams params = ComputeMix(state, voice);
        Command command;
        command.Type = CommandType::Start;
        command.Slot = slot;
        command.Clip = clip.get();
        command.Stream = stream.get();
        command.StartFrame = startFrame;
        command.Bus = static_cast<uint16_t>(std::max(BusOf(state, desc.Bus), 0));
        command.Group = static_cast<uint8_t>(desc.Group);
        command.Downmix = desc.Spatial && clip->GetChannels() == 2;
        command.GainL = params.GainL;
        command.GainR = params.GainR;
        command.Rate = params.Rate;
        FillSpatial(command, params, voice);
        command.WaitForSimulation = WillSimulate(state, desc);
        command.Value = std::max(desc.FadeIn, 0.0f);
        command.Flag = desc.Loop;
        command.LowPass = desc.Environment.LowPassHz;
        command.SendBus = static_cast<int16_t>(desc.Environment.SendBus ? BusOf(state, desc.Environment.SendBus) : -1);
        command.SendGain = command.SendBus >= 0 ? std::max(desc.Environment.SendLevel, 0.0f) : 0.0f;
        if (!Push(state, command))
        {
            voice.Alive = false;
            voice.Desc = {};
            voice.Stream.reset();
            state.FreeSlots.push_back(slot);
            return 0;
        }
        if (stream)
        {
            {
                std::scoped_lock lock(state.StreamMutex);
                state.Streams.push_back(stream);
            }
            state.StreamWake.notify_one();
        }
        return (static_cast<uint64_t>(voice.Generation) << 32) | slot;
    }

    namespace
    {
        uint32_t TakeSlot(State& state)
        {
            if (!state.FreeSlots.empty())
                return state.FreeSlots.back();
            const double now = static_cast<double>(std::chrono::steady_clock::now().time_since_epoch().count()) * 1e-9;
            if (now - state.LastVoiceLimitWarning > 2.0)
            {
                NOX_CORE_WARN("Audio: all {} voices are playing; a new sound was not started", AudioEngine::MaxVoices);
                state.LastVoiceLimitWarning = now;
            }
            return UINT32_MAX;
        }
    }

    AudioVoiceHandle AudioEngine::PlayGraph(const AudioPlayDesc& desc)
    {
        State& state = *s_State;
        const uint32_t slot = TakeSlot(state);
        if (slot == UINT32_MAX)
            return 0;
        GameVoice& voice = state.GameVoices[slot];
        // Created here (it allocates): its pin slots, node states and Wave Players' streams.
        auto graph = std::make_unique<AudioGraphInstance>(desc.Graph, static_cast<float>(state.SampleRate), state.NextGraphSeed++, &desc.GraphParameters);

        state.FreeSlots.pop_back();
        const uint32_t generation = voice.Generation + 1;
        voice = GameVoice{};
        voice.Alive = true;
        voice.Generation = generation;
        voice.Desc = desc;
        voice.Desc.GraphParameters.clear(); // (in the instance now)
        voice.Environment = desc.Environment;
        TrackVelocity(desc.Position, voice.PreviousPosition, voice.HasPrevious, voice.Velocity, 0.0f);

        const MixParams params = ComputeMix(state, voice);
        Command command;
        command.Type = CommandType::Start;
        command.Slot = slot;
        command.Graph = graph.get();
        command.Bus = static_cast<uint16_t>(std::max(BusOf(state, desc.Bus), 0));
        command.Group = static_cast<uint8_t>(desc.Group);
        command.Downmix = desc.Spatial;
        command.GainL = params.GainL;
        command.GainR = params.GainR;
        command.Rate = params.Rate;
        FillSpatial(command, params, voice);
        command.WaitForSimulation = WillSimulate(state, desc);
        command.Value = std::max(desc.FadeIn, 0.0f);
        command.LowPass = desc.Environment.LowPassHz;
        command.SendBus = static_cast<int16_t>(desc.Environment.SendBus ? BusOf(state, desc.Environment.SendBus) : -1);
        command.SendGain = command.SendBus >= 0 ? std::max(desc.Environment.SendLevel, 0.0f) : 0.0f;
        if (!Push(state, command))
        {
            voice.Alive = false;
            voice.Desc = {};
            state.FreeSlots.push_back(slot);
            return 0;
        }
        if (!graph->GetStreams().empty())
        {
            {
                std::scoped_lock lock(state.StreamMutex);
                for (const std::shared_ptr<AudioClipStream>& stream : graph->GetStreams())
                    state.ClipStreams.push_back(stream);
            }
            state.StreamWake.notify_one();
        }
        voice.Graph = std::move(graph);
        return (static_cast<uint64_t>(voice.Generation) << 32) | slot;
    }

    void AudioEngine::SetGraphParameter(AudioVoiceHandle handle, int32_t index, float value)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || !voice->Graph || index < 0)
            return;
        Command command;
        command.Type = CommandType::SetParameter;
        command.Slot = SlotOf(handle);
        command.Parameter = index;
        command.Value = value;
        Push(*s_State, command);
    }

    void AudioEngine::FireGraphParameter(AudioVoiceHandle handle, int32_t index)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || !voice->Graph || index < 0)
            return;
        Command command;
        command.Type = CommandType::FireParameter;
        command.Slot = SlotOf(handle);
        command.Parameter = index;
        Push(*s_State, command);
    }

    void AudioEngine::SetGraphConstant(AudioVoiceHandle handle, uint32_t slot, float value)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || !voice->Graph)
            return;
        Command command;
        command.Type = CommandType::SetConstant;
        command.Slot = SlotOf(handle);
        command.Parameter = static_cast<int32_t>(slot);
        command.Value = value;
        Push(*s_State, command);
    }

    glm::vec2 AudioEngine::GetVoicePeak(AudioVoiceHandle handle)
    {
        if (!Find(s_State, handle))
            return glm::vec2(0.0f);
        const uint32_t slot = SlotOf(handle);
        return { s_State->VoicePeak[slot * 2].load(std::memory_order_relaxed), s_State->VoicePeak[slot * 2 + 1].load(std::memory_order_relaxed) };
    }

    uint32_t AudioEngine::GetSampleRate()
    {
        return s_State ? s_State->SampleRate : 48000;
    }

    void AudioEngine::Stop(AudioVoiceHandle handle, float fadeOutSeconds)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || voice->StopSent)
            return;
        Command command;
        command.Type = CommandType::Stop;
        command.Slot = SlotOf(handle);
        command.Value = fadeOutSeconds;
        if (Push(*s_State, command))
            voice->StopSent = true;
    }

    bool AudioEngine::IsPlaying(AudioVoiceHandle handle)
    {
        const GameVoice* voice = Find(s_State, handle);
        return voice && !voice->StopSent;
    }

    void AudioEngine::SetVolume(AudioVoiceHandle handle, float volume)
    {
        if (GameVoice* voice = Find(s_State, handle))
            voice->Desc.Volume = volume;
    }

    void AudioEngine::SetPitch(AudioVoiceHandle handle, float pitch)
    {
        if (GameVoice* voice = Find(s_State, handle))
            voice->Desc.Pitch = pitch;
    }

    void AudioEngine::SetPosition(AudioVoiceHandle handle, const glm::dvec3& position)
    {
        if (GameVoice* voice = Find(s_State, handle))
            voice->Desc.Position = position;
    }

    void AudioEngine::SetSpatialization(AudioVoiceHandle handle, const AudioSpatialSettings& settings)
    {
        if (GameVoice* voice = Find(s_State, handle))
            voice->Desc.Spatialization = settings;
    }

    void AudioEngine::SetLoop(AudioVoiceHandle handle, bool loop)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || voice->Desc.Loop == loop)
            return;
        voice->Desc.Loop = loop;
        if (voice->Stream)
            voice->Stream->Loop.store(loop, std::memory_order_relaxed);
        Command command;
        command.Type = CommandType::SetLoop;
        command.Slot = SlotOf(handle);
        command.Flag = loop;
        Push(*s_State, command);
    }

    void AudioEngine::SetPaused(AudioVoiceHandle handle, bool paused)
    {
        GameVoice* voice = Find(s_State, handle);
        if (!voice || voice->Paused == paused)
            return;
        voice->Paused = paused;
        Command command;
        command.Type = CommandType::PauseVoice;
        command.Slot = SlotOf(handle);
        command.Flag = paused;
        Push(*s_State, command);
    }

    void AudioEngine::StopGroup(AudioVoiceGroup group, float fadeOutSeconds)
    {
        if (!IsAvailable())
            return;
        State& state = *s_State;
        Command command;
        command.Type = CommandType::StopGroup;
        command.Group = static_cast<uint8_t>(group);
        command.Value = fadeOutSeconds;
        if (!Push(state, command))
            return;
        for (GameVoice& voice : state.GameVoices)
        {
            if (voice.Alive && voice.Desc.Group == group)
                voice.StopSent = true;
        }
    }

    void AudioEngine::SetGroupPaused(AudioVoiceGroup group, bool paused)
    {
        if (!IsAvailable())
            return;
        State& state = *s_State;
        if (state.GamePaused[static_cast<size_t>(group)] == paused)
            return;
        state.GamePaused[static_cast<size_t>(group)] = paused;
        Command command;
        command.Type = CommandType::PauseGroup;
        command.Group = static_cast<uint8_t>(group);
        command.Flag = paused;
        Push(state, command);
    }

    void AudioEngine::SetListener(const glm::dvec3& position, const glm::quat& orientation)
    {
        if (!s_State)
            return;
        s_State->ListenerPosition = position;
        s_State->ListenerOrientation = glm::normalize(orientation);
        SpatialAudio::SetListener(position, s_State->ListenerOrientation);
    }

    void AudioEngine::SetReflections(const AudioReflectionSettings& settings)
    {
        if (!s_State || (s_State->Reflections == settings && s_State->ReflectionsSent))
            return;
        s_State->Reflections = settings;
        s_State->ReflectionsSent = false; // (sent with the next Update)
        SpatialAudio::ReflectionSettings simulation;
        simulation.Enabled = settings.Enabled;
        simulation.Rays = settings.Rays;
        simulation.Bounces = settings.Bounces;
        simulation.Duration = settings.Duration;
        simulation.MaxSources = settings.MaxSources;
        simulation.MaxPathingSources = settings.PathingSources;
        simulation.Reverb = s_State->HasAcousticReverb ? settings.ListenerReverb : AudioListenerReverb::Off;
        SpatialAudio::SetReflectionSettings(simulation);
    }

    float AudioEngine::GetReflectionMilliseconds()
    {
        return SpatialAudio::GetLastReflectionMilliseconds();
    }

    AudioEngine::ReflectionStats AudioEngine::GetReflectionStats()
    {
        ReflectionStats stats;
        if (!IsAvailable())
            return stats;
        stats.Milliseconds = SpatialAudio::GetLastReflectionMilliseconds();
        stats.Peak = s_State->ReflectionsMeter[0].load(std::memory_order_relaxed);
        stats.Rms = s_State->ReflectionsMeter[1].load(std::memory_order_relaxed);
        for (int32_t voice : s_State->ReflectionVoice)
            stats.Voices += voice >= 0 ? 1 : 0;
        for (int32_t voice : s_State->PathingVoice)
            stats.PathingVoices += voice >= 0 && s_State->GameVoices[voice].PathingValid ? 1 : 0;
        stats.ListenerReverb = s_State->ListenerReverbSent;
        return stats;
    }

    void AudioEngine::SetHeadphones(bool headphones)
    {
        if (s_State)
            s_State->Headphones = headphones; // (the next Update sends every 3D voice its new spatialization)
    }

    bool AudioEngine::GetHeadphones()
    {
        return !s_State || s_State->Headphones;
    }

    void AudioEngine::SetVoiceEnvironment(AudioVoiceHandle handle, const AudioVoiceEnvironment& environment)
    {
        if (GameVoice* voice = Find(s_State, handle))
            voice->Environment = environment; // (sent with the voice's next update)
    }

    void AudioEngine::SetMixerLayout(const AudioMixerLayoutDesc& desc)
    {
        if (!IsAvailable())
            return;
        State& state = *s_State;
        FreeRetiredLayouts(state);
        state.HasAcousticReverb = false;
        for (const AudioMixerLayoutDesc::Bus& bus : desc.Buses)
            for (const AudioMixerLayoutDesc::Effect& effect : bus.Effects)
                state.HasAcousticReverb |= effect.Type == AudioEffectType::AcousticReverb && !effect.Bypass;
        std::vector<PendingParameter> reused;
        MixerLayout* layout = BuildLayout(state, desc, &reused);
        Command command;
        command.Type = CommandType::SetLayout;
        command.Layout = layout;
        if (!Push(state, command))
        {
            delete layout;
            return;
        }
        for (const PendingParameter& parameter : reused)
        {
            Command set;
            set.Type = CommandType::EffectParameter;
            set.Bus = parameter.Bus;
            set.Parameter = parameter.Effect;
            set.Index = parameter.Parameter;
            set.Value = parameter.Value;
            Push(state, set);
        }
        state.GameLayout = layout;
        state.ReflectionsSent = false; // (the reflections' bus index in this layout)
        state.BusIndex.clear();
        for (uint32_t b = 0; b < layout->Buses.size(); ++b)
            state.BusIndex.emplace(layout->Buses[b].Id, b);
        // Every playing voice to its bus's (and send's) index in the new layout, applied right after it.
        for (uint32_t slot = 0; slot < MaxVoices; ++slot)
        {
            const GameVoice& voice = state.GameVoices[slot];
            if (!voice.Alive)
                continue;
            Command bus;
            bus.Type = CommandType::VoiceBus;
            bus.Slot = slot;
            bus.Bus = static_cast<uint16_t>(std::max(BusOf(state, voice.Desc.Bus), 0));
            bus.SendBus = static_cast<int16_t>(voice.Environment.SendBus ? BusOf(state, voice.Environment.SendBus) : -1);
            Push(state, bus);
        }
    }

    void AudioEngine::SetBusGain(uint32_t bus, float gain)
    {
        if (!IsAvailable())
            return;
        Command command;
        command.Type = CommandType::BusGain;
        command.Bus = static_cast<uint16_t>(bus);
        command.Value = std::max(gain, 0.0f);
        Push(*s_State, command);
    }

    void AudioEngine::SetEffectParameter(uint32_t bus, uint32_t effect, uint32_t parameter, float value)
    {
        if (!IsAvailable())
            return;
        Command command;
        command.Type = CommandType::EffectParameter;
        command.Bus = static_cast<uint16_t>(bus);
        command.Parameter = static_cast<int32_t>(effect);
        command.Index = parameter;
        command.Value = value;
        Push(*s_State, command);
    }

    void AudioEngine::SetEffectBypass(uint32_t bus, uint32_t effect, bool bypass)
    {
        if (!IsAvailable())
            return;
        Command command;
        command.Type = CommandType::EffectBypass;
        command.Bus = static_cast<uint16_t>(bus);
        command.Parameter = static_cast<int32_t>(effect);
        command.Flag = bypass;
        Push(*s_State, command);
    }

    void AudioEngine::SetSendGain(uint32_t bus, uint32_t send, float gain)
    {
        if (!IsAvailable())
            return;
        Command command;
        command.Type = CommandType::SendGain;
        command.Bus = static_cast<uint16_t>(bus);
        command.Parameter = static_cast<int32_t>(send);
        command.Value = std::max(gain, 0.0f);
        Push(*s_State, command);
    }

    AudioBusMeter AudioEngine::GetBusMeter(uint32_t bus)
    {
        AudioBusMeter meter;
        if (!IsAvailable() || bus >= MaxBuses)
            return meter;
        const std::atomic<float>* values = &s_State->BusMeters[bus * 5];
        meter.PeakLeft = values[0].load(std::memory_order_relaxed);
        meter.PeakRight = values[1].load(std::memory_order_relaxed);
        meter.RmsLeft = values[2].load(std::memory_order_relaxed);
        meter.RmsRight = values[3].load(std::memory_order_relaxed);
        meter.GainReductionDb = values[4].load(std::memory_order_relaxed);
        return meter;
    }

    float AudioEngine::GetEffectGainReduction(uint32_t bus, uint32_t effect)
    {
        if (!IsAvailable() || !s_State->GameLayout || bus >= s_State->GameLayout->Buses.size())
            return 0.0f;
        const RuntimeBus& runtime = s_State->GameLayout->Buses[bus];
        return effect < runtime.Effects.size() ? runtime.Effects[effect].Instance->GetGainReductionDb() : 0.0f; // (an atomic)
    }

    uint32_t AudioEngine::GetBusCount()
    {
        return IsAvailable() && s_State->GameLayout ? static_cast<uint32_t>(s_State->GameLayout->Buses.size()) : 0;
    }

    void AudioEngine::Update(float deltaSeconds)
    {
        if (!IsAvailable())
            return;
        NOX_PROFILE_SCOPE("Audio Update");
        State& state = *s_State;

        FinishedEvent finished;
        while (state.Finished.Pop(finished))
        {
            if (state.GameVoices[finished.Slot].Alive)
                ReleaseVoice(state, finished.Slot);
        }
        FreeRetiredLayouts(state);

        TrackVelocity(state.ListenerPosition, state.ListenerPrevious, state.ListenerHasPrevious, state.ListenerVelocity, deltaSeconds);

        // Steam Audio's simulation: every audible 3D voice that wants occlusion or air absorption traced against the acoustic
        // scene (here, on the game thread, as its docs advise for occlusion: no lag between a source moving and its sound).
        if (state.SteamAudio)
        {
            NOX_PROFILE_SCOPE("Audio Occlusion");
            const float metersPerUnit = WorldUnits::ToMeters(1.0f);
            bool any = false;
            for (GameVoice& voice : state.GameVoices)
            {
                const AudioSpatialSettings& settings = voice.Desc.Spatialization;
                const bool wanted = voice.Alive && !voice.StopSent && voice.Desc.Spatial && (settings.Occlusion || settings.AirAbsorption) &&
                                    glm::length(voice.Desc.Position - state.ListenerPosition) * metersPerUnit <= settings.MaxDistance;
                if (!wanted)
                {
                    if (voice.Source)
                    {
                        SpatialAudio::ReleaseSource(voice.Source);
                        voice.Direct = {};
                    }
                    continue;
                }
                if (!voice.Source)
                    voice.Source = SpatialAudio::CreateSource();
                SpatialAudio::SourceInputs inputs;
                inputs.Position = voice.Desc.Position;
                inputs.Occlusion = settings.Occlusion;
                inputs.AirAbsorption = settings.AirAbsorption;
                inputs.Radius = settings.SourceRadius;
                SpatialAudio::SetSourceInputs(voice.Source, inputs);
                any = true;
            }
            if (any)
            {
                SpatialAudio::RunDirect();
                for (GameVoice& voice : state.GameVoices)
                    if (voice.Source)
                    {
                        voice.Direct = SpatialAudio::GetSourceOutputs(voice.Source);
                        voice.Simulated = true;
                    }
            }
            UpdateReflections(state, metersPerUnit);
        }

        for (uint32_t slot = 0; slot < MaxVoices; ++slot)
        {
            GameVoice& voice = state.GameVoices[slot];
            if (!voice.Alive || voice.StopSent)
                continue;
            if (voice.Desc.Spatial)
                TrackVelocity(voice.Desc.Position, voice.PreviousPosition, voice.HasPrevious, voice.Velocity, deltaSeconds);
            const MixParams params = ComputeMix(state, voice);
            Command command;
            command.Type = CommandType::Update;
            command.Slot = slot;
            command.GainL = params.GainL;
            command.GainR = params.GainR;
            command.Rate = params.Rate;
            FillSpatial(command, params, voice);
            command.Simulated = voice.Simulated || !voice.Source;
            command.LowPass = voice.Environment.LowPassHz;
            command.SendBus = static_cast<int16_t>(voice.Environment.SendBus ? BusOf(state, voice.Environment.SendBus) : -1);
            command.SendGain = command.SendBus >= 0 ? std::max(voice.Environment.SendLevel, 0.0f) : 0.0f;
            Push(state, command);
        }
    }

    AudioStats AudioEngine::GetStats()
    {
        AudioStats stats;
        if (!IsAvailable())
            return stats;
        State& state = *s_State;
        stats.DeviceOpen = true;
        stats.DeviceName = state.DeviceName;
        stats.SampleRate = state.SampleRate;
        stats.PeriodFrames = state.PeriodFrames;
        for (const GameVoice& voice : state.GameVoices)
        {
            if (!voice.Alive)
                continue;
            ++stats.Voices;
            if (voice.Stream || (voice.Graph && !voice.Graph->GetStreams().empty()))
                ++stats.StreamingVoices;
        }
        stats.Underruns = state.Underruns.load(std::memory_order_relaxed);
        stats.StreamStarvations = state.Starvations.load(std::memory_order_relaxed);
        stats.MixBlockMilliseconds = state.MixAverage.load(std::memory_order_relaxed);
        stats.MixBlockMaxMilliseconds = state.MixMax.exchange(0.0f, std::memory_order_relaxed);
        stats.BlockMilliseconds = 1000.0f * static_cast<float>(kBlock) / static_cast<float>(state.SampleRate);
        return stats;
    }
}
