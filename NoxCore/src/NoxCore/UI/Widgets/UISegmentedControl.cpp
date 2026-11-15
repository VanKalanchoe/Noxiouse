#include "UISegmentedControl.h"

#include <algorithm>

#include "NoxCore/UI/Widgets/UIButton.h"

namespace Nox::UI
{
    UISegmentedControl::UISegmentedControl(
        std::vector<std::string> segments,
        int selectedIndex,
        std::function<void(int)> onSelectionChanged)
        : UIStackBox(UIOrientation::Horizontal, 3.0f)
        , OnSelectionChanged(std::move(onSelectionChanged))
        , m_Segments(std::move(segments))
        , m_SelectedIndex(selectedIndex)
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        Padding = UIThickness::All(3.0f);
        Rebuild();
    }

    void UISegmentedControl::SetSegments(std::vector<std::string> segments)
    {
        m_Segments = std::move(segments);
        m_SelectedIndex = m_Segments.empty()
            ? -1
            : std::clamp(m_SelectedIndex, 0, static_cast<int>(m_Segments.size()) - 1);
        Rebuild();
    }

    void UISegmentedControl::SetSelectedIndex(int index, bool notify)
    {
        if (m_Segments.empty())
            return;
        const int clamped = std::clamp(index, 0, static_cast<int>(m_Segments.size()) - 1);
        if (m_SelectedIndex == clamped)
            return;
        m_SelectedIndex = clamped;

        const auto& children = GetChildren();
        for (int i = 0; i < static_cast<int>(children.size()); ++i)
            children[i]->StyleVariant = (i == m_SelectedIndex) ? "primary" : "";

        if (notify && OnSelectionChanged)
            OnSelectionChanged(m_SelectedIndex);
    }

    void UISegmentedControl::Rebuild()
    {
        ClearChildren();
        for (int i = 0; i < static_cast<int>(m_Segments.size()); ++i)
        {
            Ref<UIButton> btn(new UIButton(m_Segments[i], [this, i]()
            {
                SetSelectedIndex(i, true);
            }));
            btn->StyleClass = "Button";
            btn->StyleVariant = (i == m_SelectedIndex) ? "primary" : "";
            btn->WidthMode = UISizeMode::Fill;
            AddChild(btn);
        }
    }
}
