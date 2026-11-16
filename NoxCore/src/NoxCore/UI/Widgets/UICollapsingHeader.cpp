#include "UICollapsingHeader.h"

#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    namespace
    {
        class UICollapseToggleButton final : public UIButton
        {
        public:
            UICollapseToggleButton(std::function<void()> onClick, const bool* expandedFlag)
                : UIButton("", std::move(onClick)), m_ExpandedFlag(expandedFlag)
            {
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIButton::OnPaint(drawList, theme);
                const UIRect& b = GetBounds();
                const UIStyleBox& style = GetResolvedStyle();
                const bool expanded = m_ExpandedFlag ? *m_ExpandedFlag : true;
                const float cx = b.Min.x + 12.0f;
                const float cy = (b.Min.y + b.Max.y) * 0.5f;
                const UIColor iconColor = HasState(GetState(), UIState::Hovered)
                    ? style.TextColor
                    : theme.GetToken("TextMuted", style.TextColor);
                Icons::DrawChevron(drawList, { cx, cy }, 8.0f, expanded, iconColor, 1.6f);
            }

        private:
            const bool* m_ExpandedFlag = nullptr;
        };
    }

    UICollapsingHeader::UICollapsingHeader(std::string title, bool expanded)
        : UIStackBox(UIOrientation::Vertical, 4.0f)
        , m_Title(std::move(title))
        , m_Expanded(expanded)
    {
        WidthMode = UISizeMode::Fill;

        m_HeaderBtn = Ref<UIButton>(new UICollapseToggleButton([this]()
        {
            SetExpanded(!m_Expanded);
        }, &m_Expanded));
        m_HeaderBtn->StyleClass = "TreeRow";
        m_HeaderBtn->Padding = { 24.0f, 5.0f, 8.0f, 5.0f };
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
        m_HeaderBtn->SetText(m_Title);
    }
}
