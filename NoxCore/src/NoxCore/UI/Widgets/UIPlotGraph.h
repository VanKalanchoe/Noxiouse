#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    enum class UIPlotMode : uint8_t
    {
        Line = 0,
        Histogram
    };

    // Real-time line plot / histogram widget with auto-scaling, gridlines, and hover readout.
    class UIPlotGraph : public UIElement
    {
    public:
        explicit UIPlotGraph(std::string title = {}, UIPlotMode mode = UIPlotMode::Line);

        void SetValues(std::vector<float> values);
        void PushSample(float value);
        void Clear();

        void SetMode(UIPlotMode mode) { m_Mode = mode; }
        UIPlotMode GetMode() const { return m_Mode; }

        std::string Title;
        size_t MaxSamples = 120;
        bool AutoScale = true;
        float MinValue = 0.0f;
        float MaxValue = 1.0f;
        UIColor LineColor{ 0.24f, 0.72f, 0.96f, 1.0f };

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerLeave(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        UIPlotMode m_Mode = UIPlotMode::Line;
        std::vector<float> m_Values;
        int m_HoveredIndex = -1;
    };
}
