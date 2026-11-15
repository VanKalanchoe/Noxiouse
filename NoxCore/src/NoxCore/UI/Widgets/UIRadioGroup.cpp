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
                MinSize = { 60.0f, 24.0f };

                m_Label = Ref<UILabel>(new UILabel(std::move(label), 13.5f));
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
                UIVec2 labelSize = m_Label ? m_Label->Measure() : UIVec2{ 40.0f, 18.0f };
                return {
                    std::max(MinSize.x, labelSize.x + Padding.Horizontal() + 24.0f),
                    std::max(MinSize.y, labelSize.y + Padding.Vertical())
                };
            }

            void ArrangeChildren(const UIRect& contentRect) override
            {
                if (!m_Label)
                    return;
                UIRect r = contentRect;
                r.Min.x = std::min(r.Max.x, r.Min.x + 22.0f);
                m_Label->Arrange(r);
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                UIElement::OnPaint(drawList, theme);

                const UIRect& b = GetBounds();
                const float rOuter = 7.5f;
                const UIVec2 center{ b.Min.x + Padding.Left + rOuter, (b.Min.y + b.Max.y) * 0.5f };
                const bool hovered = HasState(GetState(), UIState::Hovered);

                const UIColor borderColor = m_Selected
                    ? UIColor{ 0.24f, 0.56f, 0.96f, 1.0f }
                    : (hovered ? UIColor{ 0.50f, 0.56f, 0.66f, 1.0f } : UIColor{ 0.34f, 0.38f, 0.46f, 1.0f });
                const UIColor fillBg = hovered
                    ? UIColor{ 0.16f, 0.18f, 0.23f, 1.0f }
                    : UIColor{ 0.11f, 0.13f, 0.17f, 1.0f };

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
                    const float rInner = 3.8f;
                    drawList.AddRoundedRect(
                        center - UIVec2{ rInner, rInner },
                        center + UIVec2{ rInner, rInner },
                        { 0.24f, 0.56f, 0.96f, 1.0f },
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
