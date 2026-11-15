#pragma once

#include <string>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UILabel;

    // Titled bordered container card with a header bar and vertical content stack.
    class UIGroupBox : public UIStackBox
    {
    public:
        explicit UIGroupBox(std::string title, const UIColor& accentColor = { 0.24f, 0.56f, 0.96f, 1.0f });

        void SetTitle(const std::string& title);
        void AddContent(const Ref<UIElement>& child);
        const Ref<UIStackBox>& GetContentBox() const { return m_Content; }

    private:
        Ref<UILabel> m_TitleLabel;
        Ref<UIStackBox> m_Content;
    };
}
