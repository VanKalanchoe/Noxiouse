#pragma once

#include <string>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UIButton;

    // Collapsible section with a clickable header bar and toggleable vertical content stack.
    class UICollapsingHeader : public UIStackBox
    {
    public:
        explicit UICollapsingHeader(std::string title, bool expanded = true);

        void SetExpanded(bool expanded);
        bool IsExpanded() const { return m_Expanded; }

        void SetTitle(const std::string& title);
        void AddContent(const Ref<UIElement>& child);
        const Ref<UIStackBox>& GetContentBox() const { return m_Content; }

    private:
        void SyncHeader();

        std::string m_Title;
        bool m_Expanded = true;
        Ref<UIButton> m_HeaderBtn;
        Ref<UIStackBox> m_Content;
    };
}
