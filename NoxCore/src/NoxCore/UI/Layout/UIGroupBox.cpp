#include "UIGroupBox.h"

#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UISeparator.h"

namespace Nox::UI
{
    UIGroupBox::UIGroupBox(std::string title, const UIColor& accentColor)
        : UIStackBox(UIOrientation::Vertical, 8.0f)
    {
        StyleClass = "Panel";
        DrawBackground = true;
        Padding = UIThickness::All(12.0f);
        WidthMode = UISizeMode::Fill;

        auto header = Ref<UIStackBox>(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        auto dot = Ref<UIElement>(new UIElement());
        dot->WidthMode = UISizeMode::Fixed;
        dot->HeightMode = UISizeMode::Fixed;
        dot->FixedSize = { 4.0f, 16.0f };
        dot->VAlign = UIAlignment::Center;
        dot->StyleClass = "Button";
        dot->StyleVariant = "primary";
        dot->DrawBackground = true;
        (void)accentColor;

        m_TitleLabel = Ref<UILabel>(new UILabel(std::move(title), 14.5f));
        m_TitleLabel->VAlign = UIAlignment::Center;

        header->AddChild(dot);
        header->AddChild(m_TitleLabel);
        AddChild(header);
        AddChild(Ref<UISeparator>(new UISeparator()));

        m_Content = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 6.0f));
        m_Content->WidthMode = UISizeMode::Fill;
        AddChild(m_Content);
    }

    void UIGroupBox::SetTitle(const std::string& title)
    {
        if (m_TitleLabel)
            m_TitleLabel->Text = title;
    }

    void UIGroupBox::AddContent(const Ref<UIElement>& child)
    {
        if (m_Content && child)
            m_Content->AddChild(child);
    }
}
