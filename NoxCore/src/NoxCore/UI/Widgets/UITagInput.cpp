#include "UITagInput.h"

#include "NoxCore/UI/Layout/UIWrapBox.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    UITagInput::UITagInput(std::vector<std::string> initialTags)
        : UIStackBox(UIOrientation::Vertical, 6.0f)
        , m_Tags(std::move(initialTags))
    {
        StyleClass = "TextInput";
        DrawBackground = true;
        Padding = UIThickness::All(8.0f);
        WidthMode = UISizeMode::Fill;

        m_ChipWrap = Ref<UIWrapBox>(new UIWrapBox(6.0f, 6.0f));
        AddChild(m_ChipWrap);

        m_Input = Ref<UITextInput>(new UITextInput("", "Add tag and press Enter..."));
        m_Input->OnSubmit = [this](const std::string& text)
        {
            if (text.empty())
                return;
            m_Tags.push_back(text);
            m_Input->Text.clear();
            RebuildChips();
            if (OnTagsChanged)
                OnTagsChanged(m_Tags);
        };
        AddChild(m_Input);

        RebuildChips();
    }

    void UITagInput::SetTags(std::vector<std::string> tags, bool notify)
    {
        m_Tags = std::move(tags);
        RebuildChips();
        if (notify && OnTagsChanged)
            OnTagsChanged(m_Tags);
    }

    void UITagInput::RebuildChips()
    {
        if (!m_ChipWrap)
            return;
        m_ChipWrap->ClearChildren();

        for (size_t i = 0; i < m_Tags.size(); ++i)
        {
            auto chip = Ref<UIButton>(new UIButton(m_Tags[i] + "  x", [this, i]()
            {
                if (i < m_Tags.size())
                {
                    m_Tags.erase(m_Tags.begin() + static_cast<ptrdiff_t>(i));
                    RebuildChips();
                    if (OnTagsChanged)
                        OnTagsChanged(m_Tags);
                }
            }));
            chip->StyleVariant = "primary";
            m_ChipWrap->AddChild(chip);
        }
    }
}
