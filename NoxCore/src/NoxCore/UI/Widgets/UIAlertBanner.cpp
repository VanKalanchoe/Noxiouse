#include "UIAlertBanner.h"

#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIAlertBanner::UIAlertBanner(
        UIAlertSeverity severity,
        std::string message,
        std::string actionLabel,
        std::function<void()> onAction,
        bool dismissible)
        : UIStackBox(UIOrientation::Horizontal, 10.0f)
    {
        switch (severity)
        {
        case UIAlertSeverity::Success: m_Accent = { 0.22f, 0.76f, 0.46f, 1.0f }; break;
        case UIAlertSeverity::Warning: m_Accent = { 0.92f, 0.64f, 0.16f, 1.0f }; break;
        case UIAlertSeverity::Error:   m_Accent = { 0.90f, 0.28f, 0.30f, 1.0f }; break;
        default:                       m_Accent = { 0.24f, 0.56f, 0.96f, 1.0f }; break;
        }

        StyleClass = "Panel";
        DrawBackground = false;
        WidthMode = UISizeMode::Fill;
        Padding = UIThickness{ 16.0f, 8.0f, 10.0f, 8.0f };

        auto msg = Ref<UILabel>(new UILabel(std::move(message), 13.5f));
        msg->WidthMode = UISizeMode::Fill;
        msg->VAlign = UIAlignment::Center;
        AddChild(msg);

        if (!actionLabel.empty())
        {
            auto actBtn = Ref<UIButton>(new UIButton(std::move(actionLabel), std::move(onAction)));
            actBtn->VAlign = UIAlignment::Center;
            AddChild(actBtn);
        }

        if (dismissible)
        {
            auto closeBtn = Ref<UIButton>(new UIButton("X", [this]() { Visible = false; }));
            closeBtn->WidthMode = UISizeMode::Fixed;
            closeBtn->FixedSize.x = 26.0f;
            closeBtn->VAlign = UIAlignment::Center;
            AddChild(closeBtn);
        }
    }

    void UIAlertBanner::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIStackBox::OnPaint(drawList, theme);

        const UIRect& b = GetBounds();
        drawList.AddRoundedRect(
            b.Min,
            b.Max,
            { m_Accent.r, m_Accent.g, m_Accent.b, 0.14f },
            glm::vec4(6.0f),
            1.0f,
            { m_Accent.r, m_Accent.g, m_Accent.b, 0.65f }
        );
        drawList.AddRoundedRect(
            { b.Min.x + 2.0f, b.Min.y + 4.0f },
            { b.Min.x + 6.0f, b.Max.y - 4.0f },
            m_Accent,
            glm::vec4(2.0f)
        );
    }
}
