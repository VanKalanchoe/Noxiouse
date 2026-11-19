#include "UICommandPalette.h"

#include <algorithm>
#include <cctype>
#include <SDL3/SDL_scancode.h>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        bool ContainsNoCase(std::string_view haystack, std::string_view needle)
        {
            if (needle.empty())
                return true;
            auto it = std::search(
                haystack.begin(), haystack.end(),
                needle.begin(), needle.end(),
                [](char a, char b)
                {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }
            );
            return it != haystack.end();
        }

        class UIPaletteBackdrop : public UIAnchorBox
        {
        public:
            explicit UIPaletteBackdrop(UIContext* context)
                : m_Context(context)
            {
                StyleClass = "Background";
                DrawBackground = true;
                HitTestMode = UIHitTestMode::Self;
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (AnyChildContains(e.Position))
                    return true;
                if (e.Button == 1 && m_Context)
                {
                    m_Context->ClearOverlay();
                    return true;
                }
                return false;
            }

        private:
            UIContext* m_Context = nullptr;
        };
    }

    UICommandPalette::UICommandPalette(UIContext& context, std::vector<UICommandItem> commands)
        : UIStackBox(UIOrientation::Vertical, 8.0f)
        , m_Context(context)
        , m_AllCommands(std::move(commands))
    {
        StyleClass = "MenuPopup";
        DrawBackground = true;
        HitTestMode = UIHitTestMode::Self;
        WidthMode = UISizeMode::Fixed;
        FixedSize.x = 460.0f;
        Padding = UIThickness::All(12.0f);

        AnchorMin = { 0.5f, 0.0f };
        AnchorMax = { 0.5f, 0.0f };
        AnchorOffsets = { -230.0f, 80.0f, 0.0f, 0.0f };

        m_SearchInput = Ref<UITextInput>(new UITextInput("", "Type a command to filter..."));
        m_SearchInput->OnChanged = [this](const std::string& text)
        {
            RebuildResults(text);
        };
        m_SearchInput->OnSubmit = [this](const std::string&)
        {
            ExecuteSelected();
        };
        AddChild(m_SearchInput);

        m_ResultsBox = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 3.0f));
        m_ResultsBox->WidthMode = UISizeMode::Fill;
        AddChild(m_ResultsBox);

        RebuildResults("");
    }

    void UICommandPalette::Open(UIContext& context, std::vector<UICommandItem> commands)
    {
        Ref<UIPaletteBackdrop> backdrop(new UIPaletteBackdrop(&context));
        Ref<UICommandPalette> palette(new UICommandPalette(context, std::move(commands)));
        backdrop->AddChild(palette);
        context.SetOverlay(backdrop);
        context.SetFocus(palette->m_SearchInput.get());
    }

    bool UICommandPalette::OnKeyDown(const UIKeyEvent& e)
    {
        if (m_Filtered.empty())
            return UIStackBox::OnKeyDown(e);

        if (e.KeyCode == SDL_SCANCODE_DOWN)
        {
            m_SelectedIndex = (m_SelectedIndex + 1) % static_cast<int>(m_Filtered.size());
            RebuildResults(m_SearchInput ? m_SearchInput->Text : "");
            return true;
        }
        if (e.KeyCode == SDL_SCANCODE_UP)
        {
            const int n = static_cast<int>(m_Filtered.size());
            m_SelectedIndex = (m_SelectedIndex - 1 + n) % n;
            RebuildResults(m_SearchInput ? m_SearchInput->Text : "");
            return true;
        }
        if (e.KeyCode == SDL_SCANCODE_RETURN || e.KeyCode == SDL_SCANCODE_KP_ENTER)
        {
            ExecuteSelected();
            return true;
        }
        return UIStackBox::OnKeyDown(e);
    }

    void UICommandPalette::RebuildResults(const std::string& filter)
    {
        m_Filtered.clear();
        for (const auto& cmd : m_AllCommands)
        {
            if (ContainsNoCase(cmd.Title, filter) || ContainsNoCase(cmd.Category, filter))
            {
                m_Filtered.push_back(cmd);
                if (m_Filtered.size() >= 8)
                    break;
            }
        }

        if (m_Filtered.empty())
            m_SelectedIndex = -1;
        else
            m_SelectedIndex = std::clamp(m_SelectedIndex < 0 ? 0 : m_SelectedIndex, 0, static_cast<int>(m_Filtered.size()) - 1);

        if (!m_ResultsBox)
            return;
        m_ResultsBox->ClearChildren();

        if (m_Filtered.empty())
        {
            m_ResultsBox->AddChild(Ref<UILabel>(new UILabel("No matching commands")));
            return;
        }

        for (int i = 0; i < static_cast<int>(m_Filtered.size()); ++i)
        {
            const auto& cmd = m_Filtered[i];
            std::string label = cmd.Category.empty()
                ? cmd.Title
                : ("[" + cmd.Category + "]  " + cmd.Title);
            if (!cmd.Shortcut.empty())
                label += "   (" + cmd.Shortcut + ")";

            Ref<UIButton> btn(new UIButton(label, [this, i]()
            {
                m_SelectedIndex = i;
                ExecuteSelected();
            }));
            btn->StyleClass = "MenuItem";
            if (i == m_SelectedIndex)
                btn->StyleVariant = "primary";
            btn->WidthMode = UISizeMode::Fill;
            btn->HeightMode = UISizeMode::Auto;
            m_ResultsBox->AddChild(btn);
        }
    }

    void UICommandPalette::ExecuteSelected()
    {
        if (m_SelectedIndex < 0 || m_SelectedIndex >= static_cast<int>(m_Filtered.size()))
            return;

        auto cb = m_Filtered[m_SelectedIndex].OnExecute;
        m_Context.ClearOverlay();
        if (cb)
            cb();
    }
}
