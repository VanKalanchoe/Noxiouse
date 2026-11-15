#include "UIRadialMenu.h"

#include <cmath>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIRadialMenu::UIRadialMenu(UIContext& context, const UIVec2& centerPos, std::vector<UIRadialMenuItem> items)
        : m_Context(context)
        , m_Center(centerPos)
        , m_Items(std::move(items))
    {
        HitTestMode = UIHitTestMode::Self;
        DrawBackground = false;
        WidthMode = UISizeMode::Fill;
        HeightMode = UISizeMode::Fill;
    }

    void UIRadialMenu::Open(UIContext& context, const UIVec2& centerPos, std::vector<UIRadialMenuItem> items)
    {
        if (items.empty())
            return;
        Ref<UIRadialMenu> menu(new UIRadialMenu(context, centerPos, std::move(items)));
        context.SetOverlay(menu);
    }

    int UIRadialMenu::HitSector(const UIVec2& pos) const
    {
        if (m_Items.empty())
            return -1;
        const UIVec2 d = pos - m_Center;
        const float distSq = d.x * d.x + d.y * d.y;
        if (distSq < 26.0f * 26.0f)
            return -1; // Inside center cancel hub

        constexpr float kTwoPi = 6.2831853f;
        float angle = std::atan2(d.y, d.x) + 1.5707963f; // 0 at top (-Y)
        if (angle < 0.0f)
            angle += kTwoPi;

        const float sectorAngle = kTwoPi / static_cast<float>(m_Items.size());
        const int idx = static_cast<int>(std::floor((angle + sectorAngle * 0.5f) / sectorAngle)) % static_cast<int>(m_Items.size());
        return idx;
    }

    bool UIRadialMenu::OnPointerMove(const UIPointerEvent& e)
    {
        m_HoveredIndex = HitSector(e.Position);
        return true;
    }

    bool UIRadialMenu::OnPointerDown(const UIPointerEvent& e)
    {
        if (e.Button != 1 && e.Button != 3)
            return false;

        const int hit = (e.Button == 1) ? HitSector(e.Position) : -1;
        UIContext& ctx = m_Context;
        std::function<void()> cb;
        if (hit >= 0 && hit < static_cast<int>(m_Items.size()))
            cb = std::move(m_Items[hit].OnTrigger);

        ctx.ClearOverlay();
        if (cb)
            cb();
        return true;
    }

    void UIRadialMenu::OnPaint(UIDrawList& drawList, const UITheme&)
    {
        const UIRect& b = GetBounds();
        drawList.AddRoundedRect(b.Min, b.Max, { 0.03f, 0.04f, 0.06f, 0.45f });

        // Outer ring guide + center cancel hub
        drawList.AddRoundedRect(
            m_Center - UIVec2{ Radius, Radius },
            m_Center + UIVec2{ Radius, Radius },
            { 0.08f, 0.09f, 0.13f, 0.55f },
            glm::vec4(Radius),
            1.5f,
            { 0.28f, 0.33f, 0.42f, 0.7f }
        );
        drawList.AddRoundedRect(
            m_Center - UIVec2{ 24.0f, 24.0f },
            m_Center + UIVec2{ 24.0f, 24.0f },
            { 0.12f, 0.14f, 0.19f, 0.95f },
            glm::vec4(24.0f),
            1.5f,
            { 0.36f, 0.42f, 0.52f, 0.9f }
        );

        const Ref<Font> font = Font::GetDefault();
        drawList.AddText("X", font, m_Center - UIVec2{ 4.5f, 7.5f }, 13.0f, { 0.68f, 0.74f, 0.84f, 1.0f });

        constexpr float kTwoPi = 6.2831853f;
        const int n = static_cast<int>(m_Items.size());
        for (int i = 0; i < n; ++i)
        {
            const float angle = (static_cast<float>(i) / static_cast<float>(n)) * kTwoPi - 1.5707963f;
            const UIVec2 dir{ std::cos(angle), std::sin(angle) };
            const UIVec2 itemCenter = m_Center + dir * Radius;
            const bool hovered = (i == m_HoveredIndex);

            const UIVec2 textSize = UILabel::MeasureText(m_Items[i].Label, font, 13.5f);
            const UIVec2 pillSize{ std::max(84.0f, textSize.x + 24.0f), 32.0f };
            const UIVec2 pMin = itemCenter - pillSize * 0.5f;
            const UIVec2 pMax = itemCenter + pillSize * 0.5f;

            if (hovered)
                drawList.AddBezier(m_Center + dir * 24.0f, m_Center + dir * (Radius * 0.5f), m_Center + dir * (Radius * 0.5f), itemCenter, 2.5f, m_Items[i].AccentColor);

            const UIColor bg = hovered
                ? UIColor{ m_Items[i].AccentColor.r, m_Items[i].AccentColor.g, m_Items[i].AccentColor.b, 0.95f }
                : UIColor{ 0.11f, 0.13f, 0.18f, 0.95f };
            const UIColor border = hovered
                ? UIColor{ 0.96f, 0.98f, 1.0f, 0.95f }
                : m_Items[i].AccentColor;

            drawList.AddRoundedRect(pMin, pMax, bg, glm::vec4(16.0f), 1.5f, border);
            drawList.AddText(
                m_Items[i].Label,
                font,
                itemCenter - textSize * 0.5f,
                13.5f,
                { 0.96f, 0.98f, 1.0f, 1.0f }
            );
        }
    }
}
