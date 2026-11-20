#include "UILevelMeter.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"

namespace Nox::UI
{
    UILevelMeter::UILevelMeter(UIOrientation orientation)
        : Orientation(orientation)
    {
        HitTestMode = UIHitTestMode::Self; // (for the tooltip)
        if (orientation == UIOrientation::Horizontal)
        {
            ShowRms = false;
            ShowText = true;
            WidthMode = UISizeMode::Fill;
        }
    }

    float UILevelMeter::ToDb(float linear)
    {
        return 20.0f * std::log10(std::max(linear, 1e-6f));
    }

    float UILevelMeter::Fraction(float linear) const
    {
        if (MaxDb <= MinDb)
            return 0.0f;
        return std::clamp((ToDb(linear) - MinDb) / (MaxDb - MinDb), 0.0f, 1.0f);
    }

    UIVec2 UILevelMeter::ComputeDesiredSize()
    {
        if (WidthMode == UISizeMode::Fixed && HeightMode == UISizeMode::Fixed)
            return FixedSize;
        return Orientation == UIOrientation::Vertical ? UIVec2{ 22.0f, 120.0f } : UIVec2{ 120.0f, 34.0f };
    }

    void UILevelMeter::OnUpdate(float)
    {
        if (Source)
            Levels = Source();
        if (AutoTooltip)
        {
            char text[160];
            std::snprintf(text, sizeof(text), "Peak L %.1f dB / R %.1f dB\nRMS L %.1f dB / R %.1f dB", ToDb(Levels.PeakLeft), ToDb(Levels.PeakRight),
                          ToDb(Levels.RmsLeft), ToDb(Levels.RmsRight));
            Tooltip = text;
            if (Orientation == UIOrientation::Vertical && TickStepDb > 0.0f)
            {
                std::snprintf(text, sizeof(text), "\nTicks every %g dB from 0 dB down", TickStepDb);
                Tooltip += text;
            }
        }
    }

    void UILevelMeter::OnPaint(UIDrawList& d, const UITheme& theme)
    {
        const UIRect b = GetBounds();
        const UIColor background = theme.GetToken("MeterBackground", { 0.08f, 0.08f, 0.09f, 1.0f });
        const UIColor low = theme.GetToken("MeterLow", { 0.27f, 0.78f, 0.35f, 1.0f });
        const UIColor loud = theme.GetToken("MeterLoud", { 0.90f, 0.78f, 0.24f, 1.0f });
        const UIColor clip = theme.GetToken("MeterClip", { 0.90f, 0.24f, 0.20f, 1.0f });
        const auto colorAt = [&](float fraction)
        {
            const float db = MinDb + fraction * (MaxDb - MinDb);
            return db >= ClipDb ? clip : db >= LoudDb ? loud : low;
        };
        const float rms[2] = { Levels.RmsLeft, Levels.RmsRight };
        const float peak[2] = { Levels.PeakLeft, Levels.PeakRight };

        if (Orientation == UIOrientation::Vertical)
        {
            d.AddRoundedRect(b.Min, b.Max, background);
            const float barWidth = (b.Width() - 3.0f) * 0.5f;
            for (int channel = 0; channel < 2; ++channel)
            {
                const float x0 = b.Min.x + 1.0f + static_cast<float>(channel) * (barWidth + 1.0f);
                const float fill = Fraction(ShowRms ? rms[channel] : peak[channel]);
                const float top = Fraction(peak[channel]);
                if (fill > 0.0f)
                    d.AddRoundedRect({ x0, b.Max.y - fill * b.Height() }, { x0 + barWidth, b.Max.y }, colorAt(fill));
                if (ShowRms && top > 0.0f)
                    d.AddRoundedRect({ x0, b.Max.y - top * b.Height() - 1.0f }, { x0 + barWidth, b.Max.y - top * b.Height() + 1.0f }, colorAt(top));
            }
            if (TickStepDb > 0.0f && MaxDb > MinDb)
            {
                UIColor tick = theme.GetToken("TextMuted", { 0.63f, 0.63f, 0.67f, 1.0f });
                tick.a *= 0.63f;
                for (float db = std::min(0.0f, MaxDb); db >= MinDb; db -= TickStepDb)
                {
                    const float y = b.Min.y + b.Height() * (1.0f - (db - MinDb) / (MaxDb - MinDb));
                    d.AddRoundedRect({ b.Min.x, y }, { b.Min.x + 3.0f, y + 1.0f }, tick);
                }
            }
            return;
        }

        // Horizontal: one bar per channel, each half the height less a 2 px gap.
        const float barHeight = std::max(1.0f, (b.Height() - 2.0f) * 0.5f);
        const UIColor textColor = theme.GetToken("Text", { 0.95f, 0.96f, 1.0f, 1.0f });
        const float fontSize = std::clamp(barHeight * 0.75f, 8.0f, 13.0f);
        for (int channel = 0; channel < 2; ++channel)
        {
            const float top = b.Min.y + static_cast<float>(channel) * (barHeight + 2.0f);
            const UIVec2 min{ b.Min.x, top }, max{ b.Max.x, top + barHeight };
            d.AddRoundedRect(min, max, background, glm::vec4(3.0f));
            const float fill = Fraction(ShowRms ? rms[channel] : peak[channel]);
            if (fill > 0.0f)
                d.AddRoundedRect(min, { min.x + (max.x - min.x) * fill, max.y }, colorAt(fill), glm::vec4(3.0f));
            if (ShowRms)
            {
                const float at = Fraction(peak[channel]);
                if (at > 0.0f)
                {
                    const float x = min.x + (max.x - min.x) * at;
                    d.AddRoundedRect({ x - 1.0f, min.y }, { x + 1.0f, max.y }, colorAt(at));
                }
            }
            if (ShowText)
            {
                const float level = ShowRms ? rms[channel] : peak[channel];
                std::string text = channel == 0 ? "L" : "R";
                if (level > 1e-6f)
                {
                    char db[32];
                    std::snprintf(db, sizeof(db), "  %.1f dB", ToDb(level));
                    text += db;
                }
                d.AddText(text, nullptr, { min.x + 6.0f, top + (barHeight - fontSize) * 0.5f }, fontSize, textColor);
            }
        }
    }
}
