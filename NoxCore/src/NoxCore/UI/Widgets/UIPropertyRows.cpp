#include "UIPropertyRows.h"

#include <algorithm>
#include <cmath>

#include "NoxCore/Renderer/Font.h"
#include "NoxCore/UI/Core/UIContext.h"
#include "NoxCore/UI/Core/UITheme.h"
#include "NoxCore/UI/Layout/UILayout.h"
#include "NoxCore/UI/Rendering/UIDrawList.h"
#include "NoxCore/UI/Rendering/UIIcons.h"
#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIColorPicker.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UIDropdown.h"
#include "NoxCore/UI/Widgets/UIIconButton.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UISeparator.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    namespace
    {
        constexpr const char* kReasonClass = "PropertyReason";

        UIColor Token(UIContext* context, const char* name, const UIColor& fallback)
        {
            if (!context)
                context = UIContext::GetCurrent();
            return context && context->GetTheme() ? context->GetTheme()->GetToken(name, fallback) : fallback;
        }

        Ref<UILabel> ColoredLabel(const std::string& text, float size, const UIColor& color)
        {
            Ref<UILabel> label(new UILabel(text, size));
            label->FontSizeOverride = size;
            label->UseColorOverride = true;
            label->ColorOverride = color;
            return label;
        }

        // The value side of a row: the editor, then (hidden until EnableWhen needs it) why it is greyed out.
        Ref<UIStackBox> ValueBox(UIContext* context, const Ref<UIElement>& editor, bool fillEditor)
        {
            Ref<UIStackBox> box(new UIStackBox(UIOrientation::Horizontal, 6.0f));
            box->WidthMode = UISizeMode::Fill;
            box->VAlign = UIAlignment::Center;
            if (editor)
            {
                if (fillEditor)
                {
                    editor->WidthMode = UISizeMode::Fill;
                    editor->FlexWeight = 1.0f;
                }
                box->AddChild(editor);
            }
            Ref<UILabel> reason = ColoredLabel("", 12.5f, Token(context, "PropertyReason", { 0.85f, 0.66f, 0.36f, 1.0f }));
            reason->StyleClass = kReasonClass;
            reason->VAlign = UIAlignment::Center;
            reason->Visible = false;
            box->AddChild(reason);
            return box;
        }

        UILabel* ReasonLabelOf(UIElement* element)
        {
            if (!element)
                return nullptr;
            if (auto* label = dynamic_cast<UILabel*>(element); label && label->StyleClass == kReasonClass)
                return label;
            for (const auto& child : element->GetChildren())
                if (UILabel* found = ReasonLabelOf(child.get()))
                    return found;
            return nullptr;
        }

        // A card's header: a chevron and the title; a click opens or closes the card.
        class UICardHeader final : public UIElement
        {
        public:
            UICardHeader(std::string title, bool expanded) : m_Title(std::move(title)), m_Expanded(expanded)
            {
                StyleClass = "SectionHeader";
                DrawBackground = false;
                HitTestMode = UIHitTestMode::Self;
                WidthMode = UISizeMode::Fill;
                FlexWeight = 1.0f;
                HeightMode = UISizeMode::Auto;
            }

            std::function<void()> OnToggle;
            void SetExpanded(bool expanded) { m_Expanded = expanded; }

            bool OnPointerUp(const UIPointerEvent& e) override
            {
                if (e.Button == UIMouseButton::Left && IsEnabled() && GetBounds().Contains(e.Position))
                {
                    auto onToggle = OnToggle;
                    if (onToggle)
                        onToggle();
                    return true;
                }
                return false;
            }

        protected:
            UIVec2 ComputeDesiredSize() override
            {
                const UIStyleBox& style = GetResolvedStyle();
                const UIVec2 ts = UILabel::MeasureText(m_Title, nullptr, style.FontSize > 0.0f ? style.FontSize : 16.5f);
                return { 28.0f + ts.x + 8.0f, std::max(24.0f, ts.y + 4.0f) };
            }

            void OnPaint(UIDrawList& drawList, const UITheme& theme) override
            {
                const UIRect& b = GetBounds();
                const UIStyleBox& style = GetResolvedStyle();
                const float cy = (b.Min.y + b.Max.y) * 0.5f;
                const bool hovered = HasState(GetState(), UIState::Hovered);
                const UIColor textColor = style.TextColor;
                const UIColor chevronColor = hovered ? theme.GetToken("Accent", textColor) : theme.GetToken("TextMuted", textColor);
                Icons::DrawChevron(drawList, { b.Min.x + 9.0f, cy }, 8.0f, m_Expanded, chevronColor, 1.7f);

                // The title never leaves the header: a long one ends in "...".
                const float fontSize = style.FontSize > 0.0f ? style.FontSize : 16.5f;
                const std::string shown = UILabel::ElideText(m_Title, nullptr, fontSize, b.Max.x - (b.Min.x + 22.0f) - 2.0f);
                const UIVec2 ts = UILabel::MeasureText(shown, nullptr, fontSize);
                drawList.AddText(shown, nullptr, { b.Min.x + 22.0f, cy - ts.y * 0.5f }, fontSize, textColor);
            }

        private:
            std::string m_Title;
            bool m_Expanded = true;
        };
    }

    UIPropertyRow::UIPropertyRow(const Ref<UIElement>& label, const Ref<UIElement>& value)
        : m_Label(label), m_Value(value)
    {
        WidthMode = UISizeMode::Fill;
        AddChild(label);
        AddChild(value);
    }

    float UIPropertyRow::LabelWidth(float width) const
    {
        return std::clamp(width * LabelRatio, MinLabelWidth, MaxLabelWidth);
    }

    float UIPropertyRow::ComputeHeightForWidth(float width)
    {
        const float labelW = LabelWidth(width);
        const float valueW = std::max(0.0f, width - labelW - kGap);
        return std::max(m_Label->ComputeHeightForWidth(labelW), m_Value->ComputeHeightForWidth(valueW));
    }

    UIVec2 UIPropertyRow::ComputeDesiredSize()
    {
        const UIVec2 l = m_Label->Measure();
        const UIVec2 v = m_Value->Measure();
        return { std::min(l.x, MaxLabelWidth) + kGap + v.x, std::max(l.y, v.y) };
    }

    void UIPropertyRow::ArrangeChildren(const UIRect& rect)
    {
        const float labelW = LabelWidth(rect.Width());
        const float valueX = rect.Min.x + labelW + kGap;
        const float valueH = std::min(rect.Height(), m_Value->ComputeHeightForWidth(std::max(0.0f, rect.Max.x - valueX)));
        const float valueY = rect.Min.y + (rect.Height() - valueH) * 0.5f;
        const float labelH = std::min(rect.Height(), m_Label->GetDesiredSize().y);
        const float labelY = rect.Min.y + (rect.Height() - labelH) * 0.5f;
        m_Label->Arrange({ { rect.Min.x, labelY }, { rect.Min.x + labelW, labelY + labelH } });
        m_Value->Arrange({ { valueX, valueY }, { rect.Max.x, valueY + valueH } });
    }

    UICollapsibleCard UICollapsibleCard::Create(const std::string& title, bool expanded, std::function<void(bool)> onExpandChanged,
                                                std::function<std::vector<UIMenuItem>()> menuItems, const std::string& menuTooltip)
    {
        Ref<UIStackBox> card(new UIStackBox(UIOrientation::Vertical, 0.0f));
        card->WidthMode = UISizeMode::Fill;
        card->StyleClass = "Card";
        card->DrawBackground = true;
        card->Padding = UIThickness::All(0.0f);

        Ref<UIStackBox> headerRow(new UIStackBox(UIOrientation::Horizontal, 4.0f));
        headerRow->WidthMode = UISizeMode::Fill;
        headerRow->StyleClass = "SectionHeader";
        headerRow->DrawBackground = true;
        headerRow->Padding = UIThickness::Symmetric(8.0f, 5.0f);
        headerRow->VAlign = UIAlignment::Center;

        Ref<UIStackBox> body(new UIStackBox(UIOrientation::Vertical, 6.0f));
        body->WidthMode = UISizeMode::Fill;
        body->Padding = UIThickness::All(8.0f);
        body->Visible = expanded;

        Ref<UICardHeader> header(new UICardHeader(title, expanded));
        UICardHeader* headerRaw = header.get();
        UIStackBox* bodyRaw = body.get();
        header->OnToggle = [headerRaw, bodyRaw, onExpandChanged]()
        {
            const bool next = !bodyRaw->Visible;
            bodyRaw->Visible = next;
            headerRaw->SetExpanded(next);
            if (onExpandChanged)
                onExpandChanged(next);
        };
        headerRow->AddChild(header);

        if (menuItems)
        {
            Ref<UIIconButton> more(new UIIconButton([](UIDrawList& drawList, const UIRect& b, const UIColor& color)
            {
                Icons::DrawMoreDots(drawList, b.Center(), 12.0f, color, true);
            }, menuTooltip));
            more->FixedSize = { 24.0f, 22.0f };
            more->OnClickAt = [menuItems](const UIRect& b)
            {
                UIContext* context = UIContext::GetCurrent();
                std::vector<UIMenuItem> items = menuItems();
                if (context && !items.empty())
                    UIMenuPopup::ShowPopupMenu(*context, b, std::move(items), true);
            };
            headerRow->AddChild(more);
        }

        card->AddChild(headerRow);
        card->AddChild(body);
        return { card, body };
    }

    UIPropertyRows::UIPropertyRows(Ref<UIStackBox> body, UIContext* context)
        : m_Body(std::move(body)), m_Context(context)
    {
    }

    Ref<UIDragScalar> UIPropertyRows::SliderField(float value, float min, float max, std::function<void(float)> onChanged)
    {
        Ref<UIDragScalar> field(new UIDragScalar(value, 0.01f, std::move(onChanged)));
        field->WidthMode = UISizeMode::Fill;
        field->Min = min;
        field->Max = max;
        field->ShowFill = true;
        field->Format = (max - min) > 20.0f ? "{:.1f}" : "{:.3f}";
        field->Tooltip = "Drag to change, click to type a value";
        return field;
    }

    Ref<UIElement> UIPropertyRows::Row(const std::string& labelText, const Ref<UIElement>& editor, const std::string& tooltip)
    {
        Ref<UILabel> label = ColoredLabel(labelText, 15.0f, Token(m_Context, "PropertyLabel", { 0.72f, 0.77f, 0.85f, 1.0f }));
        label->VAlign = UIAlignment::Center;
        // A name too long for its column ends in "..."; the whole name is in the tooltip.
        label->Tooltip = tooltip.empty() ? labelText : labelText + "\n" + tooltip;

        Ref<UIStackBox> value(new UIStackBox(UIOrientation::Horizontal, 4.0f));
        value->WidthMode = UISizeMode::Fill;
        value->VAlign = UIAlignment::Center;
        if (editor)
            value->AddChild(editor);

        Ref<UIElement> row(new UIPropertyRow(label, value));
        if (!tooltip.empty())
            row->Tooltip = tooltip;
        m_Body->AddChild(row);
        return row;
    }

    UIPropertyRows UIPropertyRows::Section(const std::string& title, bool openByDefault, std::unordered_map<std::string, bool>* expanded,
                                           const std::string& tooltip)
    {
        bool open = openByDefault;
        if (expanded)
            if (const auto found = expanded->find(title); found != expanded->end())
                open = found->second;
        UICollapsibleCard card = UICollapsibleCard::Create(title, open, [expanded, title](bool value)
        {
            if (expanded)
                (*expanded)[title] = value;
        });
        if (!tooltip.empty())
            card.Card->Tooltip = tooltip;
        m_Body->AddChild(card.Card);
        return UIPropertyRows(card.Body, m_Context);
    }

    UIPropertyRows UIPropertyRows::Group()
    {
        Ref<UIStackBox> box(new UIStackBox(UIOrientation::Vertical, 2.0f));
        box->WidthMode = UISizeMode::Fill;
        box->Padding = UIThickness{ 14.0f, 0.0f, 0.0f, 0.0f };
        m_Body->AddChild(box);
        return UIPropertyRows(box, m_Context);
    }

    Ref<UIElement> UIPropertyRows::Check(const std::string& label, std::function<bool()> get, std::function<void(bool)> set, const std::string& tooltip)
    {
        Ref<UICheckBox> check(new UICheckBox("", get ? get() : false, [set](bool value) { if (set) set(value); }));
        Ref<UIElement> row = Row(label, ValueBox(m_Context, check, false), tooltip);
        AppendSync(*check, [check = check.get(), get]()
        {
            if (get && !HasState(check->GetState(), UIState::Pressed))
                check->Checked = get();
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Slider(const std::string& label, std::function<float()> get, std::function<void(float)> set, float min, float max,
                                          const char* format, const std::string& tooltip)
    {
        Ref<UIDragScalar> field = SliderField(get ? get() : min, min, max, [set](float value) { if (set) set(value); });
        field->Format = format;
        if (!tooltip.empty())
            field->Tooltip = tooltip + "\n(drag across it, or click to type a value)";
        Ref<UIElement> row = Row(label, ValueBox(m_Context, field, true), tooltip);
        AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = get();
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Drag(const std::string& label, std::function<float()> get, std::function<void(float)> set, float speed, float min,
                                        float max, const char* format, const std::string& tooltip)
    {
        Ref<UIDragScalar> field(new UIDragScalar(get ? get() : 0.0f, speed, [set](float value) { if (set) set(value); }));
        field->Min = min;
        field->Max = max;
        field->Format = format;
        Ref<UIElement> row = Row(label, ValueBox(m_Context, field, true), tooltip);
        AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = get();
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Int(const std::string& label, std::function<int()> get, std::function<void(int)> set, int min, int max,
                                       const std::string& tooltip, bool applyOnRelease)
    {
        const auto apply = [set, min, max](float value)
        {
            if (set)
                set(std::clamp(static_cast<int>(std::lround(value)), min, max));
        };
        Ref<UIDragScalar> field = SliderField(static_cast<float>(get ? get() : min), static_cast<float>(min), static_cast<float>(max),
                                              applyOnRelease ? std::function<void(float)>() : std::function<void(float)>(apply));
        if (applyOnRelease)
            field->OnCommitted = apply; // (costly to apply: once, when the drag is let go or the value typed)
        field->Format = "{:.0f}";
        field->Step = 1.0f;
        Ref<UIElement> row = Row(label, ValueBox(m_Context, field, true), tooltip);
        AppendSync(*field, [field = field.get(), get]()
        {
            if (get && !field->IsInteracting())
                field->Value = static_cast<float>(get());
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Choice(const std::string& label, std::vector<std::string> items, std::function<int()> get, std::function<void(int)> set,
                                          const std::string& tooltip)
    {
        Ref<UIDropdown> dropdown(new UIDropdown(m_Context, std::move(items), get ? get() : 0, [set](int index) { if (set) set(index); }));
        Ref<UIElement> row = Row(label, ValueBox(m_Context, dropdown, true), tooltip);
        AppendSync(*dropdown, [dropdown = dropdown.get(), get]()
        {
            if (get && dropdown->GetSelectedIndex() != get())
                dropdown->SetSelectedIndex(get(), false);
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Color3(const std::string& label, glm::vec3* target, std::function<void()> onChange, const std::string& tooltip)
    {
        Ref<UIColorEdit> color(new UIColorEdit(m_Context, UIColor(target->r, target->g, target->b, 1.0f), [target, onChange](const UIColor& c)
        {
            target->r = c.r;
            target->g = c.g;
            target->b = c.b;
            if (onChange)
                onChange();
        }));
        color->ShowAlpha = false;
        color->WidthMode = UISizeMode::Fill;
        Ref<UIElement> row = Row(label, color, tooltip);
        AppendSync(*color, [color = color.get(), target]()
        {
            const UIColor cur = color->GetColor();
            if (std::abs(cur.r - target->r) > 1e-4f || std::abs(cur.g - target->g) > 1e-4f || std::abs(cur.b - target->b) > 1e-4f)
                color->SetColor({ target->r, target->g, target->b, 1.0f }, false);
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Color4(const std::string& label, glm::vec4* target, std::function<void()> onChange, const std::string& tooltip)
    {
        Ref<UIColorEdit> color(new UIColorEdit(m_Context, *target, [target, onChange](const UIColor& c)
        {
            *target = c;
            if (onChange)
                onChange();
        }));
        color->WidthMode = UISizeMode::Fill;
        Ref<UIElement> row = Row(label, color, tooltip);
        AppendSync(*color, [color = color.get(), target]()
        {
            if (glm::length(color->GetColor() - *target) > 1e-4f)
                color->SetColor(*target, false);
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::String(const std::string& label, std::string* target, const std::string& placeholder, std::function<void()> onChange,
                                          const std::string& tooltip)
    {
        Ref<UITextInput> input(new UITextInput(*target, placeholder));
        input->WidthMode = UISizeMode::Fill;
        input->OnChanged = [target, onChange](const std::string& v)
        {
            *target = v;
            if (onChange)
                onChange();
        };
        Ref<UIElement> row = Row(label, input, tooltip);
        AppendSync(*input, [input = input.get(), target]()
        {
            if (!HasState(input->GetState(), UIState::Focused) && input->Text != *target)
                input->Text = *target;
        });
        return row;
    }

    Ref<UIElement> UIPropertyRows::Text(std::function<std::string()> text, const glm::vec4& color)
    {
        Ref<UILabel> label = ColoredLabel(text ? text() : std::string(), 13.5f, color);
        label->WordWrap = true;
        label->WidthMode = UISizeMode::Fill;
        m_Body->AddChild(label);
        AppendSync(*label, [label = label.get(), text]()
        {
            if (text)
            {
                std::string next = text();
                if (next != label->Text)
                    label->Text = std::move(next);
            }
        });
        return label;
    }

    Ref<UIElement> UIPropertyRows::Note(const std::string& text, const glm::vec4& color)
    {
        Ref<UILabel> label = ColoredLabel(text, 13.5f, color);
        label->WordWrap = true;
        label->WidthMode = UISizeMode::Fill;
        m_Body->AddChild(label);
        return label;
    }

    Ref<UIElement> UIPropertyRows::Button(const std::string& label, std::function<void()> onClick, const std::string& tooltip)
    {
        Ref<UIButton> button(new UIButton(label, std::move(onClick)));
        button->HAlign = UIAlignment::Start;
        if (!tooltip.empty())
            button->Tooltip = tooltip;
        m_Body->AddChild(button);
        return button;
    }

    Ref<UIElement> UIPropertyRows::Header(const std::string& title, const std::string& tooltip)
    {
        Ref<UILabel> label = ColoredLabel(title, 14.5f, Token(m_Context, "PropertySubheader", { 0.78f, 0.84f, 0.93f, 1.0f }));
        label->Margin = UIThickness{ 0.0f, 6.0f, 0.0f, 2.0f };
        if (!tooltip.empty())
            label->Tooltip = tooltip;
        m_Body->AddChild(label);
        return label;
    }

    Ref<UIElement> UIPropertyRows::Separator()
    {
        Ref<UISeparator> separator(new UISeparator());
        separator->Margin = UIThickness::Symmetric(0.0f, 3.0f);
        m_Body->AddChild(separator);
        return separator;
    }

    void UIPropertyRows::ShowWhen(const Ref<UIElement>& element, Cond when)
    {
        AppendSync(*element, [element = element.get(), when]()
        {
            const bool show = !when || when();
            if (element->Visible != show)
                element->Visible = show;
        });
    }

    void UIPropertyRows::EnableWhen(const Ref<UIElement>& element, Cond when, std::function<std::string()> reason)
    {
        UILabel* reasonLabel = ReasonLabelOf(element.get());
        element->VeilWhenDisabled = true;
        AppendSync(*element, [element = element.get(), when, reason, reasonLabel]()
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
