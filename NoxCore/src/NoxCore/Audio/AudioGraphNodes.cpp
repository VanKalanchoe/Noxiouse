// The audio graph's node types (docs/Audio_Architecture_Plan_2026.md §5.2, phase A2): sources, triggers and logic, values and math,
// envelopes and modulation, mixing and routing, filters. Each is a NodeTypeDesc (pins, literals, editor hints) and an AudioNodeImpl
// (a state struct, Init on the game thread when a voice starts, Process on the mixer thread every 256-frame block).
//
// Conventions: control inputs that shape audio (gains, frequencies, pans) are ramped across the block from their last value, so a
// jump in a parameter never clicks; trigger handling is sample-accurate (a Wave Player starts at the frame its Play fired).

#include "AudioGraph.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "NoxCore/NodeGraph/NodeType.h"

namespace Nox
{
    namespace
    {
        constexpr uint32_t N = kAudioGraphBlockFrames;
        constexpr float kPi = 3.14159265358979323846f;
        constexpr float kTwoPi = 2.0f * kPi;
        constexpr uint32_t kMaxGroupInputs = 16; // a Mixer's / Crossfade's inputs whose gains are ramped

        // ---- Describing a node type -----------------------------------------------------------------------------------------

        struct PinSpec
        {
            std::string Name;
            std::string Type;
            NodeGraphValue Literal = 0.0f;
            bool HasLiteral = false;
            float Min = 0.0f, Max = 0.0f; // a slider when Max > Min
        };
        PinSpec A(const char* name) { return { name, "Audio" }; }
        PinSpec T(const char* name) { return { name, "Trigger" }; }
        PinSpec F(const char* name, float value, float min = 0.0f, float max = 0.0f) { return { name, "Float", value, true, min, max }; }
        PinSpec I(const char* name, int32_t value) { return { name, "Int", value, true }; }
        PinSpec B(const char* name, bool value) { return { name, "Bool", value, true }; }

        NodeTypeDesc Desc(const char* name, const char* category, std::vector<PinSpec> inputs, std::vector<NodePinDesc> outputs)
        {
            NodeTypeDesc desc;
            desc.TypeName = name;
            desc.Domain = kAudioGraphDomain;
            desc.Category = category;
            for (const PinSpec& pin : inputs)
            {
                desc.Inputs.push_back({ pin.Name, pin.Type });
                if (pin.HasLiteral)
                    desc.DefaultProperties.emplace_back(pin.Name, pin.Literal);
                if (pin.Max > pin.Min)
                    desc.PropertyRanges[pin.Name] = { pin.Min, pin.Max };
            }
            desc.Outputs = std::move(outputs);
            return desc;
        }

        void Option(NodeTypeDesc& desc, const char* name, std::vector<std::string> options, int32_t value = 0)
        {
            desc.DefaultProperties.emplace_back(name, value);
            desc.PropertyEnums[name] = std::move(options);
        }

        void Property(NodeTypeDesc& desc, const char* name, NodeGraphValue value)
        {
            desc.DefaultProperties.emplace_back(name, value);
        }

        void ClipProperty(NodeTypeDesc& desc, const std::string& name)
        {
            desc.DefaultProperties.emplace_back(name, uint64_t(0));
            desc.PropertyAssetTypeHints[name] = "AssetType::AudioClip";
        }

        // Registers a node: Node::State, Node::Init(State&, AudioNodeSetup&), Node::Process(AudioNodeRun&, State&).
        template <typename Node>
        void Register(NodeTypeDesc desc)
        {
            using State = typename Node::State;
            AudioNodeImpl impl;
            impl.StateSize = sizeof(State);
            impl.StateAlign = alignof(State);
            impl.Construct = [](void* memory) { new (memory) State(); };
            impl.Destroy = [](void* memory) { static_cast<State*>(memory)->~State(); };
            impl.Init = [](void* memory, AudioNodeSetup& setup) { Node::Init(*static_cast<State*>(memory), setup); };
            impl.Process = [](AudioNodeRun& run) { Node::Process(run, *static_cast<State*>(run.State)); };
            RegisterAudioNodeImpl(desc.TypeName, impl);
            NodeTypeRegistry::Register(std::move(desc));
        }

        struct NoState {};
        struct NoInit
        {
            template <typename S>
            static void Init(S&, AudioNodeSetup&) {}
        };

        // ---- DSP helpers ----------------------------------------------------------------------------------------------------

        // xorshift32: deterministic, cheap, good enough for audio noise and choices.
        struct Random
        {
            uint32_t Value = 0x9E3779B9u;
            void Seed(uint32_t seed) { Value = seed ? seed : 0x9E3779B9u; }
            uint32_t Next()
            {
                Value ^= Value << 13;
                Value ^= Value >> 17;
                Value ^= Value << 5;
                return Value;
            }
            float Unit() { return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f); } // [0, 1)
            float Bipolar() { return Unit() * 2.0f - 1.0f; }                               // [-1, 1)
        };

        uint32_t SeedOf(AudioNodeSetup& setup)
        {
            const int32_t seed = setup.Int("Seed", -1);
            return seed >= 0 ? static_cast<uint32_t>(seed) * 2654435761u + 12345u : setup.VoiceSeed();
        }

        // A control value applied to audio, ramped from its last block's value.
        struct Ramp
        {
            float Previous = 0.0f;
            bool Started = false;
            // Per-frame step from the last value to `target`; `outStart` the first frame's value.
            float Begin(float target, float& outStart)
            {
                if (!Started)
                {
                    Previous = target;
                    Started = true;
                }
                outStart = Previous;
                const float step = (target - Previous) / static_cast<float>(N);
                Previous = target;
                return step;
            }
        };

        float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
        float LinearToDb(float linear) { return 20.0f * std::log10(std::max(linear, 1e-10f)); }
        float SemitonesToRatio(float semitones) { return std::exp2(semitones / 12.0f); }

        // The two triggers' events in order (Stop before Play at the same frame, so a Stop + Play retriggers).
        struct Event
        {
            uint32_t Frame;
            uint8_t Kind; // 0 = first trigger list, 1 = second
        };
        uint32_t MergeEvents(const AudioTriggers& first, const AudioTriggers& second, Event* out)
        {
            uint32_t count = 0;
            for (uint32_t i = 0; i < first.Count; ++i)
                out[count++] = { first.Offsets[i], 0 };
            for (uint32_t i = 0; i < second.Count; ++i)
                out[count++] = { second.Offsets[i], 1 };
            std::sort(out, out + count, [](const Event& a, const Event& b) { return a.Frame != b.Frame ? a.Frame < b.Frame : a.Kind > b.Kind; });
            return count;
        }

        // RBJ cookbook biquad (Robert Bristow-Johnson's Audio EQ Cookbook), transposed direct form II.
        enum class BiquadType : int32_t { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };
        struct Biquad
        {
            float B0 = 1, B1 = 0, B2 = 0, A1 = 0, A2 = 0;
            float Z1 = 0, Z2 = 0;

            void Set(BiquadType type, float frequency, float q, float gainDb, float sampleRate)
            {
                frequency = std::clamp(frequency, 10.0f, sampleRate * 0.49f);
                q = std::max(q, 0.025f);
                const float w0 = kTwoPi * frequency / sampleRate;
                const float cosw = std::cos(w0), sinw = std::sin(w0);
                const float alpha = sinw / (2.0f * q);
                const float a = std::pow(10.0f, gainDb / 40.0f);
                float b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
                switch (type)
                {
                case BiquadType::LowPass:  b0 = (1 - cosw) / 2; b1 = 1 - cosw; b2 = b0; a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha; break;
                case BiquadType::HighPass: b0 = (1 + cosw) / 2; b1 = -(1 + cosw); b2 = b0; a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha; break;
                case BiquadType::BandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha; break; // 0 dB peak
                case BiquadType::Notch:    b0 = 1; b1 = -2 * cosw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cosw; a2 = 1 - alpha; break;
                case BiquadType::Peak:     b0 = 1 + alpha * a; b1 = -2 * cosw; b2 = 1 - alpha * a; a0 = 1 + alpha / a; a1 = -2 * cosw; a2 = 1 - alpha / a; break;
                case BiquadType::LowShelf:
                {
                    const float s = 2 * std::sqrt(a) * alpha;
                    b0 = a * ((a + 1) - (a - 1) * cosw + s); b1 = 2 * a * ((a - 1) - (a + 1) * cosw); b2 = a * ((a + 1) - (a - 1) * cosw - s);
                    a0 = (a + 1) + (a - 1) * cosw + s; a1 = -2 * ((a - 1) + (a + 1) * cosw); a2 = (a + 1) + (a - 1) * cosw - s;
                    break;
                }
                case BiquadType::HighShelf:
                {
                    const float s = 2 * std::sqrt(a) * alpha;
                    b0 = a * ((a + 1) + (a - 1) * cosw + s); b1 = -2 * a * ((a - 1) + (a + 1) * cosw); b2 = a * ((a + 1) + (a - 1) * cosw - s);
                    a0 = (a + 1) - (a - 1) * cosw + s; a1 = 2 * ((a - 1) - (a + 1) * cosw); a2 = (a + 1) - (a - 1) * cosw - s;
                    break;
                }
                }
                B0 = b0 / a0; B1 = b1 / a0; B2 = b2 / a0; A1 = a1 / a0; A2 = a2 / a0;
            }
            float Process(float x)
            {
                const float y = B0 * x + Z1;
                Z1 = B1 * x - A1 * y + Z2;
                Z2 = B2 * x - A2 * y;
                return y;
            }
        };
        constexpr uint32_t kCoefficientStep = 32; // filters recompute their coefficients this often, from smoothly moving parameters

        // PolyBLEP / PolyBLAMP residuals (Valimaki et al.): the band-limiting corrections for a step / a corner at phase 0.
        float PolyBlep(float t, float dt)
        {
            if (t < dt) { t /= dt; return t + t - t * t - 1.0f; }
            if (t > 1.0f - dt) { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
            return 0.0f;
        }
        float PolyBlamp(float t, float dt)
        {
            if (t < dt) { t = t / dt - 1.0f; return -(1.0f / 3.0f) * t * t * t; }
            if (t > 1.0f - dt) { t = (t - 1.0f) / dt + 1.0f; return (1.0f / 3.0f) * t * t * t; }
            return 0.0f;
        }

        // ---- Clip playback (Wave Player, Random Wave Player, Sequence Player) -----------------------------------------------

        constexpr uint32_t kStagingFrames = 4096;
        constexpr float kMaxPlaybackRate = 8.0f;

        // Plays one clip at a rate, with an optional loop region. A decoded clip is read directly. A Compressed In Memory / Streaming
        // clip plays its first second from the clip's prime and the rest from a stream (AudioClipStream) asked for when playback starts;
        // a looping one keeps a second stream ready at the loop's start, so the wrap is seamless.
        struct ClipPlayer
        {
            const AudioClip* Clip = nullptr;
            bool Playing = false;
            int64_t Frame = 0;
            double Fraction = 0.0;
            bool Loop = false;
            int64_t LoopStart = 0;
            int64_t LoopEnd = 0;
            AudioClipStream* Streams[2] = { nullptr, nullptr };
            bool Touched[2] = { false, false };
            int Current = 0;
            int64_t StagingStart = 0;
            uint32_t StagingCount = 0;
            bool StarvedThisBlock = false;
            std::array<float, kStagingFrames * 2> Staging{};

            bool Streamed() const { return Clip && !Clip->IsDecoded(); }
            int64_t ClipFrames() const { return Clip ? static_cast<int64_t>(Clip->GetFrameCount()) : 0; }
            int64_t PrimeFrames() const { return Streamed() ? static_cast<int64_t>(Clip->GetPrimeFrames()) : 0; }

            void Ask(int index, int64_t frame)
            {
                AudioClipStream* stream = Streams[index];
                if (!stream)
                    return;
                // Already asked for and not read from: it is getting (or has) exactly that.
                if (!Touched[index] && stream->GetRequestedClip() == Clip && stream->GetRequestedFrame() == static_cast<uint64_t>(frame))
                    return;
                stream->Request(Clip, static_cast<uint64_t>(frame));
                Touched[index] = false;
            }

            void PrepareLoop()
            {
                if (Loop && Streamed() && Streams[1])
                    Ask(1 - Current, std::max(PrimeFrames(), LoopStart));
            }

            void Start(const AudioClip* clip, int64_t frame)
            {
                Clip = clip;
                Playing = clip && clip->GetChannels() > 0;
                Frame = std::max<int64_t>(frame, 0);
                Fraction = 0.0;
                StagingCount = 0;
                if (!Playing)
                    return;
                if (ClipFrames() > 0 && Frame >= ClipFrames())
                {
                    Playing = false;
                    return;
                }
                if (Streamed())
                {
                    StagingStart = std::max(PrimeFrames(), Frame);
                    Ask(Current, StagingStart);
                    PrepareLoop();
                }
            }

            void Refill()
            {
                AudioClipStream* stream = Streams[Current];
                if (!stream || !stream->IsReady())
                    return;
                const int64_t keepFrom = std::max(Frame, StagingStart);
                const int64_t drop = std::min<int64_t>(keepFrom - StagingStart, StagingCount);
                if (drop > 0)
                {
                    StagingCount -= static_cast<uint32_t>(drop);
                    std::memmove(Staging.data(), Staging.data() + drop * 2, sizeof(float) * StagingCount * 2);
                    StagingStart += drop;
                }
                if (StagingCount < kStagingFrames)
                {
                    const uint32_t got = stream->Read(Staging.data() + StagingCount * 2, kStagingFrames - StagingCount);
                    if (got > 0)
                        Touched[Current] = true;
                    StagingCount += got;
                }
            }

            // Frame `i` (stereo; a mono clip on both). False when a streamed frame has not been decoded yet.
            bool Get(int64_t i, float& left, float& right)
            {
                const uint32_t channels = Clip->GetChannels();
                const float* frame = nullptr;
                if (Clip->IsDecoded())
                    frame = Clip->GetSamples().data() + i * channels;
                else if (i < PrimeFrames())
                    frame = Clip->GetPrimeSamples().data() + i * channels;
                if (frame)
                {
                    left = frame[0];
                    right = channels == 2 ? frame[1] : frame[0];
                    return true;
                }
                int64_t local = i - StagingStart;
                if (local >= static_cast<int64_t>(StagingCount))
                {
                    Refill();
                    local = i - StagingStart;
                }
                if (local < 0 || local >= static_cast<int64_t>(StagingCount))
                    return false;
                left = Staging[local * 2];
                right = Staging[local * 2 + 1];
                return true;
            }

            // Frames [from, to) of the block, at `rate` source frames per output frame. Loops and the end go to the triggers.
            void Render(float* left, float* right, uint32_t from, uint32_t to, float rate, AudioTriggers* looped, AudioTriggers* finished)
            {
                rate = std::clamp(rate, 0.0f, kMaxPlaybackRate);
                for (uint32_t n = from; n < to; ++n)
                {
                    if (!Playing)
                    {
                        left[n] = right[n] = 0.0f;
                        continue;
                    }
                    const int64_t clipFrames = ClipFrames();
                    const bool looping = Loop && LoopEnd > LoopStart;
                    const int64_t end = looping ? LoopEnd : clipFrames;
                    float l0, r0;
                    if (!Get(Frame, l0, r0))
                    {
                        AudioClipStream* stream = Streams[Current];
                        if (stream && stream->HasEnded())
                        {
                            // The end of a clip whose length was not known up front.
                            Playing = false;
                            if (finished)
                                finished->Add(n);
                            left[n] = right[n] = 0.0f;
                            continue;
                        }
                        StarvedThisBlock = true; // not decoded yet: silence, the playhead holds
                        for (; n < to; ++n)
                            left[n] = right[n] = 0.0f;
                        return;
                    }
                    // The frame after it: the loop's start across the wrap (held where that is a stream not read yet), silence at the end.
                    float l1 = 0.0f, r1 = 0.0f;
                    int64_t next = Frame + 1;
                    if (end > 0 && next >= end)
                    {
                        if (looping)
                        {
                            next = LoopStart;
                            if (Streamed() && next >= PrimeFrames())
                                l1 = l0, r1 = r0;
                            else
                                Get(next, l1, r1);
                        }
                    }
                    else if (!Get(next, l1, r1))
                        l1 = l0, r1 = r0;
                    const float t = static_cast<float>(Fraction);
                    left[n] = l0 + (l1 - l0) * t;
                    right[n] = r0 + (r1 - r0) * t;

                    Fraction += rate;
                    const double whole = std::floor(Fraction);
                    Frame += static_cast<int64_t>(whole);
                    Fraction -= whole;
                    if (end > 0 && Frame >= end)
                    {
                        if (looping)
                        {
                            Frame = LoopStart + (Frame - end) % (LoopEnd - LoopStart);
                            if (looped)
                                looped->Add(n);
                            if (Streamed())
                            {
                                if (Streams[1])
                                    Current = 1 - Current; // the stream kept ready at the loop's start
                                StagingStart = std::max(PrimeFrames(), LoopStart);
                                StagingCount = 0;
                                Ask(Current, StagingStart); // (no-op: it was asked for already)
                                PrepareLoop();
                            }
                        }
                        else
                        {
                            Playing = false;
                            if (finished)
                                finished->Add(std::min(n + 1, N - 1));
                        }
                    }
                }
            }

            float RateFor(const AudioNodeRun& run, float semitones) const
            {
                if (!Clip)
                    return 1.0f;
                return static_cast<float>(Clip->GetSampleRate()) / run.SampleRate() * SemitonesToRatio(semitones) * run.VoicePitch();
            }
        };

        // ---- Inputs and output ----------------------------------------------------------------------------------------------

        struct GetValue
        {
            struct State { int32_t Index = -1; };
            static void Init(State& state, AudioNodeSetup& setup) { state.Index = setup.ParameterIndex(setup.String(kParameterReferenceProperty)); }
            static void Process(AudioNodeRun& run, State& state) { run.SetFloat(0, run.Parameter(state.Index)); }
        };

        struct GetTrigger
        {
            struct State { int32_t Index = -1; };
            static void Init(State& state, AudioNodeSetup& setup) { state.Index = setup.ParameterIndex(setup.String(kParameterReferenceProperty)); }
            static void Process(AudioNodeRun& run, State& state) { run.TriggerOut(0) = run.ParameterTrigger(state.Index); }
        };

        struct Output : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const AudioTriggers& finished = run.Trigger(2);
                if (finished.Any())
                    run.FinishVoice(finished.Offsets[0]);
            }
        };

        // ---- Sources --------------------------------------------------------------------------------------------------------

        // Plays a clip: Play / Stop, Start Time, Pitch Shift (semitones), Loop with a region (MetaSounds' Wave Player).
        struct WavePlayer
        {
            struct State
            {
                ClipPlayer Player;
                const AudioClip* Clip = nullptr;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                state.Clip = setup.Clip(setup.Handle("Clip"));
                if (state.Clip && !state.Clip->IsDecoded())
                {
                    state.Player.Streams[0] = setup.CreateStream();
                    state.Player.Streams[1] = setup.CreateStream();
                }
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                ClipPlayer& player = state.Player;
                float* left = run.AudioOut(0);
                float* right = run.AudioOut(1);
                AudioTriggers& onPlay = run.TriggerOut(2);
                AudioTriggers& onFinished = run.TriggerOut(3);
                AudioTriggers& onLooped = run.TriggerOut(4);
                player.StarvedThisBlock = false;

                const AudioClip* clip = state.Clip;
                const float clipRate = clip ? static_cast<float>(clip->GetSampleRate()) : 1.0f;
                player.Loop = run.Bool(4);
                const int64_t clipFrames = clip ? static_cast<int64_t>(clip->GetFrameCount()) : 0;
                player.LoopStart = std::clamp<int64_t>(static_cast<int64_t>(std::max(run.Float(5), 0.0f) * clipRate), 0, std::max<int64_t>(clipFrames - 1, 0));
                const float loopDuration = run.Float(6);
                player.LoopEnd = loopDuration > 0.0f ? std::min<int64_t>(player.LoopStart + static_cast<int64_t>(loopDuration * clipRate), clipFrames) : clipFrames;
                if (player.Playing)
                    player.PrepareLoop();
                const float semitones = run.Float(3);

                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                uint32_t frame = 0;
                for (uint32_t e = 0; e < count; ++e)
                {
                    player.Render(left, right, frame, events[e].Frame, player.RateFor(run, semitones), &onLooped, &onFinished);
                    frame = events[e].Frame;
                    if (events[e].Kind == 0)
                    {
                        player.Start(clip, static_cast<int64_t>(std::max(run.Float(2), 0.0f) * clipRate));
                        if (player.Playing)
                            onPlay.Add(frame);
                        else
                            onFinished.Add(frame);
                    }
                    else if (player.Playing)
                    {
                        player.Playing = false;
                        onFinished.Add(frame);
                    }
                }
                player.Render(left, right, frame, N, player.RateFor(run, semitones), &onLooped, &onFinished);
                if (player.StarvedThisBlock)
                    run.ReportStarvation();
                run.SetFloat(5, static_cast<float>(static_cast<double>(player.Frame) / clipRate));
            }
        };

        constexpr uint32_t kClipSlots = 8;
        std::string ClipSlot(uint32_t i) { return "Clip " + std::to_string(i + 1); }
        std::string WeightSlot(uint32_t i) { return "Weight " + std::to_string(i + 1); }

        // A clip of a list on every Play: by weight, never one of the last No Repeats (footstep and impact variations).
        struct RandomWavePlayer
        {
            struct State
            {
                ClipPlayer Player;
                const AudioClip* Clips[kClipSlots] = {};
                float Weights[kClipSlots] = {};
                uint32_t Count = 0;
                int32_t NoRepeats = 1;
                int32_t History[kClipSlots] = {};
                uint32_t HistoryCount = 0;
                int32_t Index = -1;
                Random Rng;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                bool streamed = false;
                for (uint32_t i = 0; i < kClipSlots; ++i)
                {
                    const AudioClip* clip = setup.Clip(setup.Handle(ClipSlot(i)));
                    if (!clip)
                        continue;
                    state.Clips[state.Count] = clip;
                    state.Weights[state.Count] = std::max(setup.Float(WeightSlot(i), 1.0f), 0.0f);
                    ++state.Count;
                    streamed |= !clip->IsDecoded();
                }
                state.NoRepeats = std::clamp(setup.Int("No Repeats", 1), 0, static_cast<int32_t>(kClipSlots) - 1);
                state.Rng.Seed(SeedOf(setup));
                if (streamed)
                    state.Player.Streams[0] = setup.CreateStream();
            }
            static int32_t Pick(State& state)
            {
                if (state.Count == 0)
                    return -1;
                // The recent picks are left out while something else can be chosen.
                const uint32_t exclude = std::min<uint32_t>(std::min<uint32_t>(state.NoRepeats, state.HistoryCount), state.Count - 1);
                auto excluded = [&](uint32_t i)
                {
                    for (uint32_t h = 0; h < exclude; ++h)
                        if (state.History[h] == static_cast<int32_t>(i))
                            return true;
                    return false;
                };
                float total = 0.0f;
                for (uint32_t i = 0; i < state.Count; ++i)
                    if (!excluded(i))
                        total += state.Weights[i];
                int32_t pick = -1;
                if (total > 0.0f)
                {
                    float r = state.Rng.Unit() * total;
                    for (uint32_t i = 0; i < state.Count && pick < 0; ++i)
                    {
                        if (excluded(i))
                            continue;
                        if (r < state.Weights[i])
                            pick = static_cast<int32_t>(i);
                        r -= state.Weights[i];
                    }
                    if (pick < 0) // (rounding)
                        for (uint32_t i = state.Count; i-- > 0 && pick < 0;)
                            if (!excluded(i) && state.Weights[i] > 0.0f)
                                pick = static_cast<int32_t>(i);
                }
                if (pick < 0)
                    return -1;
                // Newest first.
                for (uint32_t h = std::min(state.HistoryCount, kClipSlots - 1); h > 0; --h)
                    state.History[h] = state.History[h - 1];
                state.History[0] = pick;
                state.HistoryCount = std::min(state.HistoryCount + 1, kClipSlots);
                return pick;
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                ClipPlayer& player = state.Player;
                float* left = run.AudioOut(0);
                float* right = run.AudioOut(1);
                AudioTriggers& onPlay = run.TriggerOut(2);
                AudioTriggers& onFinished = run.TriggerOut(3);
                player.StarvedThisBlock = false;
                const float semitones = run.Float(2);

                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                uint32_t frame = 0;
                for (uint32_t e = 0; e < count; ++e)
                {
                    player.Render(left, right, frame, events[e].Frame, player.RateFor(run, semitones), nullptr, &onFinished);
                    frame = events[e].Frame;
                    if (events[e].Kind == 0)
                    {
                        state.Index = Pick(state);
                        player.Start(state.Index >= 0 ? state.Clips[state.Index] : nullptr, 0);
                        if (player.Playing)
                            onPlay.Add(frame);
                        else
                            onFinished.Add(frame);
                    }
                    else if (player.Playing)
                    {
                        player.Playing = false;
                        onFinished.Add(frame);
                    }
                }
                player.Render(left, right, frame, N, player.RateFor(run, semitones), nullptr, &onFinished);
                if (player.StarvedThisBlock)
                    run.ReportStarvation();
                run.SetInt(4, state.Index);
            }
        };

        // Its clips in order, one per Play (dialogue lines, combo sounds); Reset goes back to the first.
        struct SequencePlayer
        {
            struct State
            {
                ClipPlayer Player;
                const AudioClip* Clips[kClipSlots] = {};
                uint32_t Count = 0;
                bool Wrap = true;
                uint32_t Next = 0;
                int32_t Index = -1;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                bool streamed = false;
                for (uint32_t i = 0; i < kClipSlots; ++i)
                {
                    const AudioClip* clip = setup.Clip(setup.Handle(ClipSlot(i)));
                    if (!clip)
                        continue;
                    state.Clips[state.Count++] = clip;
                    streamed |= !clip->IsDecoded();
                }
                state.Wrap = setup.Bool("Loop Sequence", true);
                if (streamed)
                    state.Player.Streams[0] = setup.CreateStream();
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                ClipPlayer& player = state.Player;
                float* left = run.AudioOut(0);
                float* right = run.AudioOut(1);
                AudioTriggers& onPlay = run.TriggerOut(2);
                AudioTriggers& onFinished = run.TriggerOut(3);
                player.StarvedThisBlock = false;
                const float semitones = run.Float(3);
                if (run.Trigger(1).Any())
                    state.Next = 0; // (Reset: the next Play starts over, also later in this block)

                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(2), events);
                uint32_t frame = 0;
                for (uint32_t e = 0; e < count; ++e)
                {
                    player.Render(left, right, frame, events[e].Frame, player.RateFor(run, semitones), nullptr, &onFinished);
                    frame = events[e].Frame;
                    if (events[e].Kind == 0)
                    {
                        const AudioClip* clip = nullptr;
                        if (state.Next < state.Count)
                        {
                            state.Index = static_cast<int32_t>(state.Next);
                            clip = state.Clips[state.Next++];
                            if (state.Next >= state.Count && state.Wrap)
                                state.Next = 0;
                        }
                        player.Start(clip, 0);
                        if (player.Playing)
                            onPlay.Add(frame);
                        else
                            onFinished.Add(frame);
                    }
                    else if (player.Playing)
                    {
                        player.Playing = false;
                        onFinished.Add(frame);
                    }
                }
                player.Render(left, right, frame, N, player.RateFor(run, semitones), nullptr, &onFinished);
                if (player.StarvedThisBlock)
                    run.ReportStarvation();
                run.SetInt(4, state.Index);
            }
        };

        // Sine / Saw / Square / Triangle, band-limited (PolyBLEP steps, PolyBLAMP corners): no aliasing at high pitches.
        struct Oscillator
        {
            struct State
            {
                int32_t Waveform = 0;
                double Phase = 0.0;
                Ramp Frequency;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Waveform = setup.Int("Waveform", 0); }
            static void Process(AudioNodeRun& run, State& state)
            {
                float* out = run.AudioOut(0);
                if (!run.Bool(0))
                {
                    std::fill(out, out + N, 0.0f);
                    return;
                }
                const AudioTriggers& reset = run.Trigger(3);
                uint32_t nextReset = 0;
                float frequency;
                const float step = state.Frequency.Begin(std::max(run.Float(1), 0.0f) * run.VoicePitch(), frequency);
                const float width = std::clamp(run.Float(2), 0.01f, 0.99f);
                const float nyquist = run.SampleRate() * 0.5f;
                for (uint32_t n = 0; n < N; ++n, frequency += step)
                {
                    while (nextReset < reset.Count && reset.Offsets[nextReset] == n)
                    {
                        state.Phase = 0.0;
                        ++nextReset;
                    }
                    const float dt = std::min(frequency, nyquist) / run.SampleRate();
                    const float p = static_cast<float>(state.Phase);
                    float value;
                    switch (state.Waveform)
                    {
                    case 1: // Saw
                        value = 2.0f * p - 1.0f - PolyBlep(p, dt);
                        break;
                    case 2: // Square (pulse width)
                    {
                        float second = p + 1.0f - width;
                        second -= std::floor(second);
                        value = (p < width ? 1.0f : -1.0f) + PolyBlep(p, dt) - PolyBlep(second, dt);
                        break;
                    }
                    case 3: // Triangle: +1 at phase 0, -1 at 0.5; its corners (slope changes of -8 and +8 a cycle) rounded by PolyBLAMP
                    {
                        value = 4.0f * std::fabs(p - 0.5f) - 1.0f;
                        float half = p + 0.5f;
                        half -= std::floor(half);
                        value += 8.0f * dt * (PolyBlamp(half, dt) - PolyBlamp(p, dt));
                        break;
                    }
                    default: // Sine
                        value = std::sin(kTwoPi * p);
                        break;
                    }
                    out[n] = value;
                    state.Phase += dt;
                    state.Phase -= std::floor(state.Phase);
                }
            }
        };

        // White (flat), Pink (-3 dB / octave: Paul Kellet's filter) or Brown (-6 dB / octave: leaky integration) noise.
        struct Noise
        {
            struct State
            {
                int32_t Type = 0;
                Random Rng;
                float B0 = 0, B1 = 0, B2 = 0, B3 = 0, B4 = 0, B5 = 0, B6 = 0;
                float Brown = 0;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                state.Type = setup.Int("Type", 0);
                state.Rng.Seed(SeedOf(setup));
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                float* out = run.AudioOut(0);
                for (uint32_t n = 0; n < N; ++n)
                {
                    const float white = state.Rng.Bipolar();
                    if (state.Type == 1)
                    {
                        state.B0 = 0.99886f * state.B0 + white * 0.0555179f;
                        state.B1 = 0.99332f * state.B1 + white * 0.0750759f;
                        state.B2 = 0.96900f * state.B2 + white * 0.1538520f;
                        state.B3 = 0.86650f * state.B3 + white * 0.3104856f;
                        state.B4 = 0.55000f * state.B4 + white * 0.5329522f;
                        state.B5 = -0.7616f * state.B5 - white * 0.0168980f;
                        out[n] = (state.B0 + state.B1 + state.B2 + state.B3 + state.B4 + state.B5 + state.B6 + white * 0.5362f) * 0.11f;
                        state.B6 = white * 0.115926f;
                    }
                    else if (state.Type == 2)
                    {
                        state.Brown = (state.Brown + 0.02f * white) / 1.02f;
                        out[n] = state.Brown * 3.5f;
                    }
                    else
                        out[n] = white;
                }
            }
        };

        // ---- Triggers and logic ---------------------------------------------------------------------------------------------

        struct OnPlay : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                if (run.IsFirstBlock())
                    run.TriggerOut(0).Add(0);
            }
        };

        // Fires every Period seconds while running (from Start, or from the voice's start), until Stop.
        struct TriggerRepeat
        {
            struct State
            {
                bool Running = false;
                double Countdown = 0.0; // frames until the next fire
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Running = setup.Bool("Start On Play", true); }
            static void Process(AudioNodeRun& run, State& state)
            {
                AudioTriggers& out = run.TriggerOut(0);
                const double period = std::max(static_cast<double>(run.Float(2)) * run.SampleRate(), 16.0);
                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                uint32_t frame = 0;
                // Fires at exact (fractional) times; Countdown carries the time to the next one across blocks.
                auto advance = [&](uint32_t until)
                {
                    double position = frame;
                    while (state.Running)
                    {
                        const double fire = position + state.Countdown;
                        if (fire >= until)
                        {
                            state.Countdown = fire - until;
                            break;
                        }
                        out.Add(static_cast<uint32_t>(fire));
                        position = fire;
                        state.Countdown = period;
                    }
                    frame = until;
                };
                for (uint32_t e = 0; e < count; ++e)
                {
                    advance(events[e].Frame);
                    if (events[e].Kind == 0)
                    {
                        state.Running = true;
                        state.Countdown = 0.0; // fires at once
                    }
                    else
                        state.Running = false;
                }
                advance(N);
            }
        };

        // Each input fires again Delay seconds later (up to 16 waiting); Reset drops the waiting ones.
        struct TriggerDelay
        {
            struct State
            {
                double Pending[kMaxTriggersPerBlock] = {}; // frames from this block's start
                uint32_t Count = 0;
            };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                AudioTriggers& out = run.TriggerOut(0);
                if (run.Trigger(1).Any())
                    state.Count = 0;
                const double delay = std::max(static_cast<double>(run.Float(2)), 0.0) * run.SampleRate();
                const AudioTriggers& in = run.Trigger(0);
                for (uint32_t i = 0; i < in.Count && state.Count < kMaxTriggersPerBlock; ++i)
                    state.Pending[state.Count++] = in.Offsets[i] + delay;
                uint32_t kept = 0;
                for (uint32_t i = 0; i < state.Count; ++i)
                {
                    if (state.Pending[i] < N)
                        out.Add(static_cast<uint32_t>(state.Pending[i]));
                    else
                        state.Pending[kept++] = state.Pending[i] - N;
                }
                state.Count = kept;
            }
        };

        // Counts its input; fires On Count at Count (and starts over, with Auto Reset).
        struct TriggerCounter
        {
            struct State
            {
                int32_t Value = 0;
                bool AutoReset = true;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.AutoReset = setup.Bool("Auto Reset", true); }
            static void Process(AudioNodeRun& run, State& state)
            {
                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                const int32_t target = std::max(run.Int(2), 1);
                for (uint32_t e = 0; e < count; ++e)
                {
                    if (events[e].Kind == 1)
                    {
                        state.Value = 0;
                        continue;
                    }
                    ++state.Value;
                    if (state.Value == target)
                    {
                        run.TriggerOut(0).Add(events[e].Frame);
                        if (state.AutoReset)
                            state.Value = 0;
                    }
                }
                run.SetInt(1, state.Value);
            }
        };

        // Passes each input with a probability (Passed), the others to Blocked.
        struct TriggerRandom
        {
            struct State { Random Rng; };
            static void Init(State& state, AudioNodeSetup& setup) { state.Rng.Seed(SeedOf(setup)); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const AudioTriggers& in = run.Trigger(0);
                const float probability = std::clamp(run.Float(1), 0.0f, 1.0f);
                for (uint32_t i = 0; i < in.Count; ++i)
                    run.TriggerOut(state.Rng.Unit() < probability ? 0 : 1).Add(in.Offsets[i]);
            }
        };

        constexpr uint32_t kRouteOutputs = 4;

        // Each input to one of its first Count outputs: at random (no output twice in a row), or in turn (Trigger Sequence).
        template <bool Sequential>
        struct TriggerRoute
        {
            struct State
            {
                Random Rng;
                int32_t Last = -1;
                uint32_t Next = 0;
                uint32_t Count = 2;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                state.Rng.Seed(SeedOf(setup));
                state.Count = static_cast<uint32_t>(std::clamp(setup.Int("Outputs", 2), 2, static_cast<int32_t>(kRouteOutputs)));
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                for (uint32_t e = 0; e < count; ++e)
                {
                    if (events[e].Kind == 1)
                    {
                        state.Next = 0;
                        state.Last = -1;
                        continue;
                    }
                    uint32_t output;
                    if (Sequential)
                        output = state.Next++ % state.Count;
                    else
                    {
                        output = state.Rng.Next() % (state.Last < 0 ? state.Count : state.Count - 1);
                        if (state.Last >= 0 && output >= static_cast<uint32_t>(state.Last))
                            ++output;
                    }
                    state.Last = static_cast<int32_t>(output);
                    run.TriggerOut(output).Add(events[e].Frame);
                }
            }
        };

        // Any of its inputs.
        struct TriggerAny : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                AudioTriggers& out = run.TriggerOut(0);
                for (uint32_t input = 0; input < run.InputCount(); ++input)
                {
                    const AudioTriggers& in = run.Trigger(input);
                    for (uint32_t i = 0; i < in.Count; ++i)
                        out.Add(in.Offsets[i]);
                }
            }
        };

        // Fires once every input has fired (since it last fired or was reset).
        struct TriggerAll
        {
            struct State { uint32_t Seen = 0; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                // Inputs: Reset, then the variable ones.
                const uint32_t variable = run.InputCount() - 1;
                const uint32_t all = variable >= 32 ? 0xFFFFFFFFu : (1u << variable) - 1u;
                Event events[kMaxTriggersPerBlock * 34];
                uint32_t count = 0;
                for (uint32_t input = 0; input < run.InputCount(); ++input)
                {
                    const AudioTriggers& in = run.Trigger(input);
                    for (uint32_t i = 0; i < in.Count; ++i)
                        events[count++] = { in.Offsets[i], static_cast<uint8_t>(input) };
                }
                std::sort(events, events + count, [](const Event& a, const Event& b) { return a.Frame < b.Frame; });
                for (uint32_t e = 0; e < count; ++e)
                {
                    if (events[e].Kind == 0)
                    {
                        state.Seen = 0;
                        continue;
                    }
                    state.Seen |= 1u << ((events[e].Kind - 1) & 31);
                    if ((state.Seen & all) == all)
                    {
                        run.TriggerOut(0).Add(events[e].Frame);
                        state.Seen = 0;
                    }
                }
            }
        };

        // Only its first input passes, until Reset.
        struct TriggerOnce
        {
            struct State { bool Done = false; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                for (uint32_t e = 0; e < count; ++e)
                {
                    if (events[e].Kind == 1)
                        state.Done = false;
                    else if (!state.Done)
                    {
                        run.TriggerOut(0).Add(events[e].Frame);
                        state.Done = true;
                    }
                }
            }
        };

        // Passes its input while Open.
        struct TriggerGate : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                if (run.Bool(1))
                    run.TriggerOut(0) = run.Trigger(0);
            }
        };

        // Its input to True or False by Condition.
        struct TriggerBranch : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&) { run.TriggerOut(run.Bool(1) ? 0 : 1) = run.Trigger(0); }
        };

        // Compares A with B.
        struct Compare
        {
            struct State { int32_t Op = 0; };
            static void Init(State& state, AudioNodeSetup& setup) { state.Op = setup.Int("Operation", 0); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const float a = run.Float(0), b = run.Float(1);
                bool result = false;
                switch (state.Op)
                {
                case 0: result = a == b; break;
                case 1: result = a != b; break;
                case 2: result = a > b; break;
                case 3: result = a >= b; break;
                case 4: result = a < b; break;
                case 5: result = a <= b; break;
                }
                run.SetBool(0, result);
            }
        };

        // True or False by Condition.
        struct Select : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&) { run.SetFloat(0, run.Bool(0) ? run.Float(1) : run.Float(2)); }
        };

        // One of its value inputs by Index (clamped).
        struct Switch : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const int32_t count = static_cast<int32_t>(run.InputCount()) - 1;
                const int32_t index = std::clamp(run.Int(0), 0, std::max(count - 1, 0));
                run.SetFloat(0, count > 0 ? run.Float(1 + index) : 0.0f);
            }
        };

        // ---- Values and math ------------------------------------------------------------------------------------------------

        enum class MathOp { Add, Subtract, Multiply, Divide, Min, Max };
        template <MathOp Op>
        struct Math : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float a = run.Float(0), b = run.Float(1);
                float result = 0.0f;
                if constexpr (Op == MathOp::Add) result = a + b;
                else if constexpr (Op == MathOp::Subtract) result = a - b;
                else if constexpr (Op == MathOp::Multiply) result = a * b;
                else if constexpr (Op == MathOp::Divide) result = b != 0.0f ? a / b : 0.0f;
                else if constexpr (Op == MathOp::Min) result = std::min(a, b);
                else result = std::max(a, b);
                run.SetFloat(0, result);
            }
        };

        // One value in, one out.
        enum class UnaryOp { Abs, DbToLinear, LinearToDb, MidiToFrequency, FrequencyToMidi, SemitonesToRatio, RatioToSemitones, IntToFloat, FloatToInt, BoolToFloat };
        template <UnaryOp Op>
        struct Unary : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float x = run.Float(0);
                float y = x;
                if constexpr (Op == UnaryOp::Abs) y = std::fabs(x);
                else if constexpr (Op == UnaryOp::DbToLinear) y = DbToLinear(x);
                else if constexpr (Op == UnaryOp::LinearToDb) y = LinearToDb(x);
                else if constexpr (Op == UnaryOp::MidiToFrequency) y = 440.0f * std::exp2((x - 69.0f) / 12.0f);
                else if constexpr (Op == UnaryOp::FrequencyToMidi) y = 69.0f + 12.0f * std::log2(std::max(x, 1e-6f) / 440.0f);
                else if constexpr (Op == UnaryOp::SemitonesToRatio) y = SemitonesToRatio(x);
                else if constexpr (Op == UnaryOp::RatioToSemitones) y = 12.0f * std::log2(std::max(x, 1e-6f));
                else if constexpr (Op == UnaryOp::FloatToInt) y = std::round(x);
                run.SetFloat(0, y);
            }
        };

        struct Clamp : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float low = run.Float(1), high = run.Float(2);
                run.SetFloat(0, std::clamp(run.Float(0), std::min(low, high), std::max(low, high)));
            }
        };

        // e.g. an engine's RPM 800..7000 to a pitch of 0.8..1.6.
        struct MapRange : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float inMin = run.Float(1), inMax = run.Float(2), outMin = run.Float(3), outMax = run.Float(4);
                float t = inMax != inMin ? (run.Float(0) - inMin) / (inMax - inMin) : 0.0f;
                if (run.Bool(5))
                    t = std::clamp(t, 0.0f, 1.0f);
                run.SetFloat(0, outMin + (outMax - outMin) * t);
            }
        };

        struct Lerp : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&) { run.SetFloat(0, run.Float(0) + (run.Float(1) - run.Float(0)) * run.Float(2)); }
        };

        // A random value between Min and Max, rolled when the voice starts and on every Next (a random pitch per footstep).
        template <bool Integer>
        struct RandomValue
        {
            struct State
            {
                Random Rng;
                float Value = 0.0f;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Rng.Seed(SeedOf(setup)); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const float low = run.Float(1), high = run.Float(2);
                auto roll = [&]()
                {
                    if constexpr (Integer)
                    {
                        const int32_t a = static_cast<int32_t>(std::round(std::min(low, high)));
                        const int32_t b = static_cast<int32_t>(std::round(std::max(low, high)));
                        state.Value = static_cast<float>(a + static_cast<int32_t>(state.Rng.Next() % static_cast<uint32_t>(b - a + 1)));
                    }
                    else
                        state.Value = low + (high - low) * state.Rng.Unit();
                };
                if (run.IsFirstBlock())
                    roll();
                for (uint32_t i = 0; i < run.Trigger(0).Count; ++i)
                    roll();
                run.SetFloat(0, state.Value);
            }
        };

        // Moves towards Target in Time seconds (a straight line), so a jumping parameter glides.
        struct Interpolate
        {
            struct State
            {
                float Value = 0.0f;
                float Target = 0.0f;
                float Slope = 0.0f; // per second
                bool Started = false;
            };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float target = run.Float(0);
                const float time = std::max(run.Float(1), 0.0f);
                if (!state.Started)
                {
                    state.Value = state.Target = target;
                    state.Started = true;
                }
                if (target != state.Target)
                {
                    state.Target = target;
                    state.Slope = time > 0.0f ? (target - state.Value) / time : 0.0f;
                    if (time <= 0.0f)
                        state.Value = target;
                }
                const float step = state.Slope * static_cast<float>(N) / run.SampleRate();
                if (state.Value != state.Target)
                {
                    state.Value += step;
                    if ((step >= 0.0f && state.Value >= state.Target) || (step <= 0.0f && state.Value <= state.Target))
                        state.Value = state.Target;
                }
                run.SetFloat(0, state.Value);
            }
        };

        // Audio arithmetic, sample by sample.
        template <bool Multiply>
        struct AudioMath : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float* a = run.Audio(0);
                const float* b = run.Audio(1);
                float* out = run.AudioOut(0);
                for (uint32_t n = 0; n < N; ++n)
                    out[n] = Multiply ? a[n] * b[n] : a[n] + b[n];
            }
        };

        // Audio times a gain (linear, or in dB), ramped.
        template <bool Decibels>
        struct Gain
        {
            struct State { Ramp Value; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(0);
                float gain;
                const float step = state.Value.Begin(Decibels ? DbToLinear(run.Float(1)) : run.Float(1), gain);
                for (uint32_t n = 0; n < N; ++n, gain += step)
                    out[n] = in[n] * gain;
            }
        };

        // ---- Envelopes and modulation ---------------------------------------------------------------------------------------

        // An exponential segment lasts its time: it ends 60 dB below where it started (its time is the time to -60 dB, as reverb times
        // are), the step from there to its target inaudible.
        constexpr float kSegmentEnd = 0.001f;
        // Per-frame multiplier taking a level from 1 to -60 dB in `seconds`.
        float DecayCoefficient(float seconds, float sampleRate) { return std::exp(std::log(0.001f) / std::max(seconds * sampleRate, 1.0f)); }

        // Attack then decay on each trigger: as audio (multiply a signal by it) and as a value.
        struct ADEnvelope
        {
            struct State
            {
                int32_t Curve = 1;
                uint8_t Stage = 0; // 0 idle, 1 attack, 2 decay
                float Level = 0.0f;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Curve = setup.Int("Curve", 1); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const AudioTriggers& trigger = run.Trigger(0);
                float* out = run.AudioOut(0);
                const float attackStep = 1.0f / std::max(run.Float(1) * run.SampleRate(), 1.0f);
                const float decayTime = std::max(run.Float(2), 0.0f);
                const float decayStep = 1.0f / std::max(decayTime * run.SampleRate(), 1.0f);
                const float decayCoefficient = DecayCoefficient(decayTime, run.SampleRate());
                uint32_t next = 0;
                for (uint32_t n = 0; n < N; ++n)
                {
                    while (next < trigger.Count && trigger.Offsets[next] == n)
                    {
                        state.Stage = 1; // (from where it is: a retrigger does not click)
                        ++next;
                    }
                    if (state.Stage == 1)
                    {
                        state.Level += attackStep;
                        if (state.Level >= 1.0f)
                        {
                            state.Level = 1.0f;
                            state.Stage = 2;
                        }
                    }
                    else if (state.Stage == 2)
                    {
                        state.Level = state.Curve == 0 ? state.Level - decayStep : state.Level * decayCoefficient;
                        if (state.Level <= (state.Curve == 0 ? 0.0f : kSegmentEnd))
                        {
                            state.Level = 0.0f;
                            state.Stage = 0;
                            run.TriggerOut(2).Add(n);
                        }
                    }
                    out[n] = state.Level;
                }
                run.SetFloat(1, state.Level);
            }
        };

        // Attack, decay to the sustain level while held, release on Release.
        struct ADSREnvelope
        {
            struct State
            {
                int32_t Curve = 1;
                uint8_t Stage = 0; // 0 idle, 1 attack, 2 decay, 3 sustain, 4 release
                float Level = 0.0f;
                float ReleaseFrom = 0.0f; // the level the release started at
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Curve = setup.Int("Curve", 1); }
            static void Process(AudioNodeRun& run, State& state)
            {
                Event events[kMaxTriggersPerBlock * 2];
                const uint32_t count = MergeEvents(run.Trigger(0), run.Trigger(1), events);
                float* out = run.AudioOut(0);
                const float sampleRate = run.SampleRate();
                const float attackStep = 1.0f / std::max(run.Float(2) * sampleRate, 1.0f);
                const float decayTime = std::max(run.Float(3), 0.0f);
                const float sustain = std::clamp(run.Float(4), 0.0f, 1.0f);
                const float releaseTime = std::max(run.Float(5), 0.0f);
                const float decayCoefficient = DecayCoefficient(decayTime, sampleRate);
                const float releaseCoefficient = DecayCoefficient(releaseTime, sampleRate);
                const float decayStep = (1.0f - sustain) / std::max(decayTime * sampleRate, 1.0f);
                const float releaseStep = state.ReleaseFrom / std::max(releaseTime * sampleRate, 1.0f);
                uint32_t next = 0;
                for (uint32_t n = 0; n < N; ++n)
                {
                    while (next < count && events[next].Frame == n)
                    {
                        if (events[next].Kind == 0)
                            state.Stage = 1;
                        else if (state.Stage != 0)
                        {
                            state.Stage = 4;
                            state.ReleaseFrom = state.Level;
                        }
                        ++next;
                    }
                    switch (state.Stage)
                    {
                    case 1:
                        state.Level += attackStep;
                        if (state.Level >= 1.0f)
                        {
                            state.Level = 1.0f;
                            state.Stage = 2;
                        }
                        break;
                    case 2:
                        // Exponential: towards the sustain level, its distance shrinking to -60 dB in the decay time.
                        state.Level = state.Curve == 0 ? state.Level - decayStep : sustain + (state.Level - sustain) * decayCoefficient;
                        if (state.Level <= sustain + (state.Curve == 0 ? 0.0f : (1.0f - sustain) * kSegmentEnd))
                        {
                            state.Level = sustain;
                            state.Stage = 3;
                        }
                        break;
                    case 3:
                        state.Level = sustain;
                        break;
                    case 4:
                    {
                        // (Linear: the release's own step, from where it started, also when it started this block.)
                        const float step = releaseStep > 0.0f ? releaseStep : state.ReleaseFrom / std::max(releaseTime * sampleRate, 1.0f);
                        state.Level = state.Curve == 0 ? state.Level - step : state.Level * releaseCoefficient;
                        if (state.Level <= (state.Curve == 0 ? 0.0f : state.ReleaseFrom * kSegmentEnd))
                        {
                            state.Level = 0.0f;
                            state.Stage = 0;
                            run.TriggerOut(2).Add(n);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                    out[n] = state.Level;
                }
                run.SetFloat(1, state.Level);
            }
        };

        // A slow oscillator between Min and Max for modulation (vibrato, tremolo, a sweeping filter): as a value and as audio.
        struct LFO
        {
            struct State
            {
                int32_t Shape = 0;
                double Phase = 0.0;
                float Held = 0.0f; // Sample & Hold's value this cycle
                Random Rng;
            };
            static void Init(State& state, AudioNodeSetup& setup)
            {
                state.Shape = setup.Int("Shape", 0);
                state.Phase = std::clamp(setup.Float("Phase Offset", 0.0f), 0.0f, 1.0f);
                state.Rng.Seed(SeedOf(setup));
                state.Held = state.Rng.Bipolar();
            }
            static void Process(AudioNodeRun& run, State& state)
            {
                float* out = run.AudioOut(1);
                const float low = run.Float(1), high = run.Float(2);
                const double dt = std::max(run.Float(0), 0.0f) / run.SampleRate();
                const AudioTriggers& reset = run.Trigger(3);
                uint32_t next = 0;
                float value = 0.0f;
                for (uint32_t n = 0; n < N; ++n)
                {
                    while (next < reset.Count && reset.Offsets[next] == n)
                    {
                        state.Phase = 0.0;
                        ++next;
                    }
                    const float p = static_cast<float>(state.Phase);
                    float bipolar;
                    switch (state.Shape)
                    {
                    case 1: bipolar = 1.0f - 4.0f * std::fabs(p - 0.5f); break; // Triangle (-1 at 0, +1 at 0.5)
                    case 2: bipolar = 2.0f * p - 1.0f; break;                   // Saw
                    case 3: bipolar = p < 0.5f ? 1.0f : -1.0f; break;           // Square
                    case 4: bipolar = state.Held; break;                        // Sample & Hold
                    default: bipolar = std::sin(kTwoPi * p); break;             // Sine
                    }
                    value = low + (high - low) * (bipolar * 0.5f + 0.5f);
                    out[n] = value;
                    state.Phase += dt;
                    if (state.Phase >= 1.0)
                    {
                        state.Phase -= std::floor(state.Phase);
                        state.Held = state.Rng.Bipolar();
                    }
                }
                run.SetFloat(0, value);
            }
        };

        // An audio input's loudness (peak envelope, linear): a sidechain inside a graph.
        struct EnvelopeFollower
        {
            struct State { float Level = 0.0f; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(1);
                const float sampleRate = run.SampleRate();
                const float attack = std::exp(-1.0f / std::max(run.Float(1) * sampleRate, 1.0f));
                const float release = std::exp(-1.0f / std::max(run.Float(2) * sampleRate, 1.0f));
                for (uint32_t n = 0; n < N; ++n)
                {
                    const float x = std::fabs(in[n]);
                    const float coefficient = x > state.Level ? attack : release;
                    state.Level = coefficient * state.Level + (1.0f - coefficient) * x;
                    out[n] = state.Level;
                }
                run.SetFloat(0, state.Level);
            }
        };

        // ---- Mixing and routing ---------------------------------------------------------------------------------------------

        // Its inputs, each with a gain (MetaSounds' Mixer): Channels = 1 (In, Gain pairs) or 2 (In Left, In Right, Gain).
        template <uint32_t Channels>
        struct Mixer
        {
            struct State { Ramp Gains[kMaxGroupInputs]; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                constexpr uint32_t group = Channels + 1;
                float* outs[Channels];
                for (uint32_t c = 0; c < Channels; ++c)
                {
                    outs[c] = run.AudioOut(c);
                    std::fill(outs[c], outs[c] + N, 0.0f);
                }
                const uint32_t inputs = std::min(run.VariableCount(), kMaxGroupInputs);
                for (uint32_t i = 0; i < inputs; ++i)
                {
                    float gain;
                    const float step = state.Gains[i].Begin(run.Float(i * group + Channels), gain);
                    for (uint32_t c = 0; c < Channels; ++c)
                    {
                        if (!run.IsConnected(i * group + c))
                            continue;
                        const float* in = run.Audio(i * group + c);
                        float g = gain;
                        for (uint32_t n = 0; n < N; ++n, g += step)
                            outs[c][n] += in[n] * g;
                    }
                }
            }
        };

        // Between its inputs by Value (0 = the first, 1 = the second, ... equal-power between neighbours): an engine from idle / low /
        // mid / high recordings by RPM.
        struct Crossfade
        {
            struct State { Ramp Value; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                float* out = run.AudioOut(0);
                const uint32_t inputs = run.VariableCount();
                if (inputs == 0)
                {
                    std::fill(out, out + N, 0.0f);
                    return;
                }
                float value;
                const float step = state.Value.Begin(std::clamp(run.Float(0), 0.0f, static_cast<float>(inputs - 1)), value);
                for (uint32_t n = 0; n < N; ++n, value += step)
                {
                    const uint32_t low = std::min(static_cast<uint32_t>(value), inputs - 1);
                    const uint32_t high = std::min(low + 1, inputs - 1);
                    const float t = value - static_cast<float>(low);
                    const float a = std::cos(t * kPi * 0.5f), b = std::sin(t * kPi * 0.5f);
                    out[n] = run.Audio(1 + low)[n] * a + (high != low ? run.Audio(1 + high)[n] * b : 0.0f);
                }
            }
        };

        // A mono signal placed left (-1) to right (+1): equal power (-3 dB in the middle) or linear.
        struct StereoPanner
        {
            struct State
            {
                int32_t Law = 0;
                Ramp Pan;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Law = setup.Int("Pan Law", 0); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* left = run.AudioOut(0);
                float* right = run.AudioOut(1);
                float pan;
                const float step = state.Pan.Begin(std::clamp(run.Float(1), -1.0f, 1.0f), pan);
                for (uint32_t n = 0; n < N; ++n, pan += step)
                {
                    const float t = (pan + 1.0f) * 0.5f;
                    const float l = state.Law == 0 ? std::cos(t * kPi * 0.5f) : 1.0f - t;
                    const float r = state.Law == 0 ? std::sin(t * kPi * 0.5f) : t;
                    left[n] = in[n] * l;
                    right[n] = in[n] * r;
                }
            }
        };

        struct MonoToStereo : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                std::memcpy(run.AudioOut(0), run.Audio(0), sizeof(float) * N);
                std::memcpy(run.AudioOut(1), run.Audio(0), sizeof(float) * N);
            }
        };

        struct StereoToMono : NoInit
        {
            using State = NoState;
            static void Process(AudioNodeRun& run, State&)
            {
                const float* left = run.Audio(0);
                const float* right = run.Audio(1);
                float* out = run.AudioOut(0);
                for (uint32_t n = 0; n < N; ++n)
                    out[n] = 0.5f * (left[n] + right[n]);
            }
        };

        // ---- Filters ---------------------------------------------------------------------------------------------------------

        // A frequency moving smoothly from the last block's to this block's: interpolated in octaves (what the ear hears as even).
        struct FrequencyRamp
        {
            float Previous = 0.0f;
            bool Started = false;
            void Begin(float target, float sampleRate, float& from, float& to)
            {
                target = std::clamp(target, 10.0f, sampleRate * 0.49f);
                if (!Started)
                {
                    Previous = target;
                    Started = true;
                }
                from = Previous;
                to = target;
                Previous = target;
            }
            static float At(float from, float to, float t) { return from * std::pow(to / from, t); }
        };

        // Low pass / high pass / band pass / notch / peak / shelves (RBJ cookbook).
        struct BiquadFilter
        {
            struct State
            {
                int32_t Type = 0;
                Biquad Filter;
                FrequencyRamp Frequency;
            };
            static void Init(State& state, AudioNodeSetup& setup) { state.Type = setup.Int("Type", 0); }
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(0);
                float from, to;
                state.Frequency.Begin(run.Float(1), run.SampleRate(), from, to);
                for (uint32_t start = 0; start < N; start += kCoefficientStep)
                {
                    const float t = static_cast<float>(start + kCoefficientStep) / static_cast<float>(N);
                    state.Filter.Set(static_cast<BiquadType>(state.Type), FrequencyRamp::At(from, to, t), run.Float(2), run.Float(3), run.SampleRate());
                    for (uint32_t n = start; n < start + kCoefficientStep; ++n)
                        out[n] = state.Filter.Process(in[n]);
                }
            }
        };

        // Andrew Simper's trapezoidal state variable filter: low, high, band pass and notch at once, smooth under fast modulation.
        struct StateVariableFilter
        {
            struct State
            {
                float Ic1 = 0.0f, Ic2 = 0.0f;
                FrequencyRamp Frequency;
            };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* low = run.AudioOut(0);
                float* high = run.AudioOut(1);
                float* band = run.AudioOut(2);
                float* notch = run.AudioOut(3);
                const float k = 1.0f / std::max(run.Float(2), 0.025f);
                float from, to;
                state.Frequency.Begin(run.Float(1), run.SampleRate(), from, to);
                for (uint32_t start = 0; start < N; start += kCoefficientStep)
                {
                    const float t = static_cast<float>(start + kCoefficientStep) / static_cast<float>(N);
                    const float g = std::tan(kPi * FrequencyRamp::At(from, to, t) / run.SampleRate());
                    const float a1 = 1.0f / (1.0f + g * (g + k));
                    const float a2 = g * a1;
                    const float a3 = g * a2;
                    for (uint32_t n = start; n < start + kCoefficientStep; ++n)
                    {
                        const float v0 = in[n];
                        const float v3 = v0 - state.Ic2;
                        const float v1 = a1 * state.Ic1 + a2 * v3;
                        const float v2 = state.Ic2 + a2 * state.Ic1 + a3 * v3;
                        state.Ic1 = 2.0f * v1 - state.Ic1;
                        state.Ic2 = 2.0f * v2 - state.Ic2;
                        low[n] = v2;
                        band[n] = v1;
                        high[n] = v0 - k * v1 - v2;
                        notch[n] = low[n] + high[n];
                    }
                }
            }
        };

        // A synth's 4-pole resonant low pass (Moog-style): Vadim Zavalishin's zero-delay-feedback ladder, its input driven through tanh.
        // Resonance 1 self-oscillates.
        struct LadderFilter
        {
            struct State
            {
                float S[4] = {};
                FrequencyRamp Frequency;
            };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(0);
                const float k = 4.0f * std::clamp(run.Float(2), 0.0f, 1.0f);
                const float drive = std::max(run.Float(3), 0.01f);
                float from, to;
                state.Frequency.Begin(run.Float(1), run.SampleRate(), from, to);
                for (uint32_t start = 0; start < N; start += kCoefficientStep)
                {
                    const float t = static_cast<float>(start + kCoefficientStep) / static_cast<float>(N);
                    const float g = std::tan(kPi * FrequencyRamp::At(from, to, t) / run.SampleRate());
                    const float G = g / (1.0f + g);
                    const float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
                    for (uint32_t n = start; n < start + kCoefficientStep; ++n)
                    {
                        // The output solved for at once (y4 = G^4 u + S, u = x - k y4), then the stages run with it.
                        const float x = std::tanh(in[n] * drive);
                        const float sum = (G3 * state.S[0] + G2 * state.S[1] + G * state.S[2] + state.S[3]) / (1.0f + g);
                        const float y4 = (G4 * x + sum) / (1.0f + k * G4);
                        float u = x - k * y4;
                        for (float& s : state.S)
                        {
                            const float v = (u - s) * G;
                            const float y = v + s;
                            s = y + v;
                            u = y;
                        }
                        out[n] = u * (1.0f + k * 0.5f); // (some of the level resonance takes, given back)
                    }
                }
            }
        };

        // One pole (6 dB / octave): cheap muffling (behind a wall, underwater) or rumble removal.
        template <bool HighPass>
        struct OnePole
        {
            struct State
            {
                float S = 0.0f;
                FrequencyRamp Frequency;
            };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(0);
                float from, to;
                state.Frequency.Begin(run.Float(1), run.SampleRate(), from, to);
                for (uint32_t start = 0; start < N; start += kCoefficientStep)
                {
                    const float t = static_cast<float>(start + kCoefficientStep) / static_cast<float>(N);
                    const float g = std::tan(kPi * FrequencyRamp::At(from, to, t) / run.SampleRate());
                    const float G = g / (1.0f + g);
                    for (uint32_t n = start; n < start + kCoefficientStep; ++n)
                    {
                        const float v = (in[n] - state.S) * G;
                        const float low = v + state.S;
                        state.S = low + v;
                        out[n] = HighPass ? in[n] - low : low;
                    }
                }
            }
        };

        // Three bands in one: a low shelf, a peak, a high shelf.
        struct ParametricEQ
        {
            struct State { Biquad Bands[3]; };
            static void Init(State&, AudioNodeSetup&) {}
            static void Process(AudioNodeRun& run, State& state)
            {
                const float* in = run.Audio(0);
                float* out = run.AudioOut(0);
                const float sampleRate = run.SampleRate();
                state.Bands[0].Set(BiquadType::LowShelf, run.Float(1), 0.707f, run.Float(2), sampleRate);
                state.Bands[1].Set(BiquadType::Peak, run.Float(3), run.Float(5), run.Float(4), sampleRate);
                state.Bands[2].Set(BiquadType::HighShelf, run.Float(6), 0.707f, run.Float(7), sampleRate);
                for (uint32_t n = 0; n < N; ++n)
                    out[n] = state.Bands[2].Process(state.Bands[1].Process(state.Bands[0].Process(in[n])));
            }
        };
    }

    void RegisterAudioGraphNodeTypes()
    {
        const std::vector<NodePinDesc> stereoOut = { { "Out Left", "Audio" }, { "Out Right", "Audio" } };
        const std::vector<NodePinDesc> monoOut = { { "Out", "Audio" } };
        const std::vector<NodePinDesc> valueOut = { { "Value", "Float" } };

        // Inputs (graph parameters) and the output.
        for (const char* type : { "Float", "Int", "Bool" })
        {
            NodeTypeDesc desc = Desc((std::string("Get ") + type).c_str(), "Inputs", {}, { { "Value", type } });
            desc.DefaultProperties.emplace_back(kParameterReferenceProperty, std::string());
            desc.ParameterType = type;
            Register<GetValue>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Get Trigger", "Inputs", {}, { { "Trigger", "Trigger" } });
            desc.DefaultProperties.emplace_back(kParameterReferenceProperty, std::string());
            desc.ParameterType = "Trigger";
            Register<GetTrigger>(std::move(desc));
        }
        Register<Output>(Desc("Output", "Sinks", { A("Out Left"), A("Out Right"), T("On Finished") }, {}));

        // Sources.
        {
            NodeTypeDesc desc = Desc("Wave Player", "Sources",
                { T("Play"), T("Stop"), F("Start Time", 0.0f), F("Pitch Shift", 0.0f, -24.0f, 24.0f), B("Loop", false), F("Loop Start", 0.0f), F("Loop Duration", -1.0f) },
                { { "Out Left", "Audio" }, { "Out Right", "Audio" }, { "On Play", "Trigger" }, { "On Finished", "Trigger" }, { "On Looped", "Trigger" }, { "Position", "Float" } });
            ClipProperty(desc, "Clip");
            Register<WavePlayer>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Random Wave Player", "Sources", { T("Play"), T("Stop"), F("Pitch Shift", 0.0f, -24.0f, 24.0f) },
                { { "Out Left", "Audio" }, { "Out Right", "Audio" }, { "On Play", "Trigger" }, { "On Finished", "Trigger" }, { "Index", "Int" } });
            for (uint32_t i = 0; i < kClipSlots; ++i)
                ClipProperty(desc, ClipSlot(i));
            for (uint32_t i = 0; i < kClipSlots; ++i)
            {
                Property(desc, WeightSlot(i).c_str(), 1.0f);
                desc.PropertyRanges[WeightSlot(i)] = { 0.0f, 10.0f };
            }
            Property(desc, "No Repeats", int32_t(1));
            Property(desc, "Seed", int32_t(-1));
            Register<RandomWavePlayer>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Sequence Player", "Sources", { T("Play"), T("Reset"), T("Stop"), F("Pitch Shift", 0.0f, -24.0f, 24.0f) },
                { { "Out Left", "Audio" }, { "Out Right", "Audio" }, { "On Play", "Trigger" }, { "On Finished", "Trigger" }, { "Index", "Int" } });
            for (uint32_t i = 0; i < kClipSlots; ++i)
                ClipProperty(desc, ClipSlot(i));
            Property(desc, "Loop Sequence", true);
            Register<SequencePlayer>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Oscillator", "Sources", { B("Enabled", true), F("Frequency", 440.0f), F("Pulse Width", 0.5f, 0.01f, 0.99f), T("Phase Reset") }, monoOut);
            Option(desc, "Waveform", { "Sine", "Saw", "Square", "Triangle" });
            Register<Oscillator>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Noise", "Sources", {}, monoOut);
            Option(desc, "Type", { "White", "Pink", "Brown" });
            Property(desc, "Seed", int32_t(-1));
            Register<Noise>(std::move(desc));
        }

        // Triggers and logic.
        Register<OnPlay>(Desc("On Play", "Triggers", {}, { { "Out", "Trigger" } }));
        {
            NodeTypeDesc desc = Desc("Trigger Repeat", "Triggers", { T("Start"), T("Stop"), F("Period", 0.5f) }, { { "Out", "Trigger" } });
            Property(desc, "Start On Play", true);
            Register<TriggerRepeat>(std::move(desc));
        }
        Register<TriggerDelay>(Desc("Trigger Delay", "Triggers", { T("In"), T("Reset"), F("Delay", 0.25f) }, { { "Out", "Trigger" } }));
        {
            NodeTypeDesc desc = Desc("Trigger Counter", "Triggers", { T("In"), T("Reset"), I("Count", 4) }, { { "On Count", "Trigger" }, { "Value", "Int" } });
            Property(desc, "Auto Reset", true);
            Register<TriggerCounter>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Trigger Random", "Triggers", { T("In"), F("Probability", 0.5f, 0.0f, 1.0f) }, { { "Passed", "Trigger" }, { "Blocked", "Trigger" } });
            Property(desc, "Seed", int32_t(-1));
            Register<TriggerRandom>(std::move(desc));
        }
        for (bool sequential : { false, true })
        {
            NodeTypeDesc desc = Desc(sequential ? "Trigger Sequence" : "Trigger Pick Random", "Triggers", { T("In"), T("Reset") },
                { { "Out 1", "Trigger" }, { "Out 2", "Trigger" }, { "Out 3", "Trigger" }, { "Out 4", "Trigger" } });
            Property(desc, "Outputs", int32_t(2));
            desc.PropertyRanges["Outputs"] = { 2.0f, 4.0f };
            if (!sequential)
                Property(desc, "Seed", int32_t(-1));
            if (sequential)
                Register<TriggerRoute<true>>(std::move(desc));
            else
                Register<TriggerRoute<false>>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Trigger Any", "Triggers", {}, { { "Out", "Trigger" } });
            desc.VariableInputs = { { "In", "Trigger" } };
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Register<TriggerAny>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Trigger All", "Triggers", { T("Reset") }, { { "Out", "Trigger" } });
            desc.VariableInputs = { { "In", "Trigger" } };
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Register<TriggerAll>(std::move(desc));
        }
        Register<TriggerOnce>(Desc("Trigger Once", "Triggers", { T("In"), T("Reset") }, { { "Out", "Trigger" } }));
        Register<TriggerGate>(Desc("Trigger Gate", "Triggers", { T("In"), B("Open", true) }, { { "Out", "Trigger" } }));
        Register<TriggerBranch>(Desc("Trigger Branch", "Triggers", { T("In"), B("Condition", true) }, { { "True", "Trigger" }, { "False", "Trigger" } }));
        {
            NodeTypeDesc desc = Desc("Compare", "Triggers", { F("A", 0.0f), F("B", 0.0f) }, { { "Result", "Bool" } });
            Option(desc, "Operation", { "A == B", "A != B", "A > B", "A >= B", "A < B", "A <= B" }, 2);
            Register<Compare>(std::move(desc));
        }
        Register<Select>(Desc("Select", "Triggers", { B("Condition", true), F("True", 1.0f), F("False", 0.0f) }, valueOut));
        {
            NodeTypeDesc desc = Desc("Switch", "Triggers", { I("Index", 0) }, valueOut);
            desc.VariableInputs = { { "Value", "Float" } };
            desc.VariableInputLiterals["Value"] = 0.0f;
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Property(desc, "Value 0", 0.0f);
            Property(desc, "Value 1", 1.0f);
            Register<Switch>(std::move(desc));
        }

        // Values and math.
        Register<Math<MathOp::Add>>(Desc("Add", "Math", { F("A", 0.0f), F("B", 0.0f) }, valueOut));
        Register<Math<MathOp::Subtract>>(Desc("Subtract", "Math", { F("A", 0.0f), F("B", 0.0f) }, valueOut));
        Register<Math<MathOp::Multiply>>(Desc("Multiply", "Math", { F("A", 1.0f), F("B", 1.0f) }, valueOut));
        Register<Math<MathOp::Divide>>(Desc("Divide", "Math", { F("A", 1.0f), F("B", 1.0f) }, valueOut));
        Register<Math<MathOp::Min>>(Desc("Min", "Math", { F("A", 0.0f), F("B", 0.0f) }, valueOut));
        Register<Math<MathOp::Max>>(Desc("Max", "Math", { F("A", 0.0f), F("B", 0.0f) }, valueOut));
        Register<Clamp>(Desc("Clamp", "Math", { F("Value", 0.0f), F("Min", 0.0f), F("Max", 1.0f) }, valueOut));
        Register<Unary<UnaryOp::Abs>>(Desc("Abs", "Math", { F("Value", 0.0f) }, valueOut));
        Register<MapRange>(Desc("Map Range", "Math", { F("Value", 0.0f), F("In Min", 0.0f), F("In Max", 1.0f), F("Out Min", 0.0f), F("Out Max", 1.0f), B("Clamp", true) }, valueOut));
        Register<Lerp>(Desc("Lerp", "Math", { F("A", 0.0f), F("B", 1.0f), F("Alpha", 0.5f, 0.0f, 1.0f) }, valueOut));
        {
            NodeTypeDesc desc = Desc("Random Float", "Math", { T("Next"), F("Min", 0.0f), F("Max", 1.0f) }, valueOut);
            Property(desc, "Seed", int32_t(-1));
            Register<RandomValue<false>>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Random Int", "Math", { T("Next"), I("Min", 0), I("Max", 3) }, { { "Value", "Int" } });
            Property(desc, "Seed", int32_t(-1));
            Register<RandomValue<true>>(std::move(desc));
        }
        Register<Interpolate>(Desc("Interpolate", "Math", { F("Target", 0.0f), F("Time", 0.1f) }, valueOut));
        Register<Unary<UnaryOp::DbToLinear>>(Desc("dB To Linear", "Math", { F("Value", 0.0f) }, valueOut));
        Register<Unary<UnaryOp::LinearToDb>>(Desc("Linear To dB", "Math", { F("Value", 1.0f) }, valueOut));
        Register<Unary<UnaryOp::MidiToFrequency>>(Desc("MIDI To Frequency", "Math", { F("Value", 69.0f) }, valueOut));
        Register<Unary<UnaryOp::FrequencyToMidi>>(Desc("Frequency To MIDI", "Math", { F("Value", 440.0f) }, valueOut));
        Register<Unary<UnaryOp::SemitonesToRatio>>(Desc("Semitones To Ratio", "Math", { F("Value", 0.0f) }, valueOut));
        Register<Unary<UnaryOp::RatioToSemitones>>(Desc("Ratio To Semitones", "Math", { F("Value", 1.0f) }, valueOut));
        Register<Unary<UnaryOp::IntToFloat>>(Desc("Int To Float", "Math", { I("Value", 0) }, valueOut));
        Register<Unary<UnaryOp::FloatToInt>>(Desc("Float To Int", "Math", { F("Value", 0.0f) }, { { "Value", "Int" } }));
        Register<Unary<UnaryOp::BoolToFloat>>(Desc("Bool To Float", "Math", { B("Value", false) }, valueOut));
        Register<AudioMath<false>>(Desc("Add (Audio)", "Math", { A("A"), A("B") }, monoOut));
        Register<AudioMath<true>>(Desc("Multiply (Audio)", "Math", { A("A"), A("B") }, monoOut));

        // Envelopes and modulation.
        {
            NodeTypeDesc desc = Desc("AD Envelope", "Envelopes", { T("Trigger"), F("Attack", 0.01f), F("Decay", 0.5f) },
                { { "Out", "Audio" }, { "Value", "Float" }, { "On Done", "Trigger" } });
            Option(desc, "Curve", { "Linear", "Exponential" }, 1);
            Register<ADEnvelope>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("ADSR Envelope", "Envelopes",
                { T("Attack"), T("Release"), F("Attack Time", 0.01f), F("Decay Time", 0.2f), F("Sustain Level", 0.7f, 0.0f, 1.0f), F("Release Time", 0.5f) },
                { { "Out", "Audio" }, { "Value", "Float" }, { "On Done", "Trigger" } });
            Option(desc, "Curve", { "Linear", "Exponential" }, 1);
            Register<ADSREnvelope>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("LFO", "Envelopes", { F("Frequency", 1.0f), F("Min", 0.0f), F("Max", 1.0f), T("Phase Reset") },
                { { "Value", "Float" }, { "Out", "Audio" } });
            Option(desc, "Shape", { "Sine", "Triangle", "Saw", "Square", "Sample & Hold" });
            Property(desc, "Phase Offset", 0.0f);
            desc.PropertyRanges["Phase Offset"] = { 0.0f, 1.0f };
            Property(desc, "Seed", int32_t(-1));
            Register<LFO>(std::move(desc));
        }
        Register<EnvelopeFollower>(Desc("Envelope Follower", "Envelopes", { A("In"), F("Attack", 0.01f), F("Release", 0.2f) },
            { { "Value", "Float" }, { "Out", "Audio" } }));

        // Mixing and routing.
        {
            NodeTypeDesc desc = Desc("Mixer", "Mixing", {}, monoOut);
            desc.VariableInputs = { { "In", "Audio" }, { "Gain", "Float" } };
            desc.VariableInputLiterals["Gain"] = 1.0f;
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Property(desc, "Gain 0", 1.0f);
            Property(desc, "Gain 1", 1.0f);
            Register<Mixer<1>>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Stereo Mixer", "Mixing", {}, stereoOut);
            desc.VariableInputs = { { "In Left", "Audio" }, { "In Right", "Audio" }, { "Gain", "Float" } };
            desc.VariableInputLiterals["Gain"] = 1.0f;
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Property(desc, "Gain 0", 1.0f);
            Property(desc, "Gain 1", 1.0f);
            Register<Mixer<2>>(std::move(desc));
        }
        {
            NodeTypeDesc desc = Desc("Crossfade", "Mixing", { F("Value", 0.0f) }, monoOut);
            desc.VariableInputs = { { "In", "Audio" } };
            Property(desc, kVariableInputCountProperty, int32_t(2));
            Register<Crossfade>(std::move(desc));
        }
        Register<Gain<false>>(Desc("Gain", "Mixing", { A("In"), F("Gain", 1.0f, 0.0f, 2.0f) }, monoOut));
        Register<Gain<true>>(Desc("Gain (dB)", "Mixing", { A("In"), F("Gain", 0.0f, -60.0f, 12.0f) }, monoOut));
        {
            NodeTypeDesc desc = Desc("Stereo Panner", "Mixing", { A("In"), F("Pan", 0.0f, -1.0f, 1.0f) }, stereoOut);
            Option(desc, "Pan Law", { "Equal Power", "Linear" });
            Register<StereoPanner>(std::move(desc));
        }
        Register<MonoToStereo>(Desc("Mono To Stereo", "Mixing", { A("In") }, stereoOut));
        Register<StereoToMono>(Desc("Stereo To Mono", "Mixing", { A("In Left"), A("In Right") }, monoOut));

        // Filters.
        {
            NodeTypeDesc desc = Desc("Biquad Filter", "Filters", { A("In"), F("Frequency", 1000.0f), F("Q", 0.707f), F("Gain", 0.0f, -24.0f, 24.0f) }, monoOut);
            Option(desc, "Type", { "Low Pass", "High Pass", "Band Pass", "Notch", "Peak", "Low Shelf", "High Shelf" });
            Register<BiquadFilter>(std::move(desc));
        }
        Register<StateVariableFilter>(Desc("State Variable Filter", "Filters", { A("In"), F("Frequency", 1000.0f), F("Q", 0.707f) },
            { { "Low Pass", "Audio" }, { "High Pass", "Audio" }, { "Band Pass", "Audio" }, { "Notch", "Audio" } }));
        Register<LadderFilter>(Desc("Ladder Filter", "Filters", { A("In"), F("Frequency", 1000.0f), F("Resonance", 0.3f, 0.0f, 1.0f), F("Drive", 1.0f, 0.1f, 10.0f) }, monoOut));
        Register<OnePole<false>>(Desc("One-Pole Low Pass", "Filters", { A("In"), F("Frequency", 1000.0f) }, monoOut));
        Register<OnePole<true>>(Desc("One-Pole High Pass", "Filters", { A("In"), F("Frequency", 100.0f) }, monoOut));
        Register<ParametricEQ>(Desc("Parametric EQ", "Filters",
            { A("In"), F("Low Frequency", 200.0f), F("Low Gain", 0.0f, -24.0f, 24.0f), F("Mid Frequency", 1000.0f), F("Mid Gain", 0.0f, -24.0f, 24.0f),
              F("Mid Q", 1.0f), F("High Frequency", 5000.0f), F("High Gain", 0.0f, -24.0f, 24.0f) }, monoOut));
    }

    NodeGraph CreateEmptyAudioGraph()
    {
        NodeGraph graph;
        graph.Domain = kAudioGraphDomain;
        GraphNode output;
        output.Id = graph.NextNodeId++;
        output.TypeName = "Output";
        output.EditorPosition = { 400.0f, 0.0f };
        graph.Nodes.push_back(output);
        graph.OutputNode = output.Id;
        return graph;
    }
}
