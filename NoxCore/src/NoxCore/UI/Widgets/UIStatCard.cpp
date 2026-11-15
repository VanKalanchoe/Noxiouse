#include "UIStatCard.h"

#include "NoxCore/UI/Widgets/UIBadge.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    UIStatCard::UIStatCard(
        std::string title,
        std::string value,
        std::string badgeText,
        const UIColor& badgeColor)
        : UIStackBox(UIOrientation::Vertical, 4.0f)
    {
        StyleClass = "Panel";
        DrawBackground = true;
        Padding = UIThickness::All(12.0f);
        WidthMode = UISizeMode::Fill;

        m_TitleLabel = Ref<UILabel>(new UILabel(std::move(title), 12.0f));
        m_TitleLabel->UseColorOverride = true;
        m_TitleLabel->ColorOverride = { 0.62f, 0.67f, 0.76f, 1.0f };
        AddChild(m_TitleLabel);

        auto bottomRow = Ref<UIStackBox>(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        m_ValueLabel = Ref<UILabel>(new UILabel(std::move(value), 20.0f));
        m_ValueLabel->WidthMode = UISizeMode::Fill;
        m_ValueLabel->VAlign = UIAlignment::Center;
        bottomRow->AddChild(m_ValueLabel);

        if (!badgeText.empty())
        {
            m_Badge = Ref<UIBadge>(new UIBadge(std::move(badgeText), badgeColor));
            m_Badge->VAlign = UIAlignment::Center;
            bottomRow->AddChild(m_Badge);
        }
        AddChild(bottomRow);
    }

    void UIStatCard::SetValue(const std::string& value)
    {
        if (m_ValueLabel)
            m_ValueLabel->Text = value;
    }

    void UIStatCard::SetBadge(const std::string& text)
    {
        if (m_Badge)
            m_Badge->SetText(text);
    }
}
