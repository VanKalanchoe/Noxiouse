#pragma once

#include <functional>
#include <string>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIBadge;
    class UIButton;
    class UITextInput;

    // Composite search bar with text input, optional result count badge, and clear [X] button.
    class UISearchBar : public UIStackBox
    {
    public:
        explicit UISearchBar(
            std::string placeholder = "Search...",
            std::function<void(const std::string&)> onSearchChanged = {});

        void SetQuery(const std::string& query, bool notify = false);
        const std::string& GetQuery() const;
        void SetResultCount(int count);

        std::function<void(const std::string&)> OnSearchChanged;

    private:
        void UpdateControls();

        Ref<UITextInput> m_Input;
        Ref<UIBadge> m_CountBadge;
        Ref<UIButton> m_ClearBtn;
    };
}
