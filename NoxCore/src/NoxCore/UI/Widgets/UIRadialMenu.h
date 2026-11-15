#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Core/UIElement.h"

namespace Nox::UI
{
    class UIContext;

    struct UIRadialMenuItem
    {
        std::string Label;
        UIColor AccentColor{ 0.24f, 0.56f, 0.96f, 1.0f };
        std::function<void()> OnTrigger;
    };

    // Circular pie / radial menu overlay centered at cursor or specified position.
    class UIRadialMenu : public UIElement
    {
    public:
        UIRadialMenu(UIContext& context, const UIVec2& centerPos, std::vector<UIRadialMenuItem> items);

        static void Open(UIContext& context, const UIVec2& centerPos, std::vector<UIRadialMenuItem> items);

        float Radius = 115.0f;

        bool OnPointerMove(const UIPointerEvent& e) override;
        bool OnPointerDown(const UIPointerEvent& e) override;

    protected:
        void OnPaint(UIDrawList& drawList, const UITheme& theme) override;

    private:
        int HitSector(const UIVec2& pos) const;

        UIContext& m_Context;
        UIVec2 m_Center{ 0.0f, 0.0f };
        std::vector<UIRadialMenuItem> m_Items;
        int m_HoveredIndex = -1;
    };
}
