#include "UIRadioGroup.h"

#include <algorithm>

#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox::UI
{
    namespace
    {
        class UIRadioOption : public UIElement
        {
        public:
            UIRadioOption(std::string label, bool selected, std::function<void()> onClick)
                : m_Selected(selected)
                , m_OnClick(std::move(onClick))
            {
                StyleClass = "CheckBox";
                DrawBackground = false;
                HitTestMode = UIHitTestMode::Self;
                Focusable = true;
                Padding = UIThickness::Symmetric(4.0f, 3.0f);

                m_Label = Ref<UILabel>(new UILabel(std::move(label)));
                m_Label->HitTestMode = UIHitTestMode::None;
                m_Label->VAlign = UIAlignment::Center;
                AddChild(m_Label);
            }

            void SetSelected(bool selected) { m_Selected = selected; }

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                if (e.Button == 1 && IsEnabled() && GetBounds().Contains(e.Position))
                {
                    if (m_OnClick)
                        m_OnClick();
                    return true;
                }
                return false;
            }

        protected:
            UIVec2 ComputeDesiredSize() override
            {
                const UIStyleBox& style = GetResolvedStyle();
                const float rOuter = std::max(5.5f, style.FontSize * 0.52f);
                const float gap = style.FontSize * 0.55f;
                const UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ 0.0f, style.FontSize };
                return {
                    std::max(MinSize.x, labelSize.x + Padding.Horizontal() + rOuter * 2.0f + gap),
                    std::max(MinSize.y, std::max(rOuter * 2.0f, labelSize.y) + Padding.Vertical())
                };
            }

            void ArrangeChildren(const UIRect& contentRect) override
            {
                if (!m_Label)
                    return;
                const UIStyleBox& style = GetResolvedStyle();
                const float rOuter = std::max(5.5f, style.FontSize * 0.52f);
                const float gap = style.FontSize * 0.55f;
                UIRect r = contentRect;
                r.Min.x = std::min(r.Max.x, r.Min.x + rOuter * 2.0f + gap);
                m_Label->Arrange(r);
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIElement::OnPaint(drawList, theme);

                const UIStyleBox& style = GetResolvedStyle();
                const UIRect& b = GetBounds();
                const float rOuter = std::max(5.5f, style.FontSize * 0.52f);
                const UIVec2 center{ b.Min.x + Padding.Left + rOuter, (b.Min.y + b.Max.y) * 0.5f };
                const bool hovered = HasState(GetState(), UIState::Hovered);
                const UIColor accent = theme.GetToken("Accent", { 0.259f, 0.529f, 0.961f, 1.0f });

                const UIColor borderColor = m_Selected
                    ? accent
                    : (hovered ? theme.GetToken("AccentHover", accent) : style.BorderColor);
                const UIColor fillBg = hovered
                    ? theme.GetToken("Surface1", style.FillColor)
                    : style.FillColor;

                drawList.AddRoundedRect(
                    center - UIVec2{ rOuter, rOuter },
                    center + UIVec2{ rOuter, rOuter },
                    fillBg,
                    glm::vec4(rOuter),
                    1.5f,
                    borderColor
                );

                if (m_Selected)
                {
                    const float rInner = rOuter * 0.5f;
                    drawList.AddRoundedRect(
                        center - UIVec2{ rInner, rInner },
                        center + UIVec2{ rInner, rInner },
                        accent,
                        glm::vec4(rInner)
                    );
                }
            }

        private:
            bool m_Selected = false;
            std::function<void()> m_OnClick;
            Ref<UILabel> m_Label;
        };
    }

    UIRadioGroup::UIRadioGroup(
        std::vector<std::string> items,
        int selectedIndex,
        std::function<void(int)> onSelectionChanged,
        UIOrientation orientation)
        : UIStackBox(orientation, orientation == UIOrientation::Horizontal ? 12.0f : 4.0f)
        , OnSelectionChanged(std::move(onSelectionChanged))
        , m_Items(std::move(items))
        , m_SelectedIndex(selectedIndex)
    {
        Rebuild();
    }

    void UIRadioGroup::SetItems(std::vector<std::string> items)
    {
        m_Items = std::move(items);
        if (m_Items.empty())
            m_SelectedIndex = -1;
        else
            m_SelectedIndex = std::clamp(m_SelectedIndex, 0, static_cast<int>(m_Items.size()) - 1);
        Rebuild();
    }

    void UIRadioGroup::SetSelectedIndex(int index, bool notify)
    {
        if (m_Items.empty())
            return;
        int clamped = std::clamp(index, 0, static_cast<int>(m_Items.size()) - 1);
        if (m_SelectedIndex == clamped)
            return;
        m_SelectedIndex = clamped;

        const auto& children = GetChildren();
        for (int i = 0; i < static_cast<int>(children.size()); ++i)
        {
            if (auto* opt = dynamic_cast<UIRadioOption*>(children[i].get()))
                opt->SetSelected(i == m_SelectedIndex);
        }

        if (notify && OnSelectionChanged)
            OnSelectionChanged(m_SelectedIndex);
    }

    void UIRadioGroup::Rebuild()
    {
        ClearChildren();
        for (int i = 0; i < static_cast<int>(m_Items.size()); ++i)
        {
            Ref<UIRadioOption> opt(new UIRadioOption(m_Items[i], i == m_SelectedIndex, [this, i]()
            {
                SetSelectedIndex(i, true);
            }));
            AddChild(opt);
        }
    }
}
