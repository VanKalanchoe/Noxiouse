#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

// The building blocks the audio graph's nodes and the mixer's bus effects share (docs/Audio_Architecture_Plan_2026.md §5.2 and §7):
// filters, delay lines, ramps, an FFT. Plain structs, no allocation after Prepare / Resize (game thread), so they run on the mixer
// thread.
namespace Nox::AudioDSP
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kTwoPi = 2.0f * kPi;
    // Filters recompute their coefficients this often (frames), from parameters moving smoothly across the block.
    inline constexpr uint32_t kCoefficientStep = 32;
    // A level at or below this many dB is silence (a fader all the way down, a Wet of "off").
    inline constexpr float kSilenceDb = -80.0f;

    inline float DbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }
    inline float LinearToDb(float linear) { return 20.0f * std::log10(std::max(linear, 1e-10f)); }
    // As DbToLinear, with kSilenceDb and below exactly 0.
    inline float DbToGain(float db) { return db <= kSilenceDb ? 0.0f : DbToLinear(db); }
    // The coefficient of a one-pole smoother reaching 1 - 1/e of a step in `seconds`.
    inline float SmoothingCoefficient(float seconds, float sampleRate) { return std::exp(-1.0f / std::max(seconds * sampleRate, 1.0f)); }

    // A control value applied to audio, ramped linearly across a block from its last block's value (no zipper noise).
    struct Ramp
    {
        float Previous = 0.0f;
        bool Started = false;
        // Per-frame step from the last value to `target` over `frames`; `outStart` the first frame's value.
        float Begin(float target, float& outStart, uint32_t frames)
        {
            if (!Started)
            {
                Previous = target;
                Started = true;
            }
            outStart = Previous;
            const float step = (target - Previous) / static_cast<float>(frames);
            Previous = target;
            return step;
        }
    };

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
        void Reset() { Z1 = Z2 = 0.0f; }
    };

    // Topology-preserving one-pole low pass (Zavalishin): stays stable and in tune while its frequency moves.
    struct OnePoleLowPass
    {
        float G = 1.0f;
        float S = 0.0f;
        void Set(float frequency, float sampleRate)
        {
            const float g = std::tan(kPi * std::clamp(frequency, 1.0f, sampleRate * 0.49f) / sampleRate);
            G = g / (1.0f + g);
        }
        float Process(float x)
        {
            const float v = (x - S) * G;
            const float low = v + S;
            S = low + v;
            return low;
        }
        void Reset() { S = 0.0f; }
    };

    // Removes DC (a 10 Hz one-pole high pass): after asymmetric shaping.
    struct DcBlocker
    {
        float R = 0.9987f;
        float X1 = 0.0f, Y1 = 0.0f;
        void Prepare(float sampleRate) { R = std::exp(-kTwoPi * 10.0f / sampleRate); }
        float Process(float x)
        {
            const float y = x - X1 + R * Y1;
            X1 = x;
            Y1 = y;
            return y;
        }
        void Reset() { X1 = Y1 = 0.0f; }
    };

    // A circular delay line read at fractional delays (4-point Hermite interpolation: smooth when the delay moves -- choruses,
    // modulated reverb lines, a delay whose time is changed live).
    class DelayLine
    {
    public:
        // Game thread: room for `maxDelayFrames` (rounded up to a power of two, plus the interpolation's neighbours).
        void Resize(uint32_t maxDelayFrames)
        {
            uint32_t size = 4;
            while (size < maxDelayFrames + 4)
                size <<= 1;
            m_Buffer.assign(size, 0.0f);
            m_Mask = size - 1;
            m_Write = 0;
        }
        void Reset() { std::fill(m_Buffer.begin(), m_Buffer.end(), 0.0f); }
        uint32_t MaxDelay() const { return m_Buffer.empty() ? 0 : static_cast<uint32_t>(m_Buffer.size()) - 4; }

        void Push(float x)
        {
            m_Buffer[m_Write] = x;
            m_Write = (m_Write + 1) & m_Mask;
        }
        // The sample written `delay` frames ago (whole frames; 1 = the last one pushed).
        float Tap(uint32_t delay) const { return m_Buffer[(m_Write - delay) & m_Mask]; }
        // At a fractional delay (>= 1 frame), Hermite-interpolated.
        float Read(float delay) const
        {
            delay = std::clamp(delay, 1.0f, static_cast<float>(MaxDelay()));
            const float whole = std::floor(delay);
            const float t = delay - whole;
            const uint32_t d = static_cast<uint32_t>(whole);
            const float y0 = Tap(d - 1 == 0 ? 1 : d - 1); // (at 1 frame the newer neighbour is the last sample itself)
            const float y1 = Tap(d);
            const float y2 = Tap(d + 1);
            const float y3 = Tap(d + 2);
            // Reading backwards in time: y1 at t = 0, y2 at t = 1.
            const float c1 = 0.5f * (y2 - y0);
            const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
            return ((c3 * t + c2) * t + c1) * t + y1;
        }

    private:
        std::vector<float> m_Buffer;
        uint32_t m_Mask = 0;
        uint32_t m_Write = 0;
    };

    // In-place radix-2 complex FFT of a power-of-two size, twiddles and bit reversal precomputed (Init, game thread). Inverse is not
    // scaled (divide by the size).
    class FFT
    {
    public:
        void Init(uint32_t size)
        {
            m_Size = size;
            m_Twiddles.resize(size / 2);
            for (uint32_t i = 0; i < size / 2; ++i)
            {
                const double angle = -2.0 * 3.14159265358979323846 * static_cast<double>(i) / static_cast<double>(size);
                m_Twiddles[i] = { static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)) };
            }
            m_Reverse.resize(size);
            uint32_t bits = 0;
            while ((1u << bits) < size)
                ++bits;
            for (uint32_t i = 0; i < size; ++i)
            {
                uint32_t reversed = 0;
                for (uint32_t b = 0; b < bits; ++b)
                    reversed |= ((i >> b) & 1u) << (bits - 1 - b);
                m_Reverse[i] = reversed;
            }
        }
        uint32_t Size() const { return m_Size; }

        void Forward(std::complex<float>* data) const { Transform(data, false); }
        void Inverse(std::complex<float>* data) const { Transform(data, true); }

    private:
        void Transform(std::complex<float>* data, bool inverse) const
        {
            for (uint32_t i = 0; i < m_Size; ++i)
            {
                if (i < m_Reverse[i])
                    std::swap(data[i], data[m_Reverse[i]]);
            }
            for (uint32_t length = 2; length <= m_Size; length <<= 1)
            {
                const uint32_t half = length / 2;
                const uint32_t stride = m_Size / length;
                for (uint32_t start = 0; start < m_Size; start += length)
                {
                    for (uint32_t k = 0; k < half; ++k)
                    {
                        std::complex<float> w = m_Twiddles[k * stride];
                        if (inverse)
                            w = std::conj(w);
                        const std::complex<float> a = data[start + k];
                        const std::complex<float> b = data[start + k + half] * w;
                        data[start + k] = a + b;
                        data[start + k + half] = a - b;
                    }
                }
            }
        }

        uint32_t m_Size = 0;
        std::vector<std::complex<float>> m_Twiddles;
        std::vector<uint32_t> m_Reverse;
    };
}
