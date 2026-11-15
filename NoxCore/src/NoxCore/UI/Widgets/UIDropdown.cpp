#include "UIDropdown.h"

#include <algorithm>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        class UIDropdownBackdrop : public UIAnchorBox
        {
        public:
            explicit UIDropdownBackdrop(UIContext* context)
                : m_Context(context)
            {
                HitTestMode = UIHitTestMode::Self;
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

        private:
            UIContext* m_Context = nullptr;
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
        const float halfW = std::max(3.0f, style.FontSize * 0.28f);
        const float halfH = std::max(1.5f, style.FontSize * 0.14f);
        const float cx = b.Max.x - Padding.Right - halfW - 2.0f;
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        const UIColor arrowColor = style.TextColor;

        drawList.AddBezier(
            { cx - halfW, cy - halfH },
            { cx - halfW * 0.5f, cy },
            { cx - halfW * 0.25f, cy + halfH * 0.5f },
            { cx, cy + halfH },
            1.6f,
            arrowColor
        );
        drawList.AddBezier(
            { cx, cy + halfH },
            { cx + halfW * 0.25f, cy + halfH * 0.5f },
            { cx + halfW * 0.5f, cy },
            { cx + halfW, cy - halfH },
            1.6f,
            arrowColor
        );
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

        Ref<UIDropdownBackdrop> backdrop(new UIDropdownBackdrop(m_Context));

        Ref<UIStackBox> popup(new UIStackBox(UIOrientation::Vertical, 2.0f));
        popup->StyleClass = "MenuPopup";
        popup->DrawBackground = true;
        popup->HitTestMode = UIHitTestMode::Self;
        popup->WidthMode = UISizeMode::Auto;
        popup->Padding = UIThickness::All(4.0f);

        const UIRect& b = GetBounds();
        popup->MinSize.x = b.Width();
        popup->AnchorMin = { 0.0f, 0.0f };
        popup->AnchorMax = { 0.0f, 0.0f };
        popup->AnchorOffsets = { b.Min.x, b.Max.y + 3.0f, 0.0f, 0.0f };

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
            popup->AddChild(itemBtn);
        }

        backdrop->AddChild(popup);
        m_Context->SetOverlay(backdrop);
    }
}
