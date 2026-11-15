#pragma once

#include <string>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UILabel;

    // Compact pill-shaped status/tag badge.
    class UIBadge : public UIElement
    {
    public:
        explicit UIBadge(
            std::string text,
            const UIColor& badgeColor = { 0.24f, 0.56f, 0.96f, 1.0f });

        void SetText(const std::string& text);
        UIColor BadgeColor{ 0.24f, 0.56f, 0.96f, 1.0f };

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& contentRect) override;
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        Ref<UILabel> m_Label;
    };
}
