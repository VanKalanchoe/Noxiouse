#include "UISearchPicker.h"

#include <algorithm>
#include <cctype>

#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UIScrollView.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        std::string Lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        class UISearchPickerBackdrop final : public UIAnchorBox
        {
        public:
            UISearchPickerBackdrop(UIContext* context, const UIRect& anchor, std::vector<UISearchPickerOption> options, uint64_t current,
                                   std::function<void(uint64_t)> onPick, UISearchPickerSettings settings)
                : m_Context(context), m_Anchor(anchor), m_Options(std::move(options)), m_Current(current), m_OnPick(std::move(onPick)),
                  m_Settings(std::move(settings))
            {
                HitTestMode = UIHitTestMode::Self;

                m_Popup = Ref<UIStackBox>(new UIStackBox(UIOrientation::Vertical, 6.0f));
                m_Popup->StyleClass = "MenuPopup";
                m_Popup->DrawBackground = true;
                m_Popup->HitTestMode = UIHitTestMode::Self;
                m_Popup->WidthMode = UISizeMode::Fixed;
                m_Popup->FixedSize.x = std::max(m_Settings.MinWidth, std::min(m_Settings.MaxWidth, anchor.Width() + 70.0f));
                m_Popup->HeightMode = UISizeMode::Auto;
                m_Popup->Padding = UIThickness::All(8.0f);

                m_Search = Ref<UITextInput>(new UITextInput("", m_Settings.Placeholder));
                m_Search->WidthMode = UISizeMode::Fill;
                m_Search->OnChanged = [this](const std::string& query) { Rebuild(query); };
                m_Search->OnSubmit = [this](const std::string&) { PickFirst(); };
                m_Popup->AddChild(m_Search);

                m_Scroll = Ref<UIScrollView>(new UIScrollView());
                m_Scroll->WidthMode = UISizeMode::Fill;
                m_Scroll->HeightMode = UISizeMode::Auto;
                m_Scroll->ChildSpacing = 2.0f;
                m_Popup->AddChild(m_Scroll);

                AddChild(m_Popup);
                Rebuild("");
            }

            void FocusSearch()
            {
                if (m_Context && m_Search)
                    m_Context->SetFocus(m_Search.get());
            }

            bool OnPointerDown(const UIPointerEvent& e) override
            {
                if (AnyChildContains(e.Position))
                    return true; // (on the popup: its row acts on the release)
                if ((e.Button == UIMouseButton::Left || e.Button == UIMouseButton::Right) && m_Context)
                {
                    m_Context->ClearOverlay();
                    return true;
                }
                return false;
            }

        protected:
            void ArrangeChildren(const UIRect& contentRect) override
            {
                if (!m_Popup || !m_Popup->Visible)
                    return;
                constexpr float kMargin = 6.0f, kGap = 3.0f, kHeader = 52.0f;
                const float maxHeight = std::max(140.0f, std::min(340.0f, contentRect.Height() - kMargin * 2.0f));
                m_Scroll->MaxSize.y = std::max(60.0f, maxHeight - kHeader);

                const UIVec2 desired = m_Popup->Measure();
                const float w = std::min(desired.x, std::max(180.0f, contentRect.Width() - kMargin * 2.0f));
                float h = std::min(desired.y, maxHeight);

                // Below the anchor; above it when it does not fit below and there is more room above.
                const float below = contentRect.Max.y - kMargin - (m_Anchor.Max.y + kGap);
                const float above = (m_Anchor.Min.y - kGap) - (contentRect.Min.y + kMargin);
                float y = m_Anchor.Max.y + kGap;
                if (h > below && above > below)
                {
                    h = std::min(h, std::max(100.0f, above));
                    y = m_Anchor.Min.y - kGap - h;
                }
                else
                    y = std::clamp(y, contentRect.Min.y + kMargin, std::max(contentRect.Min.y + kMargin, contentRect.Max.y - h - kMargin));

                float x = m_Settings.AlignRight ? m_Anchor.Max.x - w : m_Anchor.Min.x;
                x = std::clamp(x, contentRect.Min.x + kMargin, std::max(contentRect.Min.x + kMargin, contentRect.Max.x - w - kMargin));
                m_Scroll->MaxSize.y = std::max(60.0f, h - kHeader);
                m_Popup->Arrange({ { x, y }, { x + w, y + h } });
            }

        private:
            void Pick(uint64_t id)
            {
                // Copied first: closing the popup destroys this.
                auto onPick = m_OnPick;
                if (m_Context)
                    m_Context->ClearOverlay();
                if (onPick)
                    onPick(id);
            }

            void PickFirst()
            {
                if (m_HasFirst)
                    Pick(m_First);
            }

            void Rebuild(const std::string& query)
            {
                m_Scroll->ClearChildren();
                m_Scroll->ScrollOffsetY = 0.0f;
                m_HasFirst = false;
                const std::string filter = Lower(query);

                const auto addRow = [this](const std::string& label, const std::string& detail, uint64_t id)
                {
                    if (!m_HasFirst)
                    {
                        m_HasFirst = true;
                        m_First = id;
                    }
                    Ref<UIButton> row(new UIButton(detail.empty() ? label : label + "   " + detail, [this, id]() { Pick(id); }));
                    row->StyleClass = "MenuItem";
                    if (id == m_Current)
                        row->StyleVariant = "primary";
                    row->WidthMode = UISizeMode::Fill;
                    row->HeightMode = UISizeMode::Auto;
                    row->Padding = UIThickness::Symmetric(8.0f, 4.0f);
                    if (!detail.empty())
                        row->Tooltip = detail;
                    m_Scroll->AddChild(row);
                };
                const auto addNote = [this](const std::string& text)
                {
                    Ref<UILabel> note(new UILabel(text, 13.0f));
                    note->UseColorOverride = true;
                    note->ColorOverride = m_Context && m_Context->GetTheme() ? m_Context->GetTheme()->GetToken("TextMuted", { 0.58f, 0.64f, 0.74f, 1.0f })
                                                                             : UIColor{ 0.58f, 0.64f, 0.74f, 1.0f };
                    note->Padding = UIThickness::Symmetric(8.0f, 5.0f);
                    m_Scroll->AddChild(note);
                };

                if (filter.empty() && !m_Settings.NoneLabel.empty())
                    addRow(m_Settings.NoneLabel, "", 0);
                size_t shown = 0, matches = 0;
                for (const UISearchPickerOption& option : m_Options)
                {
                    if (!filter.empty() && Lower(option.Label).find(filter) == std::string::npos && Lower(option.Detail).find(filter) == std::string::npos)
                        continue;
                    ++matches;
                    if (shown < m_Settings.MaxShown)
                    {
                        addRow(option.Label, option.Detail, option.Id);
                        ++shown;
                    }
                }
                if (matches == 0 && !filter.empty())
                    addNote(m_Settings.NoMatches);
                else if (matches > shown)
                    addNote("Showing " + std::to_string(shown) + " of " + std::to_string(matches) + " -- type to narrow");
            }

            UIContext* m_Context = nullptr;
            UIRect m_Anchor{};
            std::vector<UISearchPickerOption> m_Options;
            uint64_t m_Current = 0;
            std::function<void(uint64_t)> m_OnPick;
            UISearchPickerSettings m_Settings;
            Ref<UIStackBox> m_Popup;
            Ref<UITextInput> m_Search;
            Ref<UIScrollView> m_Scroll;
            bool m_HasFirst = false;
            uint64_t m_First = 0;
        };
    }

    void UISearchPicker::Show(UIContext& context, const UIRect& anchor, std::vector<UISearchPickerOption> options, uint64_t current,
                              std::function<void(uint64_t)> onPick, UISearchPickerSettings settings)
    {
        Ref<UISearchPickerBackdrop> backdrop(new UISearchPickerBackdrop(&context, anchor, std::move(options), current, std::move(onPick), std::move(settings)));
        context.SetOverlay(backdrop);
        backdrop->FocusSearch();
    }
}
