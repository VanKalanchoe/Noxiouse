#pragma once

#include <cstdint>
#include <functional>

#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    // An audio level meter, left and right: each channel a bar filled to its RMS with its peak as a line (or filled to the
    // peak when ShowRms is off), from MinDb to MaxDb. Levels are linear amplitudes (1 = 0 dBFS). Vertical: bars side by side
    // filling upwards, a tick every TickStepDb from 0 dB down (a mixer strip). Horizontal: bars one under the other filling to the
    // right, each with its channel and dB written on it (ShowText).
    //
    // Colours: theme tokens MeterBackground, MeterLow (below LoudDb), MeterLoud (from LoudDb), MeterClip (from ClipDb). The values
    // come from Source each frame when it is set, else from Levels (set them yourself, or in an OnSync). AutoTooltip: the peak and
    // RMS of both channels in dB as the tooltip.
    class UILevelMeter : public UIElement
    {
    public:
        struct Values
        {
            float RmsLeft = 0.0f, RmsRight = 0.0f;
            float PeakLeft = 0.0f, PeakRight = 0.0f;
        };

        explicit UILevelMeter(UIOrientation orientation = UIOrientation::Vertical);

        UIOrientation Orientation = UIOrientation::Vertical;
        std::function<Values()> Source;
        Values Levels;
        float MinDb = -60.0f;
        float MaxDb = 6.0f;
        float LoudDb = -12.0f;
        float ClipDb = -0.5f;
        float TickStepDb = 12.0f; // vertical only; 0: none
        bool ShowRms = true;
        bool ShowText = false;    // horizontal only
        bool AutoTooltip = true;

        // Where a linear level sits between MinDb (0) and MaxDb (1).
        float Fraction(float linear) const;
        static float ToDb(float linear);

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnUpdate(float dt) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;
    };
}
