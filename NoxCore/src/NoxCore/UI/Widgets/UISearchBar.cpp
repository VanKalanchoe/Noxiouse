#include "UISearchBar.h"

#include "NoxCore/UI/Widgets/UIBadge.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    UISearchBar::UISearchBar(
        std::string placeholder,
        std::function<void(const std::string&)> onSearchChanged)
        : UIStackBox(UIOrientation::Horizontal, 6.0f)
        , OnSearchChanged(std::move(onSearchChanged))
    {
        WidthMode = UISizeMode::Fill;

        m_Input = Ref<UITextInput>(new UITextInput("", std::move(placeholder)));
        m_Input->WidthMode = UISizeMode::Fill;
        m_Input->OnChanged = [this](const std::string& text)
        {
            UpdateControls();
            if (OnSearchChanged)
                OnSearchChanged(text);
        };

        m_CountBadge = Ref<UIBadge>(new UIBadge("0", UIColor{ 0.35f, 0.45f, 0.65f, 1.0f }));
        m_CountBadge->VAlign = UIAlignment::Center;
        m_CountBadge->Visible = false;

        m_ClearBtn = Ref<UIButton>(new UIButton("X", [this]()
        {
            SetQuery("", true);
        }));
        m_ClearBtn->Visible = false;

        AddChild(m_Input);
        AddChild(m_CountBadge);
        AddChild(m_ClearBtn);
    }

    void UISearchBar::SetQuery(const std::string& query, bool notify)
    {
        if (!m_Input)
            return;
        m_Input->Text = query;
        UpdateControls();
        if (notify && OnSearchChanged)
            OnSearchChanged(query);
    }

    const std::string& UISearchBar::GetQuery() const
    {
        static const std::string kEmpty;
        return m_Input ? m_Input->Text : kEmpty;
    }

    void UISearchBar::SetResultCount(int count)
    {
        if (!m_CountBadge)
            return;
        if (count < 0)
        {
            m_CountBadge->Visible = false;
            return;
        }
        m_CountBadge->SetText(std::to_string(count) + " hits");
        m_CountBadge->Visible = true;
    }

    void UISearchBar::UpdateControls()
    {
        if (m_ClearBtn && m_Input)
            m_ClearBtn->Visible = !m_Input->Text.empty();
    }
}
