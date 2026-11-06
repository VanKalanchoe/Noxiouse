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
#include <vector>

#include <miniaudio.h>

#include "AudioGraph.h"
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
        constexpr uint32_t kBusCount = static_cast<uint32_t>(AudioBus::Count);
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
            SetConstant
        };

        struct Command
        {
            CommandType Type = CommandType::Update;
            uint32_t Slot = 0;
            // Start
            const AudioClip* Clip = nullptr;
            StreamState* Stream = nullptr;
            AudioGraphInstance* Graph = nullptr;
            int32_t Parameter = -1; // SetParameter / FireParameter
            uint64_t StartFrame = 0;
            uint8_t Bus = 0;
            uint8_t Group = 0;
            bool Downmix = false; // a 3D stereo clip is heard as one point: its channels summed before panning
            // Start / Update
            float GainL = 0.0f;
            float GainR = 0.0f;
            float Rate = 1.0f;
            // Start (fade in), Stop / StopGroup (fade out), BusGain (gain)
            float Value = 0.0f;
            bool Flag = false; // Loop, paused
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
            const AudioClip* Clip = nullptr;
            StreamState* Stream = nullptr;
            AudioGraphInstance* Graph = nullptr;
            uint32_t Channels = 1;
            bool Loop = false;
            bool Downmix = false;
            uint8_t Bus = 0;
            uint8_t Group = 0;
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
        };

        struct MixParams
        {
            float GainL = 0.0f;
            float GainR = 0.0f;
            float Rate = 1.0f;
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

            // Mixer side.
            std::array<MixVoice, AudioEngine::MaxVoices> MixVoices;
            std::array<float, kBusCount> BusGain{};
            std::array<float, kBusCount> BusTarget{};
            std::array<bool, kGroupCount> GroupPaused{};
            std::array<std::array<float, kBlock * 2>, kBusCount> BusBuffers{};
            std::array<float, kBlock * 2> Block{};

            // Game side.
            std::array<GameVoice, AudioEngine::MaxVoices> GameVoices;
            std::vector<uint32_t> FreeSlots;
            std::array<float, kBusCount> BusVolume{};
            std::array<bool, kBusCount> BusMuted{};
            std::array<bool, kGroupCount> GamePaused{};
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
            std::array<std::atomic<float>, kBusCount> BusPeak{};
            std::array<std::atomic<float>, AudioEngine::MaxVoices * 2> VoicePeak{};
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
                state.BusTarget[command.Bus] = command.Value;
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

        void MixBlock(State& state)
        {
            NOX_PROFILE_SCOPE("Audio Mix Block");
            Command command;
            while (state.Commands.Pop(command))
                ApplyCommand(state, command);

            for (auto& bus : state.BusBuffers)
                bus.fill(0.0f);

            for (uint32_t slot = 0; slot < AudioEngine::MaxVoices; ++slot)
            {
                MixVoice& voice = state.MixVoices[slot];
                if (!voice.Active)
                    continue;
                const bool groupPaused = state.GroupPaused[voice.Group] && voice.Bus != static_cast<uint8_t>(AudioBus::UI);
                if (voice.Paused || groupPaused)
                    continue;
                const bool running = voice.Graph ? MixGraphBlock(state, voice, state.BusBuffers[voice.Bus].data(), slot)
                                                 : MixVoiceBlock(state, voice, state.BusBuffers[voice.Bus].data());
                if (!running)
                {
                    voice.Active = false;
                    voice.Clip = nullptr;
                    voice.Stream = nullptr;
                    voice.Graph = nullptr;
                    state.Finished.Push({ slot }); // (room for every slot: a slot is reused only after its event is read)
                }
            }

            // Buses into Master, each with its gain ramped across the block.
            std::array<float, kBlock * 2>& master = state.BusBuffers[0];
            auto applyGain = [&](uint32_t bus, std::array<float, kBlock * 2>& buffer, std::array<float, kBlock * 2>* into)
            {
                const float from = state.BusGain[bus];
                const float to = state.BusTarget[bus];
                const float step = (to - from) / static_cast<float>(kBlock);
                float peak = 0.0f;
                for (uint32_t i = 0; i < kBlock; ++i)
                {
                    const float gain = from + step * static_cast<float>(i + 1);
                    const float left = buffer[i * 2] * gain;
                    const float right = buffer[i * 2 + 1] * gain;
                    peak = std::max(peak, std::max(std::fabs(left), std::fabs(right)));
                    if (into)
                    {
                        (*into)[i * 2] += left;
                        (*into)[i * 2 + 1] += right;
                    }
                    else
                    {
                        buffer[i * 2] = left;
                        buffer[i * 2 + 1] = right;
                    }
                }
                state.BusGain[bus] = to;
                // Peak meter, falling ~20 dB a second.
                const float held = state.BusPeak[bus].load(std::memory_order_relaxed) * 0.985f;
                state.BusPeak[bus].store(std::max(held, peak), std::memory_order_relaxed);
            };
            for (uint32_t bus = 1; bus < kBusCount; ++bus)
                applyGain(bus, state.BusBuffers[bus], &master);
            applyGain(0, master, nullptr);

            // The device takes floats in [-1, 1]. (Master's limiter is phase A3's bus effects.)
            for (uint32_t i = 0; i < kBlock * 2; ++i)
                state.Block[i] = std::clamp(master[i], -1.0f, 1.0f);
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
            const float minimum = std::max(settings.MinDistance, 0.001f);
            const float maximum = std::max(settings.MaxDistance, minimum + 0.001f);
            if (distance <= minimum)
                return 1.0f;
            if (distance >= maximum)
                return 0.0f;
            switch (settings.Attenuation)
            {
            case AudioAttenuation::Inverse:
            {
                const float gain = minimum / distance;
                const float atMax = minimum / maximum;
                return (gain - atMax) / (1.0f - atMax);
            }
            case AudioAttenuation::InverseSquare:
            {
                const float gain = (minimum / distance) * (minimum / distance);
                const float atMax = (minimum / maximum) * (minimum / maximum);
                return (gain - atMax) / (1.0f - atMax);
            }
            case AudioAttenuation::Linear:
                return 1.0f - (distance - minimum) / (maximum - minimum);
            case AudioAttenuation::None:
                return 1.0f;
            }
            return 1.0f;
        }

        MixParams ComputeMix(const State& state, const GameVoice& voice)
        {
            const AudioPlayDesc& desc = voice.Desc;
            MixParams params;
            const float volume = std::max(desc.Volume, 0.0f);
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

    void AudioEngine::Init()
    {
        if (s_State)
            return;
        s_State = new State();
        State& state = *s_State;
        state.BusVolume.fill(1.0f);
        state.BusGain.fill(1.0f);
        state.BusTarget.fill(1.0f);
        for (auto& peak : state.BusPeak)
            peak.store(0.0f);
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
        TrackVelocity(desc.Position, voice.PreviousPosition, voice.HasPrevious, voice.Velocity, 0.0f);

        const MixParams params = ComputeMix(state, voice);
        Command command;
        command.Type = CommandType::Start;
        command.Slot = slot;
        command.Clip = clip.get();
        command.Stream = stream.get();
        command.StartFrame = startFrame;
        command.Bus = static_cast<uint8_t>(desc.Bus);
        command.Group = static_cast<uint8_t>(desc.Group);
        command.Downmix = desc.Spatial && clip->GetChannels() == 2;
        command.GainL = params.GainL;
        command.GainR = params.GainR;
        command.Rate = params.Rate;
        command.Value = std::max(desc.FadeIn, 0.0f);
        command.Flag = desc.Loop;
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
        TrackVelocity(desc.Position, voice.PreviousPosition, voice.HasPrevious, voice.Velocity, 0.0f);

        const MixParams params = ComputeMix(state, voice);
        Command command;
        command.Type = CommandType::Start;
        command.Slot = slot;
        command.Graph = graph.get();
        command.Bus = static_cast<uint8_t>(desc.Bus);
        command.Group = static_cast<uint8_t>(desc.Group);
        command.Downmix = desc.Spatial;
        command.GainL = params.GainL;
        command.GainR = params.GainR;
        command.Rate = params.Rate;
        command.Value = std::max(desc.FadeIn, 0.0f);
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
    }

    void AudioEngine::SetBusVolume(AudioBus bus, float volume)
    {
        if (!s_State || bus >= AudioBus::Count)
            return;
        State& state = *s_State;
        state.BusVolume[static_cast<size_t>(bus)] = std::max(volume, 0.0f);
        Command command;
        command.Type = CommandType::BusGain;
        command.Bus = static_cast<uint8_t>(bus);
        command.Value = state.BusMuted[static_cast<size_t>(bus)] ? 0.0f : state.BusVolume[static_cast<size_t>(bus)];
        if (state.DeviceOpen)
            Push(state, command);
    }

    float AudioEngine::GetBusVolume(AudioBus bus)
    {
        return s_State && bus < AudioBus::Count ? s_State->BusVolume[static_cast<size_t>(bus)] : 1.0f;
    }

    void AudioEngine::SetBusMuted(AudioBus bus, bool muted)
    {
        if (!s_State || bus >= AudioBus::Count)
            return;
        s_State->BusMuted[static_cast<size_t>(bus)] = muted;
        SetBusVolume(bus, s_State->BusVolume[static_cast<size_t>(bus)]);
    }

    bool AudioEngine::IsBusMuted(AudioBus bus)
    {
        return s_State && bus < AudioBus::Count && s_State->BusMuted[static_cast<size_t>(bus)];
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

        TrackVelocity(state.ListenerPosition, state.ListenerPrevious, state.ListenerHasPrevious, state.ListenerVelocity, deltaSeconds);

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
        for (uint32_t bus = 0; bus < kBusCount; ++bus)
            stats.BusPeak[bus] = state.BusPeak[bus].load(std::memory_order_relaxed);
        return stats;
    }
}
