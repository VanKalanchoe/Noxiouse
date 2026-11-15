#include "UITabPanel.h"

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UITabPanel::UITabPanel()
        : UIStackBox(UIOrientation::Vertical, 0.0f)
    {
        StyleClass = "Panel";
        DrawBackground = true;
        Padding = UIThickness::All(0.0f);

        m_TabBar = Ref<UIStackBox>(new UIStackBox(UIOrientation::Horizontal, 4.0f));
        m_TabBar->StyleClass = "TabBar";
        m_TabBar->DrawBackground = true;
        m_TabBar->HeightMode = UISizeMode::Fixed;
        m_TabBar->FixedSize.y = 32.0f;
        m_TabBar->Padding = UIThickness::Symmetric(8.0f, 4.0f);

        m_ContentHost = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 0.0f));
        m_ContentHost->WidthMode = UISizeMode::Fill;
        m_ContentHost->HeightMode = UISizeMode::Fill;
        m_ContentHost->Padding = UIThickness::All(10.0f);

        AddChild(m_TabBar);
        AddChild(m_ContentHost);
    }

    size_t UITabPanel::AddTab(const std::string& title, const Ref<UIElement>& content)
    {
        const size_t idx = m_Tabs.size();
        Ref<UIButton> btn(new UIButton(title, [this, idx]()
        {
            SetActiveTab(idx);
        }));
        btn->StyleClass = "Tab";
        btn->VAlign = UIAlignment::Center;
        m_TabBar->AddChild(btn);

        if (content)
        {
            content->WidthMode = UISizeMode::Fill;
            content->HeightMode = UISizeMode::Fill;
            m_ContentHost->AddChild(content);
        }

        m_Tabs.push_back({ title, btn, content });
        if (m_Tabs.size() == 1)
            m_ActiveIndex = 0;
        RefreshTabStyles();
        return idx;
    }

    void UITabPanel::SetActiveTab(size_t index)
    {
        if (index >= m_Tabs.size())
            return;
        m_ActiveIndex = index;
        RefreshTabStyles();
    }

    void UITabPanel::RefreshTabStyles()
    {
        for (size_t i = 0; i < m_Tabs.size(); ++i)
        {
            const bool active = (i == m_ActiveIndex);
            if (m_Tabs[i].Button)
                m_Tabs[i].Button->StyleVariant = active ? "active" : "";
            if (m_Tabs[i].Content)
                m_Tabs[i].Content->Visible = active;
        }
    }
}
