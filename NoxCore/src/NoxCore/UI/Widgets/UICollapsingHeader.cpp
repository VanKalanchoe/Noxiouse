#include "UICollapsingHeader.h"

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UICollapsingHeader::UICollapsingHeader(std::string title, bool expanded)
        : UIStackBox(UIOrientation::Vertical, 4.0f)
        , m_Title(std::move(title))
        , m_Expanded(expanded)
    {
        WidthMode = UISizeMode::Fill;

        m_HeaderBtn = Ref<UIButton>(new UIButton("", [this]()
        {
            SetExpanded(!m_Expanded);
        }));
        m_HeaderBtn->StyleClass = "TreeRow";
        m_HeaderBtn->WidthMode = UISizeMode::Fill;
        m_HeaderBtn->HeightMode = UISizeMode::Auto;
        AddChild(m_HeaderBtn);

        m_Content = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 6.0f));
        m_Content->WidthMode = UISizeMode::Fill;
        m_Content->Padding = UIThickness{ 12.0f, 4.0f, 4.0f, 4.0f };
        m_Content->Visible = m_Expanded;
        AddChild(m_Content);

        SyncHeader();
    }

    void UICollapsingHeader::SetExpanded(bool expanded)
    {
        if (m_Expanded == expanded)
            return;
        m_Expanded = expanded;
        if (m_Content)
            m_Content->Visible = m_Expanded;
        SyncHeader();
    }

    void UICollapsingHeader::SetTitle(const std::string& title)
    {
        m_Title = title;
        SyncHeader();
    }

    void UICollapsingHeader::AddContent(const Ref<UIElement>& child)
    {
        if (m_Content)
            m_Content->AddChild(child);
    }

    void UICollapsingHeader::SyncHeader()
    {
        if (!m_HeaderBtn)
            return;
        m_HeaderBtn->SetText(std::string(m_Expanded ? "v  " : ">  ") + m_Title);
    }
}
