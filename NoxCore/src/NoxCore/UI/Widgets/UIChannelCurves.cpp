#include "UIChannelCurves.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    namespace
    {
        constexpr int kSamples = 160;
        constexpr float kPickRadius = 8.0f;
    }

    UIChannelCurves::UIChannelCurves()
    {
        HitTestMode = UIHitTestMode::Self;
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Fixed;
        FixedSize = { 0.0f, 150.0f };
    }

    int UIChannelCurves::Channels() const
    {
        return ChannelCount ? std::clamp(ChannelCount(), 0, 4) : 0;
    }

    UIVec2 UIChannelCurves::ToScreen(float time, float value) const
    {
        const UIRect b = GetBounds();
        return { b.Min.x + time / std::max(m_Duration, 0.01f) * b.Width(), b.Min.y + (1.0f - (value - m_Low) / std::max(m_High - m_Low, 1e-6f)) * b.Height() };
    }

    void UIChannelCurves::UpdateRange()
    {
        if (m_Key >= 0)
            return; // (held still while a key is dragged)
        const int channels = Channels();
        const int keys = KeyCount ? KeyCount() : 0;
        const float duration = std::max(Duration ? Duration() : 1.0f, 0.01f);
        float low = FLT_MAX, high = -FLT_MAX;
        if (Evaluate)
            for (int i = 0; i <= kSamples; ++i)
                for (int c = 0; c < channels; ++c)
                {
                    const float v = Evaluate(duration * static_cast<float>(i) / kSamples, c);
                    low = std::min(low, v);
                    high = std::max(high, v);
                }
        if (KeyValue)
            for (int k = 0; k < keys; ++k)
                for (int c = 0; c < channels; ++c)
                {
                    low = std::min(low, KeyValue(k, c));
                    high = std::max(high, KeyValue(k, c));
                }
        if (low > high)
            low = high = 0.0f;
        if (high - low < 1e-3f)
        {
            low -= 0.5f;
            high += 0.5f;
        }
        const float margin = (high - low) * 0.1f;
        m_Low = low - margin;
        m_High = high + margin;
        m_Duration = duration;
    }

    bool UIChannelCurves::OnPointerDown(const UIPointerEvent& e)
    {
        const int channels = Channels();
        const int keys = KeyCount ? KeyCount() : 0;
        if (e.Button != UIMouseButton::Left || channels == 0 || !KeyTime || !KeyValue)
            return false;
        UpdateRange();
        m_Key = -1;
        float best = kPickRadius;
        for (int k = 0; k < keys; ++k)
            for (int c = 0; c < channels; ++c)
            {
                const UIVec2 p = ToScreen(KeyTime(k), KeyValue(k, c));
                const float distance = std::hypot(p.x - e.Position.x, p.y - e.Position.y);
                if (distance < best)
                {
                    best = distance;
                    m_Key = k;
                    m_Channel = c;
                }
            }
        if (m_Key >= 0 && OnSelectKey)
            OnSelectKey(m_Key);
        return true;
    }

    bool UIChannelCurves::OnPointerMove(const UIPointerEvent& e)
    {
        const int keys = KeyCount ? KeyCount() : 0;
        if (m_Key < 0 || m_Key >= keys || !SetKey || !KeyTime)
            return true;
        const UIRect b = GetBounds();
        const float value = m_Low + (1.0f - (std::clamp(e.Position.y, b.Min.y, b.Max.y) - b.Min.y) / std::max(b.Height(), 1.0f)) * (m_High - m_Low);
        float time = KeyTime(m_Key);
        if (e.Modifiers & UIMod::Shift)
        {
            const float duration = std::max(Duration ? Duration() : 1.0f, 0.01f);
            time = std::clamp((e.Position.x - b.Min.x) / std::max(b.Width(), 1.0f) * duration, 0.0f, duration);
            if (SnapTime)
                time = std::clamp(SnapTime(time), 0.0f, duration);
        }
        SetKey(m_Key, m_Channel, time, value);
        if (OnChanging)
            OnChanging();
        return true;
    }

    bool UIChannelCurves::OnPointerUp(const UIPointerEvent&)
    {
        const bool dragged = m_Key >= 0;
        m_Key = -1;
        if (dragged && OnChanged)
        {
            auto onChanged = OnChanged; // (copied: it may rebuild what holds this view)
            onChanged();
        }
        return true;
    }

    void UIChannelCurves::OnPaint(UIDrawList& d, const UITheme& theme)
    {
        const UIRect b = GetBounds();
        d.AddRoundedRect(b.Min, b.Max, theme.GetToken("CurveBackground", { 0.11f, 0.11f, 0.13f, 1.0f }));
        const UIColor dim = theme.GetToken("TextMuted", { 0.55f, 0.57f, 0.62f, 1.0f });
        if (!ChannelCount || !KeyCount || ChannelCount() < 0)
        {
            d.AddText(NothingText, nullptr, { b.Min.x + 8.0f, b.Min.y + 8.0f }, 13.0f, dim);
            return;
        }
        const int channels = Channels();
        const int keys = KeyCount();
        if (channels == 0 || keys == 0)
        {
            d.AddText(channels == 0 ? NoValuesText : NoKeysText, nullptr, { b.Min.x + 8.0f, b.Min.y + 8.0f }, 13.0f, dim);
            return;
        }
        UpdateRange();
        d.PushClipRect(b.Min, b.Max);
        const UIColor colors[4] = { theme.GetToken("CurveX", { 0.9f, 0.31f, 0.31f, 1.0f }), theme.GetToken("CurveY", { 0.35f, 0.82f, 0.35f, 1.0f }),
                                    theme.GetToken("CurveZ", { 0.35f, 0.55f, 1.0f, 1.0f }), theme.GetToken("CurveW", { 0.85f, 0.85f, 0.85f, 1.0f }) };
        const UIColor single = theme.GetToken("CurveSingle", { 0.9f, 0.9f, 0.9f, 1.0f });
        const auto colorOf = [&](int c) { return channels == 1 ? single : colors[c]; };
        if (m_Low < 0.0f && m_High > 0.0f)
        {
            const UIVec2 zero = ToScreen(0.0f, 0.0f);
            d.AddRoundedRect({ b.Min.x, zero.y }, { b.Max.x, zero.y + 1.0f }, theme.GetToken("CurveZero", { 0.31f, 0.31f, 0.35f, 1.0f }));
        }
        if (Playhead)
        {
            const float x = b.Min.x + std::clamp(Playhead() / m_Duration, 0.0f, 1.0f) * b.Width();
            d.AddRoundedRect({ x - 0.75f, b.Min.y }, { x + 0.75f, b.Max.y }, theme.GetToken("CurvePlayhead", { 1.0f, 0.31f, 0.24f, 0.8f }));
        }
        if (Evaluate)
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i <= kSamples; ++i)
                {
                    const float time = m_Duration * static_cast<float>(i) / kSamples;
                    const UIVec2 p = ToScreen(time, Evaluate(time, c));
                    d.AddRoundedRect({ p.x - 1.0f, p.y - 1.0f }, { p.x + 1.0f, p.y + 1.0f }, colorOf(c));
                }
        if (KeyTime && KeyValue)
        {
            const int selectedKey = SelectedKey ? SelectedKey() : -1;
            const UIColor selectedColor = theme.GetToken("CurveKeySelected", { 1.0f, 0.78f, 0.27f, 1.0f });
            for (int k = 0; k < keys; ++k)
                for (int c = 0; c < channels; ++c)
                {
                    const UIVec2 p = ToScreen(KeyTime(k), KeyValue(k, c));
                    const bool selected = k == selectedKey;
                    const float r = selected ? 5.0f : 4.0f;
                    d.AddRoundedRect({ p.x - r, p.y - r }, { p.x + r, p.y + r }, selected ? selectedColor : colorOf(c), glm::vec4(r));
                }
        }
        char range[64];
        std::snprintf(range, sizeof(range), "%.3g .. %.3g", m_Low, m_High);
        d.AddText(range, nullptr, { b.Min.x + 4.0f, b.Min.y + 2.0f }, 11.5f, dim);
        d.PopClipRect();
    }
}
