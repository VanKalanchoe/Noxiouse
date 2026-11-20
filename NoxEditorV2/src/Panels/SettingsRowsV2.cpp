#include "SettingsRowsV2.h"

#include <algorithm>
#include <cmath>

#include "Panels/InspectorWidgetsV2.h"
#include "NoxCore/UI/Core/UIBuilder.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UILabel.h"

namespace Nox
{
    namespace
    {
        constexpr const char* kReasonClass = "SettingsReason";

        // The value side of a row: the editor, then (hidden until EnableWhen needs it) why it is greyed out.
        Ref<UI::UIStackBox> ValueBox(const Ref<UI::UIElement>& editor, bool fillEditor)
        {
            auto box = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Horizontal, 6.0f);
            box->WidthMode = UI::UISizeMode::Fill;
            box->VAlign = UI::UIAlignment::Center;
            if (editor)
            {
                if (fillEditor)
                {
                    editor->WidthMode = UI::UISizeMode::Fill;
                    editor->FlexWeight = 1.0f;
                }
                box->AddChild(editor);
            }
            auto reason = UI::Label("", 12.5f);
            reason->StyleClass = kReasonClass;
            reason->UseColorOverride = true;
            reason->ColorOverride = { 0.85f, 0.66f, 0.36f, 1.0f };
            reason->VAlign = UI::UIAlignment::Center;
            reason->Visible = false;
            box->AddChild(reason);
            return box;
        }

        UI::UILabel* ReasonLabelOf(UI::UIElement* element)
        {
            if (!element)
                return nullptr;
            if (auto* label = dynamic_cast<UI::UILabel*>(element); label && label->StyleClass == kReasonClass)
                return label;
            for (const auto& child : element->GetChildren())
                if (UI::UILabel* found = ReasonLabelOf(child.get()))
                    return found;
            return nullptr;
        }
    }

    SettingsRowsV2::SettingsRowsV2(Ref<UI::UIStackBox> body, UI::UIContext* context)
        : m_Body(std::move(body)), m_Context(context)
    {
    }

    Ref<UI::UIElement> SettingsRowsV2::AddRow(const std::string& label, const Ref<UI::UIElement>& editor, const std::string& tooltip)
    {
        InspectorCardBuilder builder{ m_Body, {}, m_Context };
        builder.AddRow(label, editor, tooltip);
        return m_Body->GetChildren().back();
    }

    SettingsRowsV2 SettingsRowsV2::Section(const std::string& title, bool openByDefault, std::unordered_map<std::string, bool>* expanded,
                                           const std::string& tooltip)
    {
        bool open = openByDefault;
        if (expanded)
            if (const auto found = expanded->find(title); found != expanded->end())
                open = found->second;
        InspectorCardResult card = CreateCollapsibleCard(title, open, [expanded, title](bool value)
        {
            if (expanded)
                (*expanded)[title] = value;
        }, false, {}, {});
        if (!tooltip.empty())
            card.Card->Tooltip = tooltip;
        m_Body->AddChild(card.Card);
        return SettingsRowsV2(card.Body, m_Context);
    }

    SettingsRowsV2 SettingsRowsV2::Group()
    {
        auto box = UI::MakeUI<UI::UIStackBox>(UI::UIOrientation::Vertical, 2.0f);
        box->WidthMode = UI::UISizeMode::Fill;
        box->Padding = UI::UIThickness{ 14.0f, 0.0f, 0.0f, 0.0f };
        m_Body->AddChild(box);
        return SettingsRowsV2(box, m_Context);
    }

    Ref<UI::UIElement> SettingsRowsV2::Check(const std::string& label, std::function<bool()> get, std::function<void(bool)> set, const std::string& tooltip)
    {
        auto box = UI::CheckBox("", get ? get() : false, [set](bool value) { if (set) set(value); });
        Ref<UI::UICheckBox> check = box;
        Ref<UI::UIElement> row = AddRow(label, ValueBox(check, false), tooltip);
        UI::AppendSync(*check, [check = check.get(), get]()
        {
            if (get && !UI::HasState(check->GetState(), UI::UIState::Pressed))
                check->Checked = get();
        });
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::Slider(const std::string& label, std::function<float()> get, std::function<void(float)> set, float min,
                                              float max, const char* format, const std::string& tooltip)
    {
        Ref<UI::UIDragScalar> field = CreateSliderField(get ? get() : min, min, max, [set](float value) { if (set) set(value); });
        field->Format = format;
        if (!tooltip.empty())
            field->Tooltip = tooltip + "\n(drag across it, or click to type a value)";
        Ref<UI::UIElement> row = AddRow(label, ValueBox(field, true), tooltip);
        UI::AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = get();
        });
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::Drag(const std::string& label, std::function<float()> get, std::function<void(float)> set, float speed,
                                            float min, float max, const char* format, const std::string& tooltip)
    {
        auto field = UI::MakeUI<UI::UIDragScalar>(get ? get() : 0.0f, speed, [set](float value) { if (set) set(value); });
        field->Min = min;
        field->Max = max;
        field->Format = format;
        Ref<UI::UIElement> row = AddRow(label, ValueBox(field, true), tooltip);
        UI::AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = get();
        });
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::Int(const std::string& label, std::function<int()> get, std::function<void(int)> set, int min, int max,
                                           const std::string& tooltip, bool applyOnRelease)
    {
        const auto apply = [set, min, max](float value)
        {
            if (set)
                set(std::clamp(static_cast<int>(std::lround(value)), min, max));
        };
        Ref<UI::UIDragScalar> field = CreateSliderField(static_cast<float>(get ? get() : min), static_cast<float>(min), static_cast<float>(max),
            applyOnRelease ? std::function<void(float)>() : std::function<void(float)>(apply));
        if (applyOnRelease)
            field->OnCommitted = apply; // (expensive to apply: once, when the drag is let go or the value typed)
        field->Format = "{:.0f}";
        field->Step = 1.0f;
        Ref<UI::UIElement> row = AddRow(label, ValueBox(field, true), tooltip);
        UI::AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = static_cast<float>(get());
        });
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::Choice(const std::string& label, std::vector<std::string> items, std::function<int()> get,
                                              std::function<void(int)> set, const std::string& tooltip)
    {
        auto dropdown = UI::MakeUI<UI::UIDropdown>(m_Context, std::move(items), get ? get() : 0, [set](int index) { if (set) set(index); });
        Ref<UI::UIElement> row = AddRow(label, ValueBox(dropdown, true), tooltip);
        UI::AppendSync(*dropdown, [dropdown = dropdown.get(), get]()
        {
            if (get && dropdown->GetSelectedIndex() != get())
                dropdown->SetSelectedIndex(get(), false);
        });
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::Color3(const std::string& label, glm::vec3* target, std::function<void()> onChange, const std::string& tooltip)
    {
        InspectorCardBuilder builder{ m_Body, {}, m_Context };
        builder.AddColor3(label, target, std::move(onChange));
        Ref<UI::UIElement> row = m_Body->GetChildren().back();
        if (!tooltip.empty())
            row->Tooltip = tooltip;
        return row;
    }

    Ref<UI::UIElement> SettingsRowsV2::String(const std::string& label, std::string* target, const std::string& placeholder,
                                              std::function<void()> onChange, const std::string& tooltip)
    {
        InspectorCardBuilder builder{ m_Body, {}, m_Context };
        builder.AddString(label, target, placeholder, std::move(onChange), tooltip);
        return m_Body->GetChildren().back();
    }

    Ref<UI::UIElement> SettingsRowsV2::Text(std::function<std::string()> text, const glm::vec4& color)
    {
        auto label = UI::Label(text ? text() : std::string(), 13.5f);
        Ref<UI::UILabel> ref = label;
        ref->UseColorOverride = true;
        ref->ColorOverride = { color.r, color.g, color.b, color.a };
        ref->WordWrap = true;
        ref->WidthMode = UI::UISizeMode::Fill;
        m_Body->AddChild(ref);
        UI::AppendSync(*ref, [ref = ref.get(), text]()
        {
            if (text)
            {
                std::string next = text();
                if (next != ref->Text)
                    ref->Text = std::move(next);
            }
        });
        return ref;
    }

    Ref<UI::UIElement> SettingsRowsV2::Note(const std::string& text, const glm::vec4& color)
    {
        auto label = UI::Label(text, 13.5f);
        Ref<UI::UILabel> ref = label;
        ref->UseColorOverride = true;
        ref->ColorOverride = { color.r, color.g, color.b, color.a };
        ref->WordWrap = true;
        ref->WidthMode = UI::UISizeMode::Fill;
        m_Body->AddChild(ref);
        return ref;
    }

    Ref<UI::UIElement> SettingsRowsV2::Button(const std::string& label, std::function<void()> onClick, const std::string& tooltip)
    {
        Ref<UI::UIButton> button(new UI::UIButton(label, std::move(onClick)));
        button->HAlign = UI::UIAlignment::Start;
        if (!tooltip.empty())
            button->Tooltip = tooltip;
        m_Body->AddChild(button);
        return button;
    }

    Ref<UI::UIElement> SettingsRowsV2::Header(const std::string& title, const std::string& tooltip)
    {
        InspectorCardBuilder builder{ m_Body, {}, m_Context };
        builder.AddSubheader(title, tooltip);
        return m_Body->GetChildren().back();
    }

    Ref<UI::UIElement> SettingsRowsV2::Separator()
    {
        InspectorCardBuilder builder{ m_Body, {}, m_Context };
        builder.AddSeparator();
        return m_Body->GetChildren().back();
    }

    void SettingsRowsV2::ShowWhen(const Ref<UI::UIElement>& element, Cond when)
    {
        UI::AppendSync(*element, [element = element.get(), when]()
        {
            const bool show = !when || when();
            if (element->Visible != show)
                element->Visible = show;
        });
    }

    void SettingsRowsV2::EnableWhen(const Ref<UI::UIElement>& element, Cond when, std::function<std::string()> reason)
    {
        UI::UILabel* reasonLabel = ReasonLabelOf(element.get());
        element->VeilWhenDisabled = true;
        UI::AppendSync(*element, [element = element.get(), when, reason, reasonLabel]()
        {
            const bool enabled = !when || when();
            if (element->IsEnabled() != enabled)
                element->SetEnabled(enabled);
            if (reasonLabel)
            {
                const std::string text = !enabled && reason ? reason() : std::string();
                reasonLabel->Visible = !text.empty();
                if (reasonLabel->Text != text)
                    reasonLabel->Text = text;
            }
        });
    }
}
