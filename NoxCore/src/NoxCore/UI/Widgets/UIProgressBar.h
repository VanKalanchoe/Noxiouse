#pragma once

#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UILabel;

    // Horizontal progress bar with SDF rounded track, fill bar, and optional overlay text.
    class UIProgressBar : public UIElement
    {
    public:
        explicit UIProgressBar(float fraction = 0.0f, std::string overlayText = {});

        void SetFraction(float fraction);
        float GetFraction() const { return m_Fraction; }

        void SetOverlayText(const std::string& text);
        const std::string& GetOverlayText() const { return m_OverlayText; }

        bool ShowPercentage = true;
        UIColor FillColor{ 0.24f, 0.56f, 0.96f, 1.0f };

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        void SyncLabel();

        float m_Fraction = 0.0f;
        std::string m_OverlayText;
        Ref<UILabel> m_Label;
    };
}
