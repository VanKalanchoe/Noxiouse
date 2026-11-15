#pragma once

#include <functional>
#include <string>
#include <vector>

#include "NoxCore/UI/Layout/UILayout.h"

namespace Nox::UI
{
    class UITextInput;
    class UIWrapBox;

    // Editable tag/chip input: displays removable tag pills and an inline text box (Enter adds a tag).
    class UITagInput : public UIStackBox
    {
    public:
        explicit UITagInput(std::vector<std::string> initialTags = {});

        void SetTags(std::vector<std::string> tags, bool notify = false);
        const std::vector<std::string>& GetTags() const { return m_Tags; }

        std::function<void(const std::vector<std::string>&)> OnTagsChanged;

    private:
        void RebuildChips();

        std::vector<std::string> m_Tags;
        Ref<UIWrapBox> m_ChipWrap;
        Ref<UITextInput> m_Input;
    };
}
