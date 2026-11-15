#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIContext;
    class UITextInput;

    struct UICommandItem
    {
        std::string Title;
        std::string Category;
        std::string Shortcut;
        std::function<void()> OnExecute;
    };

    // Centered quick-action / command palette overlay with live search filtering and keyboard navigation.
    class UICommandPalette : public UIStackBox
    {
    public:
        UICommandPalette(UIContext& context, std::vector<UICommandItem> commands);

        static void Open(UIContext& context, std::vector<UICommandItem> commands);

        bool OnKeyDown(const UIKeyEvent& e) override;

    private:
        void RebuildResults(const std::string& filter);
        void ExecuteSelected();

        UIContext& m_Context;
        std::vector<UICommandItem> m_AllCommands;
        std::vector<UICommandItem> m_Filtered;
        int m_SelectedIndex = 0;

        Ref<UITextInput> m_SearchInput;
        Ref<UIStackBox> m_ResultsBox;
    };
}
