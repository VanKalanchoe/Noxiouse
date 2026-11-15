#include "UIAccordion.h"

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UIAccordion::UIAccordion(bool allowMultipleOpen)
        : UIStackBox(UIOrientation::Vertical, 4.0f)
        , AllowMultipleOpen(allowMultipleOpen)
    {
        WidthMode = UISizeMode::Fill;
    }

    void UIAccordion::AddSection(std::string title, const Ref<UIElement>& content, bool expanded)
    {
        const int idx = static_cast<int>(m_Sections.size());
        auto btn = Ref<UIButton>(new UIButton("", [this, idx]()
        {
            if (idx >= 0 && idx < static_cast<int>(m_Sections.size()))
            {
                const bool nextState = !m_Sections[idx].Expanded;
                if (nextState && !AllowMultipleOpen)
                {
                    for (auto& s : m_Sections)
                        s.Expanded = false;
                }
                m_Sections[idx].Expanded = nextState;
                SyncSections();
            }
        }));
        btn->StyleClass = "TreeRow";
        btn->WidthMode = UISizeMode::Fill;
        btn->HeightMode = UISizeMode::Fixed;
        btn->FixedSize.y = 28.0f;

        if (content)
            content->Visible = expanded;

        AddChild(btn);
        if (content)
            AddChild(content);

        m_Sections.push_back({ std::move(title), btn, content, expanded });
        SyncSections();
    }

    void UIAccordion::ExpandSection(int index)
    {
        if (index < 0 || index >= static_cast<int>(m_Sections.size()))
            return;
        if (!AllowMultipleOpen)
        {
            for (auto& s : m_Sections)
                s.Expanded = false;
        }
        m_Sections[index].Expanded = true;
        SyncSections();
    }

    void UIAccordion::SyncSections()
    {
        for (auto& s : m_Sections)
        {
            if (s.HeaderBtn)
                s.HeaderBtn->SetText(std::string(s.Expanded ? "v  " : ">  ") + s.Title);
            if (s.Content)
                s.Content->Visible = s.Expanded;
        }
    }
}
