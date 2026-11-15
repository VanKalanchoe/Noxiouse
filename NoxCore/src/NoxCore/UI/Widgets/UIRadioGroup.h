#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    // Mutually exclusive radio button group supporting horizontal or vertical layout.
    class UIRadioGroup : public UIStackBox
    {
    public:
        explicit UIRadioGroup(
            std::vector<std::string> items = {},
            int selectedIndex = 0,
            std::function<void(int)> onSelectionChanged = {},
            UIOrientation orientation = UIOrientation::Vertical);

        void SetItems(std::vector<std::string> items);
        void SetSelectedIndex(int index, bool notify = false);
        int GetSelectedIndex() const { return m_SelectedIndex; }

        std::function<void(int)> OnSelectionChanged;

    private:
        void Rebuild();

        std::vector<std::string> m_Items;
        int m_SelectedIndex = 0;
    };
}
