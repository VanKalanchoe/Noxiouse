#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    // Horizontal pill-bar segmented switcher (e.g., [ Local | World ] or [ Low | Med | High ]).
    class UISegmentedControl : public UIStackBox
    {
    public:
        explicit UISegmentedControl(
            std::vector<std::string> segments = {},
            int selectedIndex = 0,
            std::function<void(int)> onSelectionChanged = {});

        void SetSegments(std::vector<std::string> segments);
        void SetSelectedIndex(int index, bool notify = false);
        int GetSelectedIndex() const { return m_SelectedIndex; }

        std::function<void(int)> OnSelectionChanged;

    private:
        void Rebuild();

        std::vector<std::string> m_Segments;
        int m_SelectedIndex = 0;
    };
}
