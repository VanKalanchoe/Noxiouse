// The mixer's and the audio graph's effects (docs/Audio_Architecture_Plan_2026.md §7, phase A3). Each is the textbook design the big
// engines' and plug-ins' versions are:
//   EQ / Filter        RBJ cookbook biquads, parameters smoothed in octaves
//   Compressor         Giannoulis, Massberg & Reiss (2012): soft-knee gain computer, smooth branching attack / release in dB
//   Limiter            look-ahead brickwall: sliding minimum of the needed gain over the look-ahead, release, then a box filter of the
//                      same length -- the gain is already down when the peak arrives (no overshoot, no clipping distortion)
//   Gate               peak detector, hysteresis, hold, attack / release
//   Delay              Hermite-interpolated lines (time changes slew like tape), damped feedback, ping pong / cross
//   Reverb             Jot's feedback delay network: 8 modulated lines, Householder feedback, per-line absorption filters set from
//                      the decay time and its high-frequency ratio; Schroeder all-pass input diffusion; early reflections
//   Convolution Reverb uniformly partitioned overlap-save FFT convolution (256-frame partitions: no latency beyond the block)
//   Chorus / Flanger   modulated delays (three voices / one), feedback
//   Phaser             swept first-order all-pass stages with feedback
//   Distortion         waveshapers run at 2x through a half-band FIR (aliasing kept down), DC blocked, tone filter
//   Bitcrusher         sample-and-hold rate reduction, quantization

#include "AudioEffects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>

#include "AudioClip.h"
#include "AudioDSP.h"
#include "NoxCore/Core/Log.h"

namespace Nox
{
    using namespace AudioDSP;

    namespace
    {
        constexpr uint32_t kMax = AudioEffect::MaxFrames;
        using Complex = std::complex<float>;

        AudioEffectParameterInfo P(const char* name, float min, float max, float value, const char* unit = "", bool logarithmic = false)
        {
            AudioEffectParameterInfo info;
            info.Name = name;
            info.Min = min;
            info.Max = max;
            info.Default = value;
            info.Unit = unit;
            info.Logarithmic = logarithmic;
            return info;
        }
        AudioEffectParameterInfo Toggle(const char* name, bool value)
        {
            AudioEffectParameterInfo info = P(name, 0.0f, 1.0f, value ? 1.0f : 0.0f);
            info.Toggle = true;
            return info;
        }
        AudioEffectParameterInfo Choice(const char* name, std::vector<std::string> options, int32_t value = 0)
        {
            AudioEffectParameterInfo info = P(name, 0.0f, static_cast<float>(options.size() - 1), static_cast<float>(value));
            info.Options = std::move(options);
            return info;
        }

        std::vector<AudioEffectInfo> BuildInfos()
        {
            std::vector<AudioEffectInfo> infos(static_cast<size_t>(AudioEffectType::Count));
            auto set = [&](AudioEffectType type, const char* name, const char* description, std::vector<AudioEffectParameterInfo> parameters)
                -> AudioEffectInfo&
            {
                AudioEffectInfo& info = infos[static_cast<size_t>(type)];
                info.Type = type;
                info.Name = name;
                info.Description = description;
                info.Parameters = std::move(parameters);
                return info;
            };
            set(AudioEffectType::EQ, "EQ", "Four-band equalizer: a low shelf, two peaks, a high shelf.",
                { P("Low Frequency", 20.0f, 1000.0f, 100.0f, "Hz", true), P("Low Gain", -24.0f, 24.0f, 0.0f, "dB"),
                  P("Low Mid Frequency", 80.0f, 5000.0f, 500.0f, "Hz", true), P("Low Mid Gain", -24.0f, 24.0f, 0.0f, "dB"), P("Low Mid Q", 0.1f, 10.0f, 1.0f, "", true),
                  P("High Mid Frequency", 400.0f, 16000.0f, 2500.0f, "Hz", true), P("High Mid Gain", -24.0f, 24.0f, 0.0f, "dB"), P("High Mid Q", 0.1f, 10.0f, 1.0f, "", true),
                  P("High Frequency", 1000.0f, 20000.0f, 8000.0f, "Hz", true), P("High Gain", -24.0f, 24.0f, 0.0f, "dB"),
                  P("Output", -24.0f, 24.0f, 0.0f, "dB") });
            set(AudioEffectType::Filter, "Filter", "Low / high / band pass filter (12 dB per octave). A snapshot's muffle: Pause, Underwater.",
                { Choice("Type", { "Low Pass", "High Pass", "Band Pass" }), P("Frequency", 20.0f, 20000.0f, 20000.0f, "Hz", true),
                  P("Resonance", 0.1f, 10.0f, 0.707f, "", true) });
            set(AudioEffectType::Compressor, "Compressor", "Turns loud parts down. With a sidechain bus, that bus's level turns this one down (ducking).",
                { P("Threshold", -60.0f, 0.0f, -20.0f, "dB"), P("Ratio", 1.0f, 20.0f, 4.0f, ":1", true), P("Knee", 0.0f, 24.0f, 6.0f, "dB"),
                  P("Attack", 0.1f, 200.0f, 10.0f, "ms", true), P("Release", 5.0f, 2000.0f, 100.0f, "ms", true), P("Makeup", 0.0f, 24.0f, 0.0f, "dB"),
                  P("Mix", 0.0f, 1.0f, 1.0f), Choice("Detector", { "Peak", "RMS" }) }).Sidechain = true;
            infos[static_cast<size_t>(AudioEffectType::Compressor)].ReportsGainReduction = true;
            set(AudioEffectType::Limiter, "Limiter", "Look-ahead brickwall limiter: nothing passes the ceiling. Master's last effect.",
                { P("Input Gain", -12.0f, 24.0f, 0.0f, "dB"), P("Ceiling", -24.0f, 0.0f, -0.3f, "dB"), P("Release", 1.0f, 1000.0f, 50.0f, "ms", true),
                  P("Lookahead", 0.5f, 10.0f, 5.0f, "ms") }).ReportsGainReduction = true;
            set(AudioEffectType::Gate, "Gate", "Silences the signal while it is below the threshold.",
                { P("Threshold", -90.0f, 0.0f, -50.0f, "dB"), P("Attack", 0.1f, 100.0f, 1.0f, "ms", true), P("Hold", 0.0f, 1000.0f, 50.0f, "ms"),
                  P("Release", 5.0f, 2000.0f, 100.0f, "ms", true), P("Range", -90.0f, 0.0f, -90.0f, "dB") }).Sidechain = true;
            infos[static_cast<size_t>(AudioEffectType::Gate)].ReportsGainReduction = true;
            set(AudioEffectType::Delay, "Delay", "Stereo delay (echo) with damped feedback.",
                { Choice("Mode", { "Normal", "Ping Pong", "Cross" }), P("Time Left", 1.0f, 2000.0f, 300.0f, "ms", true), P("Time Right", 1.0f, 2000.0f, 400.0f, "ms", true),
                  P("Feedback", 0.0f, 0.95f, 0.4f), P("Damping", 500.0f, 20000.0f, 8000.0f, "Hz", true),
                  P("Wet", kSilenceDb, 12.0f, -6.0f, "dB"), P("Dry", kSilenceDb, 12.0f, 0.0f, "dB") });
            set(AudioEffectType::Reverb, "Reverb", "Algorithmic room: early reflections and a diffuse tail.",
                { P("Pre-Delay", 0.0f, 200.0f, 10.0f, "ms"), P("Decay Time", 0.1f, 20.0f, 1.8f, "s", true), P("HF Ratio", 0.1f, 1.0f, 0.5f),
                  P("Room Size", 0.0f, 1.0f, 0.6f), P("Diffusion", 0.0f, 1.0f, 0.8f), P("Early Level", kSilenceDb, 12.0f, -6.0f, "dB"),
                  P("Width", 0.0f, 1.0f, 1.0f), P("Wet", kSilenceDb, 12.0f, -6.0f, "dB"), P("Dry", kSilenceDb, 12.0f, 0.0f, "dB") });
            set(AudioEffectType::ConvolutionReverb, "Convolution Reverb", "A recorded space: the sound convolved with an impulse response clip.",
                { P("Pre-Delay", 0.0f, 200.0f, 0.0f, "ms"), P("Wet", kSilenceDb, 12.0f, -6.0f, "dB"), P("Dry", kSilenceDb, 12.0f, 0.0f, "dB"),
                  Toggle("Normalize", true) }).ImpulseResponse = true;
            set(AudioEffectType::Chorus, "Chorus", "Three modulated voices per side: a wider, thicker sound.",
                { P("Rate", 0.01f, 10.0f, 0.8f, "Hz", true), P("Depth", 0.0f, 20.0f, 3.0f, "ms"), P("Delay", 1.0f, 40.0f, 15.0f, "ms"),
                  P("Feedback", -0.9f, 0.9f, 0.0f), P("Spread", 0.0f, 1.0f, 1.0f), P("Mix", 0.0f, 1.0f, 0.5f) });
            set(AudioEffectType::Flanger, "Flanger", "A short modulated delay with feedback: the jet sweep.",
                { P("Rate", 0.01f, 10.0f, 0.25f, "Hz", true), P("Depth", 0.0f, 10.0f, 2.0f, "ms"), P("Delay", 0.1f, 10.0f, 1.0f, "ms"),
                  P("Feedback", -0.95f, 0.95f, 0.6f), P("Spread", 0.0f, 1.0f, 0.5f), P("Mix", 0.0f, 1.0f, 0.5f) });
            set(AudioEffectType::Phaser, "Phaser", "Swept all-pass stages: moving notches.",
                { P("Rate", 0.01f, 10.0f, 0.5f, "Hz", true), P("Depth", 0.0f, 1.0f, 0.8f), P("Frequency", 100.0f, 5000.0f, 800.0f, "Hz", true),
                  P("Feedback", -0.95f, 0.95f, 0.5f), Choice("Stages", { "2", "4", "6", "8", "12" }, 1), P("Spread", 0.0f, 1.0f, 0.5f),
                  P("Mix", 0.0f, 1.0f, 0.5f) });
            set(AudioEffectType::Distortion, "Distortion", "Waveshaping, oversampled 2x: soft / hard clip, foldback, tube.",
                { Choice("Type", { "Soft Clip", "Hard Clip", "Foldback", "Tube" }), P("Drive", 0.0f, 48.0f, 12.0f, "dB"),
                  P("Tone", 500.0f, 20000.0f, 8000.0f, "Hz", true), P("Output", -48.0f, 12.0f, -6.0f, "dB"), P("Mix", 0.0f, 1.0f, 1.0f) });
            set(AudioEffectType::Bitcrusher, "Bitcrusher", "Fewer bits, a lower sample rate: lo-fi.",
                { P("Bits", 1.0f, 24.0f, 8.0f), P("Sample Rate", 500.0f, 48000.0f, 8000.0f, "Hz", true), P("Mix", 0.0f, 1.0f, 1.0f) });
            return infos;
        }

        const std::vector<AudioEffectInfo>& Infos()
        {
            static const std::vector<AudioEffectInfo> infos = BuildInfos();
            return infos;
        }

        // A parameter moving toward its target block by block (~20 ms); frequencies in octaves. True while it moves.
        bool SmoothTowards(float& current, float target, float k, bool logarithmic)
        {
            if (current == target)
                return false;
            if (logarithmic && current > 0.0f && target > 0.0f)
            {
                const float next = current * std::pow(target / current, k);
                current = std::fabs(std::log2(target / next)) < 1e-3f ? target : next;
            }
            else
            {
                const float next = current + (target - current) * k;
                current = std::fabs(target - next) < 1e-4f * std::max(1.0f, std::fabs(target)) ? target : next;
            }
            return true;
        }

        // The per-block smoothing factor of a ~20 ms one-pole.
        float BlockSmoothing(uint32_t frames, float sampleRate) { return 1.0f - std::exp(-static_cast<float>(frames) / (0.02f * sampleRate)); }

        // A gain from dB, ramped across the block.
        struct GainRamp
        {
            Ramp R;
            float Begin(float db, float& start, uint32_t frames) { return R.Begin(DbToGain(db), start, frames); }
            float BeginLinear(float gain, float& start, uint32_t frames) { return R.Begin(gain, start, frames); }
        };

        // ---- EQ ---------------------------------------------------------------------------------------------------------------

        class EqEffect final : public AudioEffect
        {
            enum { LowFrequency, LowGain, LowMidFrequency, LowMidGain, LowMidQ, HighMidFrequency, HighMidGain, HighMidQ, HighFrequency, HighGain, Output };

        public:
            explicit EqEffect(float sampleRate) : AudioEffect(AudioEffectType::EQ, sampleRate) {}

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const float k = m_Started ? BlockSmoothing(frames, m_SampleRate) : 1.0f;
                bool changed = !m_Started;
                for (uint32_t i = 0; i < Output; ++i)
                {
                    const bool logarithmic = i == LowFrequency || i == LowMidFrequency || i == HighMidFrequency || i == HighFrequency || i == LowMidQ || i == HighMidQ;
                    changed |= SmoothTowards(m_Current[i], Value(i), k, logarithmic);
                }
                m_Started = true;
                if (changed)
                {
                    for (Biquad* bands : { m_Bands[0], m_Bands[1] })
                    {
                        bands[0].Set(BiquadType::LowShelf, m_Current[LowFrequency], 0.707f, m_Current[LowGain], m_SampleRate);
                        bands[1].Set(BiquadType::Peak, m_Current[LowMidFrequency], m_Current[LowMidQ], m_Current[LowMidGain], m_SampleRate);
                        bands[2].Set(BiquadType::Peak, m_Current[HighMidFrequency], m_Current[HighMidQ], m_Current[HighMidGain], m_SampleRate);
                        bands[3].Set(BiquadType::HighShelf, m_Current[HighFrequency], 0.707f, m_Current[HighGain], m_SampleRate);
                    }
                }
                float gain;
                const float step = m_Output.Begin(Value(Output), gain, frames);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    gain += step;
                    float l = left[n], r = right[n];
                    for (uint32_t b = 0; b < 4; ++b)
                    {
                        l = m_Bands[0][b].Process(l);
                        r = m_Bands[1][b].Process(r);
                    }
                    left[n] = l * gain;
                    right[n] = r * gain;
                }
            }
            void Reset() override
            {
                for (auto& bands : m_Bands)
                    for (Biquad& band : bands)
                        band.Reset();
            }

        private:
            Biquad m_Bands[2][4];
            float m_Current[Output] = {};
            bool m_Started = false;
            GainRamp m_Output;
        };

        // ---- Filter -----------------------------------------------------------------------------------------------------------

        class FilterEffect final : public AudioEffect
        {
            enum { Type, Frequency, Resonance };

        public:
            explicit FilterEffect(float sampleRate) : AudioEffect(AudioEffectType::Filter, sampleRate) {}

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const BiquadType type = std::array{ BiquadType::LowPass, BiquadType::HighPass, BiquadType::BandPass }[std::clamp(static_cast<int>(Value(Type)), 0, 2)];
                if (type != m_Type)
                {
                    m_Type = type;
                    Reset();
                }
                float from, to;
                m_Frequency.Begin(Value(Frequency), m_SampleRate, from, to);
                const float k = m_Started ? BlockSmoothing(frames, m_SampleRate) : 1.0f;
                m_Started = true;
                SmoothTowards(m_Q, Value(Resonance), k, true);
                for (uint32_t start = 0; start < frames; start += kCoefficientStep)
                {
                    const uint32_t end = std::min(frames, start + kCoefficientStep);
                    const float frequency = FrequencyRamp::At(from, to, static_cast<float>(end) / static_cast<float>(frames));
                    m_Filters[0].Set(type, frequency, m_Q, 0.0f, m_SampleRate);
                    m_Filters[1].B0 = m_Filters[0].B0; m_Filters[1].B1 = m_Filters[0].B1; m_Filters[1].B2 = m_Filters[0].B2;
                    m_Filters[1].A1 = m_Filters[0].A1; m_Filters[1].A2 = m_Filters[0].A2;
                    for (uint32_t n = start; n < end; ++n)
                    {
                        left[n] = m_Filters[0].Process(left[n]);
                        right[n] = m_Filters[1].Process(right[n]);
                    }
                }
            }
            void Reset() override
            {
                m_Filters[0].Reset();
                m_Filters[1].Reset();
            }

        private:
            Biquad m_Filters[2];
            BiquadType m_Type = BiquadType::LowPass;
            FrequencyRamp m_Frequency;
            float m_Q = 0.707f;
            bool m_Started = false;
        };

        // ---- Compressor -------------------------------------------------------------------------------------------------------

        class CompressorEffect final : public AudioEffect
        {
            enum { Threshold, Ratio, Knee, Attack, Release, Makeup, Mix, Detector };

        public:
            explicit CompressorEffect(float sampleRate) : AudioEffect(AudioEffectType::Compressor, sampleRate) {}

            // The static curve (Giannoulis et al., eq. 4): the output level for an input level, dB.
            static float GainComputer(float x, float threshold, float ratio, float knee)
            {
                const float over = x - threshold;
                if (2.0f * over < -knee)
                    return x;
                if (knee > 0.0f && 2.0f * std::fabs(over) <= knee)
                {
                    const float t = over + 0.5f * knee;
                    return x + (1.0f / ratio - 1.0f) * t * t / (2.0f * knee);
                }
                return threshold + over / ratio;
            }

            void Process(float* left, float* right, uint32_t frames, const float* keyLeft, const float* keyRight) override
            {
                const float threshold = Value(Threshold);
                const float ratio = std::max(Value(Ratio), 1.0f);
                const float knee = std::max(Value(Knee), 0.0f);
                const float attack = SmoothingCoefficient(Value(Attack) * 0.001f, m_SampleRate);
                const float release = SmoothingCoefficient(Value(Release) * 0.001f, m_SampleRate);
                const float rmsCoefficient = SmoothingCoefficient(0.01f, m_SampleRate);
                const bool rms = Value(Detector) >= 0.5f;
                const float* kl = keyLeft ? keyLeft : left;
                const float* kr = keyRight ? keyRight : (keyLeft ? keyLeft : right);
                float makeup, mix;
                const float makeupStep = m_Makeup.Begin(Value(Makeup), makeup, frames);
                const float mixStep = m_Mix.BeginLinear(std::clamp(Value(Mix), 0.0f, 1.0f), mix, frames);
                float deepest = 0.0f;
                for (uint32_t n = 0; n < frames; ++n)
                {
                    makeup += makeupStep;
                    mix += mixStep;
                    float level = std::max(std::fabs(kl[n]), std::fabs(kr[n]));
                    if (rms)
                    {
                        m_Power = rmsCoefficient * m_Power + (1.0f - rmsCoefficient) * level * level;
                        level = std::sqrt(m_Power);
                    }
                    const float x = LinearToDb(level);
                    const float reduction = GainComputer(x, threshold, ratio, knee) - x; // <= 0
                    const float coefficient = reduction < m_Envelope ? attack : release;
                    m_Envelope = coefficient * m_Envelope + (1.0f - coefficient) * reduction;
                    deepest = std::min(deepest, m_Envelope);
                    const float gain = DbToLinear(m_Envelope) * makeup;
                    const float wet = mix * gain + (1.0f - mix);
                    left[n] *= wet;
                    right[n] *= wet;
                }
                m_GainReduction.store(-deepest, std::memory_order_relaxed);
            }
            void Reset() override
            {
                m_Envelope = 0.0f;
                m_Power = 0.0f;
            }

        private:
            float m_Envelope = 0.0f; // the smoothed gain change, dB
            float m_Power = 0.0f;
            GainRamp m_Makeup, m_Mix;
        };

        // ---- Limiter ----------------------------------------------------------------------------------------------------------

        class LimiterEffect final : public AudioEffect
        {
            enum { InputGain, Ceiling, Release, Lookahead };
            static constexpr float kMaxLookahead = 0.010f;

        public:
            explicit LimiterEffect(float sampleRate) : AudioEffect(AudioEffectType::Limiter, sampleRate)
            {
                m_Capacity = static_cast<uint32_t>(std::ceil(kMaxLookahead * sampleRate)) + 1;
                m_Delay[0].Resize(m_Capacity + 1);
                m_Delay[1].Resize(m_Capacity + 1);
                m_RingSize = m_Capacity + 1; // (the window's frames plus the one just pushed)
                m_MinValues.resize(m_RingSize);
                m_MinIndices.resize(m_RingSize);
                m_Box.resize(m_Capacity);
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const uint32_t window = std::clamp(static_cast<uint32_t>(std::lround(Value(Lookahead) * 0.001f * m_SampleRate)), 1u, m_Capacity);
                if (window != m_Window)
                {
                    m_Window = window;
                    Reset();
                }
                const float ceiling = DbToLinear(std::min(Value(Ceiling), 0.0f));
                const float release = SmoothingCoefficient(std::max(Value(Release), 1.0f) * 0.001f, m_SampleRate);
                float input;
                const float inputStep = m_Input.Begin(Value(InputGain), input, frames);
                float lowest = 1.0f;
                for (uint32_t n = 0; n < frames; ++n)
                {
                    input += inputStep;
                    const float l = left[n] * input;
                    const float r = right[n] * input;
                    m_Delay[0].Push(l);
                    m_Delay[1].Push(r);
                    const float peak = std::max(std::fabs(l), std::fabs(r));
                    const float needed = peak > ceiling ? ceiling / peak : 1.0f;

                    // Sliding minimum of the needed gain over the last `window` frames (a monotonic deque in a ring).
                    const uint64_t index = m_Counter++;
                    while (m_MinCount > 0 && m_MinValues[(m_MinHead + m_MinCount - 1) % m_RingSize] >= needed)
                        --m_MinCount;
                    m_MinValues[(m_MinHead + m_MinCount) % m_RingSize] = needed;
                    m_MinIndices[(m_MinHead + m_MinCount) % m_RingSize] = index;
                    ++m_MinCount;
                    while (m_MinIndices[m_MinHead] + m_Window <= index)
                    {
                        m_MinHead = (m_MinHead + 1) % m_RingSize;
                        --m_MinCount;
                    }
                    const float held = m_MinValues[m_MinHead];

                    // Release: rising back toward 1 exponentially, never above the held minimum.
                    m_Envelope = std::min(held, release * m_Envelope + (1.0f - release));
                    // Box filter of the window's length: smooth, and down before the peak leaves the delay.
                    m_Sum += static_cast<double>(m_Envelope) - static_cast<double>(m_Box[m_BoxPosition]);
                    m_Box[m_BoxPosition] = m_Envelope;
                    m_BoxPosition = (m_BoxPosition + 1) % m_Window;
                    const float gain = std::min(1.0f, static_cast<float>(m_Sum / static_cast<double>(m_Window)));
                    lowest = std::min(lowest, gain);
                    left[n] = m_Delay[0].Tap(m_Window) * gain;
                    right[n] = m_Delay[1].Tap(m_Window) * gain;
                }
                m_GainReduction.store(-LinearToDb(lowest), std::memory_order_relaxed);
            }
            void Reset() override
            {
                m_Delay[0].Reset();
                m_Delay[1].Reset();
                m_MinHead = m_MinCount = 0;
                m_Counter = 0;
                std::fill(m_Box.begin(), m_Box.end(), 1.0f);
                m_Sum = static_cast<double>(m_Window);
                m_BoxPosition = 0;
                m_Envelope = 1.0f;
            }

        private:
            uint32_t m_Capacity = 1;
            uint32_t m_RingSize = 2;
            uint32_t m_Window = 0;
            DelayLine m_Delay[2];
            std::vector<float> m_MinValues;
            std::vector<uint64_t> m_MinIndices;
            uint32_t m_MinHead = 0, m_MinCount = 0;
            uint64_t m_Counter = 0;
            std::vector<float> m_Box;
            uint32_t m_BoxPosition = 0;
            double m_Sum = 0.0;
            float m_Envelope = 1.0f;
            GainRamp m_Input;
        };

        // ---- Gate -------------------------------------------------------------------------------------------------------------

        class GateEffect final : public AudioEffect
        {
            enum { Threshold, Attack, Hold, Release, Range };
            static constexpr float kHysteresisDb = 4.0f; // closes this far below the threshold (no chatter around it)

        public:
            explicit GateEffect(float sampleRate) : AudioEffect(AudioEffectType::Gate, sampleRate) {}

            void Process(float* left, float* right, uint32_t frames, const float* keyLeft, const float* keyRight) override
            {
                const float open = DbToLinear(Value(Threshold));
                const float close = DbToLinear(Value(Threshold) - kHysteresisDb);
                const float attack = SmoothingCoefficient(Value(Attack) * 0.001f, m_SampleRate);
                const float release = SmoothingCoefficient(Value(Release) * 0.001f, m_SampleRate);
                const float detectorRelease = SmoothingCoefficient(0.005f, m_SampleRate);
                const uint32_t hold = static_cast<uint32_t>(std::max(Value(Hold), 0.0f) * 0.001f * m_SampleRate);
                const float floor = DbToGain(Value(Range));
                const float* kl = keyLeft ? keyLeft : left;
                const float* kr = keyRight ? keyRight : (keyLeft ? keyLeft : right);
                float lowest = 1.0f;
                for (uint32_t n = 0; n < frames; ++n)
                {
                    const float level = std::max(std::fabs(kl[n]), std::fabs(kr[n]));
                    m_Detector = level > m_Detector ? level : detectorRelease * m_Detector + (1.0f - detectorRelease) * level;
                    if (m_Detector >= open)
                    {
                        m_Open = true;
                        m_HoldLeft = hold;
                    }
                    else if (m_Detector < close)
                    {
                        if (m_HoldLeft > 0)
                            --m_HoldLeft;
                        else
                            m_Open = false;
                    }
                    const float target = m_Open ? 1.0f : floor;
                    const float coefficient = target > m_Gain ? attack : release;
                    m_Gain = target + (m_Gain - target) * coefficient;
                    lowest = std::min(lowest, m_Gain);
                    left[n] *= m_Gain;
                    right[n] *= m_Gain;
                }
                m_GainReduction.store(-LinearToDb(lowest), std::memory_order_relaxed);
            }
            void Reset() override
            {
                m_Detector = 0.0f;
                m_Gain = 0.0f;
                m_Open = false;
                m_HoldLeft = 0;
            }

        private:
            float m_Detector = 0.0f;
            float m_Gain = 0.0f;
            bool m_Open = false;
            uint32_t m_HoldLeft = 0;
        };

        // ---- Delay ------------------------------------------------------------------------------------------------------------

        class DelayEffect final : public AudioEffect
        {
            enum { Mode, TimeLeft, TimeRight, Feedback, Damping, Wet, Dry };

        public:
            explicit DelayEffect(float sampleRate) : AudioEffect(AudioEffectType::Delay, sampleRate)
            {
                const uint32_t frames = static_cast<uint32_t>(2.0f * sampleRate) + 8;
                m_Lines[0].Resize(frames);
                m_Lines[1].Resize(frames);
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const int mode = std::clamp(static_cast<int>(Value(Mode)), 0, 2);
                const float targetL = std::clamp(Value(TimeLeft), 1.0f, 2000.0f) * 0.001f * m_SampleRate;
                const float targetR = std::clamp(Value(TimeRight), 1.0f, 2000.0f) * 0.001f * m_SampleRate;
                if (!m_Started)
                {
                    m_TimeL = targetL;
                    m_TimeR = targetR;
                    m_Started = true;
                }
                const float slew = 1.0f - SmoothingCoefficient(0.05f, m_SampleRate);
                m_Damp[0].Set(Value(Damping), m_SampleRate);
                m_Damp[1].G = m_Damp[0].G;
                float feedback, wet, dry;
                const float feedbackStep = m_Feedback.BeginLinear(std::clamp(Value(Feedback), 0.0f, 0.95f), feedback, frames);
                const float wetStep = m_Wet.Begin(Value(Wet), wet, frames);
                const float dryStep = m_Dry.Begin(Value(Dry), dry, frames);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    feedback += feedbackStep;
                    wet += wetStep;
                    dry += dryStep;
                    m_TimeL += (targetL - m_TimeL) * slew;
                    m_TimeR += (targetR - m_TimeR) * slew;
                    const float echoL = m_Lines[0].Read(m_TimeL);
                    const float echoR = m_Lines[1].Read(m_TimeR);
                    const float backL = m_Damp[0].Process(echoL) * feedback;
                    const float backR = m_Damp[1].Process(echoR) * feedback;
                    const float inL = left[n], inR = right[n];
                    switch (mode)
                    {
                    case 0: m_Lines[0].Push(inL + backL); m_Lines[1].Push(inR + backR); break;
                    case 1: m_Lines[0].Push(0.5f * (inL + inR) + backR); m_Lines[1].Push(backL); break; // ping pong: in on the left, bouncing
                    default: m_Lines[0].Push(inL + backR); m_Lines[1].Push(inR + backL); break;          // cross: each side feeds the other
                    }
                    left[n] = inL * dry + echoL * wet;
                    right[n] = inR * dry + echoR * wet;
                }
            }
            void Reset() override
            {
                m_Lines[0].Reset();
                m_Lines[1].Reset();
                m_Damp[0].Reset();
                m_Damp[1].Reset();
            }

        private:
            DelayLine m_Lines[2];
            OnePoleLowPass m_Damp[2];
            float m_TimeL = 0.0f, m_TimeR = 0.0f;
            bool m_Started = false;
            GainRamp m_Feedback, m_Wet, m_Dry;
        };

        // ---- Reverb (feedback delay network) ----------------------------------------------------------------------------------

        class ReverbEffect final : public AudioEffect
        {
            enum { PreDelay, DecayTime, HFRatio, RoomSize, Diffusion, EarlyLevel, Width, Wet, Dry };
            static constexpr uint32_t kLines = 8;
            static constexpr uint32_t kDiffusers = 4;
            static constexpr uint32_t kTaps = 8;
            // Line lengths at scale 1 (ms): spread over a 2.5x range, no common factors (no reinforced modes).
            static constexpr float kLineMs[kLines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.5f, 59.3f, 67.1f, 73.3f };
            static constexpr float kModulationHz[kLines] = { 0.31f, 0.47f, 0.59f, 0.71f, 0.83f, 0.97f, 1.09f, 1.23f };
            static constexpr float kDiffuserMs[2][kDiffusers] = { { 4.77f, 3.59f, 12.73f, 9.31f }, { 4.93f, 3.71f, 12.29f, 9.67f } };
            static constexpr float kTapMs[2][kTaps] = { { 3.1f, 7.9f, 11.3f, 17.7f, 23.1f, 29.9f, 37.3f, 43.9f }, { 4.3f, 8.9f, 13.1f, 16.3f, 24.7f, 31.1f, 35.9f, 45.1f } };
            static constexpr float kTapGain[kTaps] = { 0.84f, 0.72f, 0.60f, 0.52f, 0.43f, 0.36f, 0.30f, 0.25f };
            static constexpr float kMaxScale = 2.0f;
            static constexpr float kModulationMs = 0.3f;
            static constexpr float kHighReferenceHz = 5000.0f;

            static float Scale(float roomSize) { return 0.4f + 1.6f * std::clamp(roomSize, 0.0f, 1.0f); }

        public:
            explicit ReverbEffect(float sampleRate) : AudioEffect(AudioEffectType::Reverb, sampleRate)
            {
                const float msToFrames = sampleRate * 0.001f;
                for (uint32_t c = 0; c < 2; ++c)
                {
                    m_PreDelay[c].Resize(static_cast<uint32_t>((200.0f + 46.0f * kMaxScale) * msToFrames) + 8);
                    for (uint32_t d = 0; d < kDiffusers; ++d)
                    {
                        m_DiffuserLength[c][d] = std::max<uint32_t>(1, static_cast<uint32_t>(kDiffuserMs[c][d] * msToFrames));
                        m_Diffusers[c][d].Resize(m_DiffuserLength[c][d] + 1);
                    }
                }
                for (uint32_t i = 0; i < kLines; ++i)
                {
                    m_Lines[i].Resize(static_cast<uint32_t>((kLineMs[i] * kMaxScale + 2.0f * kModulationMs) * msToFrames) + 8);
                    const float phase = kTwoPi * static_cast<float>(i) / static_cast<float>(kLines);
                    m_Phasor[i] = { std::cos(phase), std::sin(phase) };
                    const float w = kTwoPi * kModulationHz[i] / sampleRate;
                    m_Rotation[i] = { std::cos(w), std::sin(w) };
                }
                m_TapNorm = 0.0f;
                for (float gain : kTapGain)
                    m_TapNorm += gain * gain;
                m_TapNorm = 1.0f / std::sqrt(m_TapNorm);
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const float msToFrames = m_SampleRate * 0.001f;
                const float k = m_Started ? BlockSmoothing(frames, m_SampleRate) : 1.0f;
                if (!m_Started)
                {
                    m_Scale = Scale(Value(RoomSize));
                    m_PreDelayFrames = std::clamp(Value(PreDelay), 0.0f, 200.0f) * msToFrames;
                }
                m_Started = true;
                SmoothTowards(m_Scale, Scale(Value(RoomSize)), k, false);
                SmoothTowards(m_PreDelayFrames, std::clamp(Value(PreDelay), 0.0f, 200.0f) * msToFrames, k, false);
                const float decay = std::max(Value(DecayTime), 0.05f);
                const float decayHigh = decay * std::clamp(Value(HFRatio), 0.05f, 1.0f);
                const float diffusion = 0.7f * std::clamp(Value(Diffusion), 0.0f, 1.0f);
                const float width = std::clamp(Value(Width), 0.0f, 1.0f);

                // Jot's absorption filters: each line loses 60 dB in Decay Time at low frequencies and in Decay Time x HF Ratio at the
                // high-frequency reference, 5 kHz (I3DL2's HF Reference, what Unity's and Wwise's reverbs use). A one-pole low pass
                // b0 / (1 - a1 z^-1) with gain `low` at DC and `high` at the reference: (1 - a1)^2 = r (1 - 2 a1 cos w + a1^2).
                const float cosine = std::cos(kTwoPi * std::min(kHighReferenceHz, 0.45f * m_SampleRate) / m_SampleRate);
                float lengths[kLines];
                float meanSquareGain = 0.0f;
                for (uint32_t i = 0; i < kLines; ++i)
                {
                    lengths[i] = kLineMs[i] * m_Scale * msToFrames;
                    const float low = std::pow(10.0f, -3.0f * lengths[i] / (decay * m_SampleRate));
                    const float high = std::pow(10.0f, -3.0f * lengths[i] / (decayHigh * m_SampleRate));
                    const float r = (high * high) / (low * low);
                    float a1 = 0.0f;
                    if (r < 0.9999f)
                    {
                        const float b = 1.0f - r * cosine;
                        a1 = (b - std::sqrt(std::max(b * b - (1.0f - r) * (1.0f - r), 0.0f))) / (1.0f - r);
                    }
                    m_A1[i] = a1;
                    m_B0[i] = low * (1.0f - a1);
                    meanSquareGain += low * low;
                }
                meanSquareGain /= static_cast<float>(kLines);
                // The tail's energy grows as 1 / (1 - g^2): fed by sqrt(1 - g^2), a long and a short decay sound as loud.
                const float inputGain = std::sqrt(std::max(1.0f - meanSquareGain, 1e-4f));
                const float modulation = kModulationMs * msToFrames;

                float wet, dry, early;
                const float wetStep = m_Wet.Begin(Value(Wet), wet, frames);
                const float dryStep = m_Dry.Begin(Value(Dry), dry, frames);
                const float earlyStep = m_Early.Begin(Value(EarlyLevel), early, frames);
                float tapFrames[2][kTaps];
                for (uint32_t c = 0; c < 2; ++c)
                    for (uint32_t t = 0; t < kTaps; ++t)
                        tapFrames[c][t] = m_PreDelayFrames + kTapMs[c][t] * m_Scale * msToFrames;

                for (uint32_t n = 0; n < frames; ++n)
                {
                    wet += wetStep;
                    dry += dryStep;
                    early += earlyStep;
                    const float inL = left[n], inR = right[n];
                    m_PreDelay[0].Push(inL);
                    m_PreDelay[1].Push(inR);

                    // Early reflections: taps off the pre-delayed input.
                    float earlyOut[2] = { 0.0f, 0.0f };
                    for (uint32_t c = 0; c < 2; ++c)
                        for (uint32_t t = 0; t < kTaps; ++t)
                            earlyOut[c] += m_PreDelay[c].Read(tapFrames[c][t]) * kTapGain[t];

                    // Input diffusion: Schroeder all-passes in series, per side.
                    float diffused[2];
                    for (uint32_t c = 0; c < 2; ++c)
                    {
                        float x = m_PreDelay[c].Read(std::max(m_PreDelayFrames, 1.0f));
                        for (uint32_t d = 0; d < kDiffusers; ++d)
                        {
                            const float delayed = m_Diffusers[c][d].Tap(m_DiffuserLength[c][d]);
                            const float v = x + diffusion * delayed;
                            m_Diffusers[c][d].Push(v);
                            x = delayed - diffusion * v;
                        }
                        diffused[c] = x * inputGain;
                    }

                    // The network: read (modulated), absorb, Householder mix, feed back with the input.
                    float y[kLines];
                    float sum = 0.0f;
                    for (uint32_t i = 0; i < kLines; ++i)
                    {
                        const float delay = lengths[i] + modulation * (1.0f + m_Phasor[i].imag());
                        const float x = m_Lines[i].Read(delay);
                        m_Filter[i] = m_B0[i] * x + m_A1[i] * m_Filter[i];
                        y[i] = m_Filter[i];
                        sum += y[i];
                        m_Phasor[i] *= m_Rotation[i];
                    }
                    const float householder = sum * (2.0f / static_cast<float>(kLines));
                    float outL = 0.0f, outR = 0.0f;
                    for (uint32_t i = 0; i < kLines; ++i)
                    {
                        const float injected = (i & 1u) ? diffused[1] : diffused[0];
                        const float sign = (i & 2u) ? -1.0f : 1.0f;
                        m_Lines[i].Push(y[i] - householder + sign * injected);
                        outL += (i & 1u) ? -y[i] : y[i];  // two orthogonal sign patterns: decorrelated left and right
                        outR += (i & 2u) ? -y[i] : y[i];
                    }
                    outL *= 0.5f;
                    outR *= 0.5f;
                    float l = outL + earlyOut[0] * m_TapNorm * early;
                    float r = outR + earlyOut[1] * m_TapNorm * early;
                    const float mid = 0.5f * (l + r);
                    const float side = 0.5f * (l - r) * width;
                    l = mid + side;
                    r = mid - side;
                    left[n] = inL * dry + l * wet;
                    right[n] = inR * dry + r * wet;
                }
                // (keep the phasors on the unit circle)
                for (Complex& phasor : m_Phasor)
                    phasor /= std::abs(phasor);
            }
            void Reset() override
            {
                for (uint32_t c = 0; c < 2; ++c)
                {
                    m_PreDelay[c].Reset();
                    for (DelayLine& diffuser : m_Diffusers[c])
                        diffuser.Reset();
                }
                for (uint32_t i = 0; i < kLines; ++i)
                {
                    m_Lines[i].Reset();
                    m_Filter[i] = 0.0f;
                }
            }

        private:
            DelayLine m_PreDelay[2];
            DelayLine m_Diffusers[2][kDiffusers];
            uint32_t m_DiffuserLength[2][kDiffusers] = {};
            DelayLine m_Lines[kLines];
            float m_Filter[kLines] = {};
            float m_B0[kLines] = {};
            float m_A1[kLines] = {};
            Complex m_Phasor[kLines];
            Complex m_Rotation[kLines];
            float m_TapNorm = 1.0f;
            float m_Scale = 1.0f;
            float m_PreDelayFrames = 0.0f;
            bool m_Started = false;
            GainRamp m_Wet, m_Dry, m_Early;
        };

        // ---- Convolution reverb -----------------------------------------------------------------------------------------------

        // A clip's samples at `sampleRate`, as up to two channels: windowed-sinc resampling (offline, at creation).
        std::vector<std::vector<float>> PrepareImpulse(const AudioClip& clip, float sampleRate, float maxSeconds)
        {
            std::vector<float> interleaved;
            const uint32_t channels = std::max<uint32_t>(clip.GetChannels(), 1);
            if (clip.IsDecoded())
                interleaved = clip.GetSamples();
            else
            {
                AudioClipDecoder decoder;
                if (!decoder.Open(Ref<AudioClip>(const_cast<AudioClip*>(&clip))))
                    return {};
                interleaved.resize(static_cast<size_t>(clip.GetFrameCount()) * channels);
                const uint64_t read = decoder.Read(interleaved.data(), clip.GetFrameCount());
                interleaved.resize(static_cast<size_t>(read) * channels);
            }
            const uint64_t sourceFrames = interleaved.size() / channels;
            const uint32_t used = std::min<uint32_t>(channels, 2);
            const double ratio = static_cast<double>(clip.GetSampleRate()) / static_cast<double>(sampleRate); // source frames per output frame
            uint64_t outFrames = static_cast<uint64_t>(std::floor(static_cast<double>(sourceFrames) / ratio));
            const uint64_t limit = static_cast<uint64_t>(maxSeconds * sampleRate);
            if (outFrames > limit)
            {
                NOX_CORE_WARN("Audio: the impulse response {} is longer than {:.0f} s; the rest is not used", clip.GetPath().generic_string(), maxSeconds);
                outFrames = limit;
            }
            std::vector<std::vector<float>> result(used, std::vector<float>(outFrames, 0.0f));
            if (std::fabs(ratio - 1.0) < 1e-9)
            {
                for (uint32_t c = 0; c < used; ++c)
                    for (uint64_t i = 0; i < outFrames; ++i)
                        result[c][i] = interleaved[i * channels + c];
                return result;
            }
            constexpr int kHalfWidth = 16;
            const double cutoff = std::min(1.0, 1.0 / ratio); // below both Nyquists
            for (uint64_t i = 0; i < outFrames; ++i)
            {
                const double position = static_cast<double>(i) * ratio;
                const int64_t centre = static_cast<int64_t>(std::floor(position));
                for (uint32_t c = 0; c < used; ++c)
                {
                    double sum = 0.0;
                    for (int64_t k = centre - kHalfWidth + 1; k <= centre + kHalfWidth; ++k)
                    {
                        if (k < 0 || k >= static_cast<int64_t>(sourceFrames))
                            continue;
                        const double x = position - static_cast<double>(k);
                        const double t = x * cutoff;
                        const double sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(3.14159265358979323846 * t) / (3.14159265358979323846 * t);
                        const double window = 0.5 + 0.5 * std::cos(3.14159265358979323846 * x / kHalfWidth); // Hann
                        sum += interleaved[static_cast<size_t>(k) * channels + c] * sinc * window * cutoff;
                    }
                    result[c][i] = static_cast<float>(sum);
                }
            }
            return result;
        }

        class ConvolutionEffect final : public AudioEffect
        {
            enum { PreDelay, Wet, Dry, Normalize };
            static constexpr uint32_t kPartition = kMax;
            static constexpr uint32_t kSize = 2 * kPartition;   // FFT size
            static constexpr uint32_t kBins = kPartition + 1;   // a real signal's spectrum, 0 .. Nyquist

        public:
            ConvolutionEffect(float sampleRate, const AudioClip* impulse) : AudioEffect(AudioEffectType::ConvolutionReverb, sampleRate)
            {
                m_Fft.Init(kSize);
                m_PreDelay[0].Resize(static_cast<uint32_t>(0.2f * sampleRate) + 8);
                m_PreDelay[1].Resize(static_cast<uint32_t>(0.2f * sampleRate) + 8);
                m_Work.resize(kSize);
                m_History[0].assign(kSize, 0.0f);
                m_History[1].assign(kSize, 0.0f);
                if (!impulse || impulse->GetFrameCount() == 0)
                    return;
                const std::vector<std::vector<float>> channels = PrepareImpulse(*impulse, sampleRate, 10.0f);
                if (channels.empty() || channels[0].empty())
                    return;
                const size_t frames = channels[0].size();
                m_Partitions = static_cast<uint32_t>((frames + kPartition - 1) / kPartition);
                // Normalized to unit energy (white noise in, the same loudness out), as Unreal's Normalize option.
                double energy = 0.0;
                for (const std::vector<float>& channel : channels)
                {
                    double e = 0.0;
                    for (float x : channel)
                        e += static_cast<double>(x) * x;
                    energy = std::max(energy, e);
                }
                m_NormalizeGain = energy > 0.0 ? static_cast<float>(1.0 / std::sqrt(energy)) : 1.0f;
                m_Impulse.assign(static_cast<size_t>(m_Partitions) * kBins * 2, Complex());
                for (uint32_t c = 0; c < 2; ++c)
                {
                    const std::vector<float>& source = channels[std::min<size_t>(c, channels.size() - 1)];
                    for (uint32_t p = 0; p < m_Partitions; ++p)
                    {
                        std::fill(m_Work.begin(), m_Work.end(), Complex());
                        for (uint32_t i = 0; i < kPartition; ++i)
                        {
                            const size_t index = static_cast<size_t>(p) * kPartition + i;
                            if (index < frames)
                                m_Work[i] = source[index];
                        }
                        m_Fft.Forward(m_Work.data());
                        std::copy(m_Work.begin(), m_Work.begin() + kBins, m_Impulse.begin() + Offset(p, c));
                    }
                }
                m_Spectra.assign(static_cast<size_t>(m_Partitions) * kBins * 2, Complex());
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                float wet, dry;
                const float normalize = Value(Normalize) >= 0.5f ? m_NormalizeGain : 1.0f;
                const float wetStep = m_Wet.BeginLinear(DbToGain(Value(Wet)) * normalize, wet, frames);
                const float dryStep = m_Dry.Begin(Value(Dry), dry, frames);
                if (m_Partitions == 0 || frames != kPartition)
                {
                    for (uint32_t n = 0; n < frames; ++n)
                    {
                        dry += dryStep;
                        left[n] *= dry;
                        right[n] *= dry;
                    }
                    return;
                }
                const float preDelay = std::max(std::clamp(Value(PreDelay), 0.0f, 200.0f) * 0.001f * m_SampleRate, 0.0f);
                const uint32_t preDelayFrames = static_cast<uint32_t>(preDelay);

                // Overlap-save: the last two blocks of (pre-delayed) input, left in the real part and right in the imaginary part.
                for (uint32_t c = 0; c < 2; ++c)
                    std::memmove(m_History[c].data(), m_History[c].data() + kPartition, sizeof(float) * kPartition);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    m_PreDelay[0].Push(left[n]);
                    m_PreDelay[1].Push(right[n]);
                    m_History[0][kPartition + n] = preDelayFrames == 0 ? left[n] : m_PreDelay[0].Tap(preDelayFrames + 1);
                    m_History[1][kPartition + n] = preDelayFrames == 0 ? right[n] : m_PreDelay[1].Tap(preDelayFrames + 1);
                }
                for (uint32_t i = 0; i < kSize; ++i)
                    m_Work[i] = { m_History[0][i], m_History[1][i] };
                m_Fft.Forward(m_Work.data());

                // The two real spectra out of the packed one, into the frequency-domain delay line's newest slot.
                m_Head = (m_Head + m_Partitions - 1) % m_Partitions;
                Complex* newestL = m_Spectra.data() + Offset(m_Head, 0);
                Complex* newestR = m_Spectra.data() + Offset(m_Head, 1);
                for (uint32_t k = 0; k < kBins; ++k)
                {
                    const Complex z = m_Work[k];
                    const Complex mirror = std::conj(m_Work[(kSize - k) % kSize]);
                    newestL[k] = 0.5f * (z + mirror);
                    newestR[k] = Complex(0.0f, -0.5f) * (z - mirror);
                }

                // Every partition of the impulse times the input spectrum from that many blocks ago.
                std::array<Complex, kBins> sumL{}, sumR{};
                for (uint32_t p = 0; p < m_Partitions; ++p)
                {
                    const uint32_t slot = (m_Head + p) % m_Partitions;
                    const Complex* xl = m_Spectra.data() + Offset(slot, 0);
                    const Complex* xr = m_Spectra.data() + Offset(slot, 1);
                    const Complex* hl = m_Impulse.data() + Offset(p, 0);
                    const Complex* hr = m_Impulse.data() + Offset(p, 1);
                    for (uint32_t k = 0; k < kBins; ++k)
                    {
                        sumL[k] += xl[k] * hl[k];
                        sumR[k] += xr[k] * hr[k];
                    }
                }
                // Both outputs back in one inverse transform: left real, right imaginary (each spectrum is Hermitian).
                for (uint32_t k = 0; k < kBins; ++k)
                    m_Work[k] = sumL[k] + Complex(0.0f, 1.0f) * sumR[k];
                for (uint32_t k = kBins; k < kSize; ++k)
                    m_Work[k] = std::conj(sumL[kSize - k]) + Complex(0.0f, 1.0f) * std::conj(sumR[kSize - k]);
                m_Fft.Inverse(m_Work.data());
                const float scale = 1.0f / static_cast<float>(kSize);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    wet += wetStep;
                    dry += dryStep;
                    const Complex y = m_Work[kPartition + n] * scale;
                    left[n] = left[n] * dry + y.real() * wet;
                    right[n] = right[n] * dry + y.imag() * wet;
                }
            }
            void Reset() override
            {
                std::fill(m_Spectra.begin(), m_Spectra.end(), Complex());
                std::fill(m_History[0].begin(), m_History[0].end(), 0.0f);
                std::fill(m_History[1].begin(), m_History[1].end(), 0.0f);
                m_PreDelay[0].Reset();
                m_PreDelay[1].Reset();
            }

        private:
            size_t Offset(uint32_t partition, uint32_t channel) const { return (static_cast<size_t>(partition) * 2 + channel) * kBins; }

            FFT m_Fft;
            uint32_t m_Partitions = 0;
            uint32_t m_Head = 0;
            float m_NormalizeGain = 1.0f;
            std::vector<Complex> m_Impulse; // per partition: left spectrum, right spectrum
            std::vector<Complex> m_Spectra; // the input's spectra, the newest at m_Head
            std::vector<Complex> m_Work;
            std::vector<float> m_History[2];
            DelayLine m_PreDelay[2];
            GainRamp m_Wet, m_Dry;
        };

        // ---- Chorus / Flanger -------------------------------------------------------------------------------------------------

        class ModulatedDelayEffect final : public AudioEffect
        {
            enum { Rate, Depth, Delay, Feedback, Spread, Mix };

        public:
            ModulatedDelayEffect(AudioEffectType type, float sampleRate) : AudioEffect(type, sampleRate)
            {
                m_Voices = type == AudioEffectType::Chorus ? 3u : 1u;
                const float maxMs = type == AudioEffectType::Chorus ? 60.0f : 20.0f;
                m_Lines[0].Resize(static_cast<uint32_t>(maxMs * 0.001f * sampleRate) + 8);
                m_Lines[1].Resize(static_cast<uint32_t>(maxMs * 0.001f * sampleRate) + 8);
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const float msToFrames = 0.001f * m_SampleRate;
                const float depth = std::max(Value(Depth), 0.0f) * msToFrames;
                const float base = std::max(Value(Delay), 0.05f) * msToFrames;
                const float increment = std::max(Value(Rate), 0.0f) / m_SampleRate;
                const float spread = std::clamp(Value(Spread), 0.0f, 1.0f) * 0.25f; // up to a quarter cycle between the sides
                float feedback, mix;
                const float feedbackStep = m_Feedback.BeginLinear(std::clamp(Value(Feedback), -0.95f, 0.95f), feedback, frames);
                const float mixStep = m_Mix.BeginLinear(std::clamp(Value(Mix), 0.0f, 1.0f), mix, frames);
                const float voiceGain = 1.0f / static_cast<float>(m_Voices);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    feedback += feedbackStep;
                    mix += mixStep;
                    float* io[2] = { &left[n], &right[n] };
                    for (uint32_t c = 0; c < 2; ++c)
                    {
                        float wet = 0.0f;
                        for (uint32_t v = 0; v < m_Voices; ++v)
                        {
                            float phase = m_Phase + (c == 1 ? spread : 0.0f) + static_cast<float>(v) / static_cast<float>(m_Voices);
                            phase -= std::floor(phase);
                            const float lfo = 0.5f + 0.5f * std::sin(kTwoPi * phase);
                            wet += m_Lines[c].Read(base + depth * lfo);
                        }
                        wet *= voiceGain;
                        const float dry = *io[c];
                        m_Lines[c].Push(dry + wet * feedback);
                        *io[c] = dry * (1.0f - mix) + wet * mix;
                    }
                    m_Phase += increment;
                    m_Phase -= std::floor(m_Phase);
                }
            }
            void Reset() override
            {
                m_Lines[0].Reset();
                m_Lines[1].Reset();
            }

        private:
            DelayLine m_Lines[2];
            uint32_t m_Voices = 1;
            float m_Phase = 0.0f;
            GainRamp m_Feedback, m_Mix;
        };

        // ---- Phaser -----------------------------------------------------------------------------------------------------------

        class PhaserEffect final : public AudioEffect
        {
            enum { Rate, Depth, Frequency, Feedback, Stages, Spread, Mix };
            static constexpr uint32_t kMaxStages = 12;
            static constexpr float kRangeOctaves = 2.0f; // the sweep: +-2 octaves around Frequency at full depth

        public:
            explicit PhaserEffect(float sampleRate) : AudioEffect(AudioEffectType::Phaser, sampleRate) {}

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                static constexpr uint32_t kStageCounts[] = { 2, 4, 6, 8, 12 };
                const uint32_t stages = kStageCounts[std::clamp(static_cast<int>(Value(Stages)), 0, 4)];
                const float increment = std::max(Value(Rate), 0.0f) / m_SampleRate;
                const float centre = std::clamp(Value(Frequency), 20.0f, m_SampleRate * 0.45f);
                const float depth = std::clamp(Value(Depth), 0.0f, 1.0f) * kRangeOctaves;
                const float spread = std::clamp(Value(Spread), 0.0f, 1.0f) * 0.25f;
                float feedback, mix;
                const float feedbackStep = m_Feedback.BeginLinear(std::clamp(Value(Feedback), -0.95f, 0.95f), feedback, frames);
                const float mixStep = m_Mix.BeginLinear(std::clamp(Value(Mix), 0.0f, 1.0f), mix, frames);
                float coefficient[2] = {};
                for (uint32_t n = 0; n < frames; ++n)
                {
                    feedback += feedbackStep;
                    mix += mixStep;
                    if ((n & 7u) == 0) // the sweep's coefficients every 8 frames
                    {
                        for (uint32_t c = 0; c < 2; ++c)
                        {
                            float phase = m_Phase + (c == 1 ? spread : 0.0f);
                            phase -= std::floor(phase);
                            const float frequency = std::min(centre * std::exp2(depth * std::sin(kTwoPi * phase)), m_SampleRate * 0.45f);
                            const float t = std::tan(kPi * frequency / m_SampleRate);
                            coefficient[c] = (t - 1.0f) / (t + 1.0f);
                        }
                    }
                    float* io[2] = { &left[n], &right[n] };
                    for (uint32_t c = 0; c < 2; ++c)
                    {
                        float x = *io[c] + m_Last[c] * feedback;
                        for (uint32_t s = 0; s < stages; ++s)
                        {
                            // First-order all-pass: (a + z^-1) / (1 + a z^-1).
                            const float y = coefficient[c] * x + m_State[c][s];
                            m_State[c][s] = x - coefficient[c] * y;
                            x = y;
                        }
                        m_Last[c] = x;
                        *io[c] = *io[c] * (1.0f - mix) + x * mix;
                    }
                    m_Phase += increment;
                    m_Phase -= std::floor(m_Phase);
                }
            }
            void Reset() override
            {
                std::memset(m_State, 0, sizeof(m_State));
                m_Last[0] = m_Last[1] = 0.0f;
            }

        private:
            float m_State[2][kMaxStages] = {};
            float m_Last[2] = {};
            float m_Phase = 0.0f;
            GainRamp m_Feedback, m_Mix;
        };

        // ---- Distortion -------------------------------------------------------------------------------------------------------

        // A 31-tap half-band low pass (Kaiser-windowed sinc, beta 8): the 2x oversampler's interpolation and decimation filter.
        struct HalfBand
        {
            static constexpr int kTaps = 31;
            static constexpr int kCentre = kTaps / 2;
            float H[kTaps] = {};
            HalfBand()
            {
                auto bessel = [](double x) // I0
                {
                    double sum = 1.0, term = 1.0;
                    for (int k = 1; k < 30; ++k)
                    {
                        term *= (x / (2.0 * k)) * (x / (2.0 * k));
                        sum += term;
                    }
                    return sum;
                };
                const double beta = 8.0;
                for (int i = 0; i < kTaps; ++i)
                {
                    const int m = i - kCentre;
                    const double sinc = m == 0 ? 0.5 : std::sin(3.14159265358979323846 * m / 2.0) / (3.14159265358979323846 * m);
                    const double r = static_cast<double>(m) / kCentre;
                    const double window = bessel(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / bessel(beta);
                    H[i] = static_cast<float>(sinc * window);
                }
            }
        };
        const HalfBand& HalfBandFilter()
        {
            static const HalfBand filter;
            return filter;
        }

        class DistortionEffect final : public AudioEffect
        {
            enum { Type, Drive, Tone, Output, Mix };

        public:
            explicit DistortionEffect(float sampleRate) : AudioEffect(AudioEffectType::Distortion, sampleRate)
            {
                m_Dc[0].Prepare(sampleRate);
                m_Dc[1].Prepare(sampleRate);
            }

            static float Shape(int type, float x)
            {
                switch (type)
                {
                case 0: return std::tanh(x);
                case 1: return std::clamp(x, -1.0f, 1.0f);
                case 2: return std::sin(x);                            // a sine wavefolder: folds back past full scale
                default: return std::tanh(x + 0.3f) - 0.29131261f;     // tube: asymmetric (even harmonics), tanh(0.3) taken off
                }
            }

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const HalfBand& filter = HalfBandFilter();
                const int type = std::clamp(static_cast<int>(Value(Type)), 0, 3);
                float tone, toneTo;
                m_Tone.Begin(Value(Tone), m_SampleRate, tone, toneTo);
                float drive, output, mix;
                const float driveStep = m_Drive.Begin(std::max(Value(Drive), 0.0f), drive, frames);
                const float outputStep = m_Output.Begin(Value(Output), output, frames);
                const float mixStep = m_Mix.BeginLinear(std::clamp(Value(Mix), 0.0f, 1.0f), mix, frames);
                for (uint32_t c = 0; c < 2; ++c)
                {
                    float* io = c == 0 ? left : right;
                    float d = drive, o = output, m = mix;
                    for (uint32_t start = 0; start < frames; start += kCoefficientStep)
                    {
                        const uint32_t end = std::min(frames, start + kCoefficientStep);
                        m_ToneFilter[c].Set(BiquadType::LowPass, FrequencyRamp::At(tone, toneTo, static_cast<float>(end) / static_cast<float>(frames)), 0.707f, 0.0f, m_SampleRate);
                        for (uint32_t n = start; n < end; ++n)
                        {
                            d += driveStep;
                            o += outputStep;
                            m += mixStep;
                            const float dry = io[n];
                            // Up: zero-stuffed, filtered (x2 gain); shaped at 2x; down: filtered, every other sample kept.
                            m_Up[c][m_UpPosition[c]] = dry;
                            float shaped[2];
                            for (int phase = 0; phase < 2; ++phase)
                            {
                                float u = 0.0f;
                                for (int k = phase; k < HalfBand::kTaps; k += 2)
                                    u += filter.H[k] * m_Up[c][(m_UpPosition[c] + kUpSize - (k - phase) / 2) % kUpSize];
                                shaped[phase] = Shape(type, 2.0f * u * d);
                            }
                            m_UpPosition[c] = (m_UpPosition[c] + 1) % kUpSize;
                            for (int phase = 0; phase < 2; ++phase)
                            {
                                m_Down[c][m_DownPosition[c]] = shaped[phase];
                                m_DownPosition[c] = (m_DownPosition[c] + 1) % HalfBand::kTaps;
                            }
                            float y = 0.0f;
                            for (int k = 0; k < HalfBand::kTaps; ++k)
                                y += filter.H[k] * m_Down[c][(m_DownPosition[c] + HalfBand::kTaps - 1 - k) % HalfBand::kTaps];
                            y = m_ToneFilter[c].Process(m_Dc[c].Process(y)) * o;
                            io[n] = dry * (1.0f - m) + y * m;
                        }
                    }
                }
            }
            void Reset() override
            {
                std::memset(m_Up, 0, sizeof(m_Up));
                std::memset(m_Down, 0, sizeof(m_Down));
                for (uint32_t c = 0; c < 2; ++c)
                {
                    m_Dc[c].Reset();
                    m_ToneFilter[c].Reset();
                }
            }

        private:
            static constexpr int kUpSize = HalfBand::kTaps / 2 + 1;
            float m_Up[2][kUpSize] = {};
            int m_UpPosition[2] = {};
            float m_Down[2][HalfBand::kTaps] = {};
            int m_DownPosition[2] = {};
            DcBlocker m_Dc[2];
            Biquad m_ToneFilter[2];
            FrequencyRamp m_Tone;
            GainRamp m_Drive, m_Output, m_Mix;
        };

        // ---- Bitcrusher -------------------------------------------------------------------------------------------------------

        class BitcrusherEffect final : public AudioEffect
        {
            enum { Bits, SampleRate, Mix };

        public:
            explicit BitcrusherEffect(float sampleRate) : AudioEffect(AudioEffectType::Bitcrusher, sampleRate) {}

            void Process(float* left, float* right, uint32_t frames, const float*, const float*) override
            {
                const float levels = std::exp2(std::clamp(Value(Bits), 1.0f, 24.0f) - 1.0f);
                const float increment = std::clamp(Value(SampleRate), 1.0f, m_SampleRate) / m_SampleRate;
                float mix;
                const float mixStep = m_Mix.BeginLinear(std::clamp(Value(Mix), 0.0f, 1.0f), mix, frames);
                for (uint32_t n = 0; n < frames; ++n)
                {
                    mix += mixStep;
                    if (m_Phase >= 1.0f) // a new sample every 1 / increment frames, the first at once
                    {
                        m_Phase -= std::floor(m_Phase);
                        m_Held[0] = std::round(left[n] * levels) / levels;
                        m_Held[1] = std::round(right[n] * levels) / levels;
                    }
                    m_Phase += increment;
                    left[n] = left[n] * (1.0f - mix) + m_Held[0] * mix;
                    right[n] = right[n] * (1.0f - mix) + m_Held[1] * mix;
                }
            }
            void Reset() override
            {
                m_Phase = 1.0f;
                m_Held[0] = m_Held[1] = 0.0f;
            }

        private:
            float m_Phase = 1.0f;
            float m_Held[2] = {};
            GainRamp m_Mix;
        };
    }

    AudioEffect::AudioEffect(AudioEffectType type, float sampleRate) : m_Type(type), m_SampleRate(sampleRate)
    {
        for (const AudioEffectParameterInfo& parameter : GetAudioEffectInfo(type).Parameters)
            m_Values.push_back(parameter.Default);
    }

    const AudioEffectInfo& GetAudioEffectInfo(AudioEffectType type)
    {
        const std::vector<AudioEffectInfo>& infos = Infos();
        return infos[std::min(static_cast<size_t>(type), infos.size() - 1)];
    }

    std::string_view AudioEffectTypeToString(AudioEffectType type)
    {
        return type < AudioEffectType::Count ? std::string_view(GetAudioEffectInfo(type).Name) : std::string_view();
    }

    AudioEffectType AudioEffectTypeFromString(std::string_view name)
    {
        for (const AudioEffectInfo& info : Infos())
        {
            if (info.Name == name)
                return info.Type;
        }
        return AudioEffectType::Count;
    }

    int32_t FindAudioEffectParameter(AudioEffectType type, std::string_view name)
    {
        if (type >= AudioEffectType::Count)
            return -1;
        const std::vector<AudioEffectParameterInfo>& parameters = GetAudioEffectInfo(type).Parameters;
        for (size_t i = 0; i < parameters.size(); ++i)
        {
            if (parameters[i].Name == name)
                return static_cast<int32_t>(i);
        }
        return -1;
    }

    std::unique_ptr<AudioEffect> CreateAudioEffect(AudioEffectType type, float sampleRate, const AudioClip* impulse)
    {
        std::unique_ptr<AudioEffect> effect;
        switch (type)
        {
        case AudioEffectType::EQ: effect = std::make_unique<EqEffect>(sampleRate); break;
        case AudioEffectType::Filter: effect = std::make_unique<FilterEffect>(sampleRate); break;
        case AudioEffectType::Compressor: effect = std::make_unique<CompressorEffect>(sampleRate); break;
        case AudioEffectType::Limiter: effect = std::make_unique<LimiterEffect>(sampleRate); break;
        case AudioEffectType::Gate: effect = std::make_unique<GateEffect>(sampleRate); break;
        case AudioEffectType::Delay: effect = std::make_unique<DelayEffect>(sampleRate); break;
        case AudioEffectType::Reverb: effect = std::make_unique<ReverbEffect>(sampleRate); break;
        case AudioEffectType::ConvolutionReverb: effect = std::make_unique<ConvolutionEffect>(sampleRate, impulse); break;
        case AudioEffectType::Chorus:
        case AudioEffectType::Flanger: effect = std::make_unique<ModulatedDelayEffect>(type, sampleRate); break;
        case AudioEffectType::Phaser: effect = std::make_unique<PhaserEffect>(sampleRate); break;
        case AudioEffectType::Distortion: effect = std::make_unique<DistortionEffect>(sampleRate); break;
        case AudioEffectType::Bitcrusher: effect = std::make_unique<BitcrusherEffect>(sampleRate); break;
        default: return nullptr;
        }
        effect->Reset();
        return effect;
    }
}
