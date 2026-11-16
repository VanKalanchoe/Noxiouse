#include "UIDropdown.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"

namespace Nox::UI
{
    namespace
    {
        class UIDropdownBackdrop : public UIAnchorBox
        {
        public:
            UIDropdownBackdrop(UIContext* context, const UIRect& triggerBounds, const Ref<UIStackBox>& popup, const Ref<UIScrollView>& scroll)
                : m_Context(context), m_TriggerBounds(triggerBounds), m_Popup(popup), m_Scroll(scroll)
            {
                HitTestMode = UIHitTestMode::Self;
                if (m_Popup)
                    AddChild(m_Popup);
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && m_Context)
                {
                    m_Context->ClearOverlay();
                    return true;
                }
                return false;
            }

        protected:
            void ArrangeChildren(const UIRect& contentRect) override
            {
                if (!m_Popup || !m_Popup->Visible)
                    return;

                constexpr float kMargin = 6.0f;
                const float maxPopupH = std::max(80.0f, std::min(320.0f, contentRect.Height() - kMargin * 2.0f));
                if (m_Scroll)
                    m_Scroll->MaxSize.y = maxPopupH - m_Popup->Padding.Vertical();

                const UIVec2 desired = m_Popup->Measure();
                const float w = std::min(desired.x, std::max(80.0f, contentRect.Width() - kMargin * 2.0f));
                float h = std::min(desired.y, maxPopupH);

                const float spaceBelow = contentRect.Max.y - kMargin - (m_TriggerBounds.Max.y + 3.0f);
                const float spaceAbove = (m_TriggerBounds.Min.y - 3.0f) - (contentRect.Min.y + kMargin);
                float y = m_TriggerBounds.Max.y + 3.0f;
                if (h > spaceBelow && spaceAbove > spaceBelow)
                {
                    h = std::min(h, std::max(60.0f, spaceAbove));
                    y = m_TriggerBounds.Min.y - 3.0f - h;
                }
                else
                {
                    y = std::clamp(y, contentRect.Min.y + kMargin, std::max(contentRect.Min.y + kMargin, contentRect.Max.y - h - kMargin));
                }

                float x = m_TriggerBounds.Min.x;
                x = std::clamp(x, contentRect.Min.x + kMargin, std::max(contentRect.Min.x + kMargin, contentRect.Max.x - w - kMargin));

                m_Popup->Arrange({ { x, y }, { x + w, y + h } });
            }

        private:
            UIContext* m_Context = nullptr;
            UIRect m_TriggerBounds{};
            Ref<UIStackBox> m_Popup;
            Ref<UIScrollView> m_Scroll;
        };
    }

    UIDropdown::UIDropdown(
        UIContext* context,
        std::vector<std::string> items,
        int selectedIndex,
        std::function<void(int)> onSelectionChanged)
        : OnSelectionChanged(std::move(onSelectionChanged))
        , m_Context(context)
        , m_Items(std::move(items))
    {
        StyleClass = "Button";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        Focusable = true;
        Padding = UIThickness::Symmetric(10.0f, 6.0f);
        MinSize = { 120.0f, 28.0f };

        m_Label = Ref<UILabel>(new UILabel(""));
        m_Label->HitTestMode = UIHitTestMode::None;
        m_Label->VAlign = UIAlignment::Center;
        AddChild(m_Label);

        if (!m_Items.empty())
            m_SelectedIndex = std::clamp(selectedIndex, 0, static_cast<int>(m_Items.size()) - 1);
        SyncLabel();
    }

    void UIDropdown::SetItems(std::vector<std::string> items)
    {
        m_Items = std::move(items);
        if (m_Items.empty())
            m_SelectedIndex = -1;
        else
            m_SelectedIndex = std::clamp(m_SelectedIndex < 0 ? 0 : m_SelectedIndex, 0, static_cast<int>(m_Items.size()) - 1);
        SyncLabel();
    }

    void UIDropdown::SetSelectedIndex(int index, bool notify)
    {
        if (m_Items.empty())
        {
            m_SelectedIndex = -1;
            SyncLabel();
            return;
        }

        int clamped = std::clamp(index, 0, static_cast<int>(m_Items.size()) - 1);
        if (m_SelectedIndex == clamped)
            return;

        m_SelectedIndex = clamped;
        SyncLabel();
        if (notify && OnSelectionChanged)
            OnSelectionChanged(m_SelectedIndex);
    }

    std::string UIDropdown::GetSelectedText() const
    {
        if (m_SelectedIndex >= 0 && m_SelectedIndex < static_cast<int>(m_Items.size()))
            return m_Items[m_SelectedIndex];
        return "";
    }

    bool UIDropdown::OnPointerUp(const UIPointerEvent& e)
    {
        if (e.Button != 1 || !IsEnabled())
            return false;
        if (GetBounds().Contains(e.Position))
            OpenPopup();
        return true;
    }

    UIVec2 UIDropdown::ComputeDesiredSize()
    {
        UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ 40.0f, 18.0f };
        return {
            std::max(MinSize.x, labelSize.x + Padding.Horizontal() + 22.0f),
            std::max(MinSize.y, labelSize.y + Padding.Vertical())
        };
    }

    void UIDropdown::ArrangeChildren(const UIRect& contentRect)
    {
        if (!m_Label)
            return;
        UIRect labelRect = contentRect;
        labelRect.Max.x = std::max(labelRect.Min.x, labelRect.Max.x - 18.0f);
        m_Label->Arrange(labelRect);
    }

    void UIDropdown::OnPaint(UIDrawList& drawList, const UITheme& theme)
    {
        UIElement::OnPaint(drawList, theme);

        const UIStyleBox& style = GetResolvedStyle();
        const UIRect& b = GetBounds();
        const float cx = b.Max.x - Padding.Right - 6.0f;
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        Icons::DrawChevron(drawList, { cx, cy }, 7.0f, true, style.TextColor, 1.5f);
    }

    void UIDropdown::SyncLabel()
    {
        if (!m_Label)
            return;
        const std::string text = GetSelectedText();
        m_Label->Text = text.empty() ? "<None>" : text;
    }

    void UIDropdown::OpenPopup()
    {
        if (!m_Context || m_Items.empty())
            return;

        Ref<UIStackBox> popup(new UIStackBox(UIOrientation::Vertical, 0.0f));
        popup->StyleClass = "MenuPopup";
        popup->DrawBackground = true;
        popup->HitTestMode = UIHitTestMode::Self;
        popup->WidthMode = UISizeMode::Auto;
        popup->Padding = UIThickness::All(4.0f);

        const UIRect& b = GetBounds();
        popup->MinSize.x = std::max(b.Width(), 120.0f);

        Ref<UIScrollView> scroll(new UIScrollView());
        scroll->WidthMode = UISizeMode::Fill;
        scroll->HeightMode = UISizeMode::Auto;
        scroll->ChildSpacing = 2.0f;

        for (int i = 0; i < static_cast<int>(m_Items.size()); ++i)
        {
            const bool isSelected = (i == m_SelectedIndex);
            Ref<UIButton> itemBtn(new UIButton(m_Items[i], [this, i]()
            {
                if (m_Context)
                    m_Context->ClearOverlay();
                SetSelectedIndex(i, true);
            }));
            itemBtn->StyleClass = "MenuItem";
            if (isSelected)
                itemBtn->StyleVariant = "primary";
            itemBtn->WidthMode = UISizeMode::Fill;
            itemBtn->HeightMode = UISizeMode::Auto;
            scroll->AddChild(itemBtn);
        }

        popup->AddChild(scroll);
        Ref<UIDropdownBackdrop> backdrop(new UIDropdownBackdrop(m_Context, b, popup, scroll));
        m_Context->SetOverlay(backdrop);
    }
}
