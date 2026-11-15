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
                if (e.Button == 0 && m_Context)
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

        m_Label = Ref<UILabel>(new UILabel("", 13.5f));
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
        if (e.Button != 0 || !IsEnabled())
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

        const UIRect& b = GetBounds();
        const float cx = b.Max.x - Padding.Right - 6.0f;
        const float cy = (b.Min.y + b.Max.y) * 0.5f;
        const UIColor arrowColor = !IsEnabled()
            ? UIColor{ 0.45f, 0.48f, 0.54f, 0.7f }
            : UIColor{ 0.78f, 0.82f, 0.88f, 0.95f };

        drawList.AddBezier(
            { cx - 4.0f, cy - 2.0f },
            { cx - 2.0f, cy },
            { cx - 1.0f, cy + 1.0f },
            { cx, cy + 2.0f },
            1.6f,
            arrowColor
        );
        drawList.AddBezier(
            { cx, cy + 2.0f },
            { cx + 1.0f, cy + 1.0f },
            { cx + 2.0f, cy },
            { cx + 4.0f, cy - 2.0f },
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
        popup->WidthMode = UISizeMode::Fixed;
        popup->Padding = UIThickness::All(4.0f);

        const UIRect& b = GetBounds();
        const float popupWidth = std::max(b.Width(), 150.0f);
        popup->FixedSize.x = popupWidth;
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
            itemBtn->HeightMode = UISizeMode::Fixed;
            itemBtn->FixedSize.y = 26.0f;
            popup->AddChild(itemBtn);
        }

        backdrop->AddChild(popup);
        m_Context->SetOverlay(backdrop);
    }
}
