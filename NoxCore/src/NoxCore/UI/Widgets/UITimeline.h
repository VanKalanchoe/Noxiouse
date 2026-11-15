#pragma once

#include <functional>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    // Scrubbable timeline bar with tick marks, keyframe markers, and draggable playhead.
    class UITimeline : public UIElement
    {
    public:
        explicit UITimeline(float duration = 5.0f, float currentTime = 0.0f);

        void SetDuration(float duration);
        void SetCurrentTime(float time, bool notify = false);
        float GetCurrentTime() const { return m_CurrentTime; }
        float GetDuration() const { return m_Duration; }

        void SetKeyframes(std::vector<float> times) { m_Keyframes = std::move(times); }
        const std::vector<float>& GetKeyframes() const { return m_Keyframes; }

        std::function<void(float currentTime)> OnScrub;

        bool OnPointerDown(const UIPointerEvent& e) override;
        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerUp(const UIPointerEvent& e) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        float TimeFromX(float x) const;

        float m_Duration = 5.0f;
        float m_CurrentTime = 0.0f;
        std::vector<float> m_Keyframes{ 0.0f, 1.2f, 2.5f, 4.1f };
        bool m_Scrubbing = false;
    };
}
