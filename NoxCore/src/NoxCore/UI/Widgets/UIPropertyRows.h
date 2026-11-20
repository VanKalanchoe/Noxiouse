#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "NoxCore/UI/Core/UIElement.h"
#include "NoxCore/UI/Widgets/UIMenu.h"

namespace Nox::UI
{
    class UIContext;
    class UIDragScalar;
    class UIStackBox;

    // A label and its editor side by side: the label column a third of the width (76 to 150 px), the editor the rest, both centred
    // on the row's height (an editor taller than one line -- a vector, a wrapped text -- makes the row taller).
    class UIPropertyRow : public UIElement
    {
    public:
        UIPropertyRow(const Ref<UIElement>& label, const Ref<UIElement>& value);

        static constexpr float kGap = 8.0f;
        float LabelRatio = 0.34f;
        float MinLabelWidth = 76.0f, MaxLabelWidth = 150.0f;
        float LabelWidth(float width) const;
        float ComputeHeightForWidth(float width) override;

    protected:
        UIVec2 ComputeDesiredSize() override;
        void ArrangeChildren(const UIRect& rect) override;

    private:
        Ref<UIElement> m_Label;
        Ref<UIElement> m_Value;
    };

    // A card with a header that opens and closes its body: a chevron and the title (a long one ends in "..."), and at the right a
    // "..." button when there is a menu (MenuItems, read when it is opened). Theme classes Card and SectionHeader.
    struct UICollapsibleCard
    {
        Ref<UIStackBox> Card;
        Ref<UIStackBox> Body;

        static UICollapsibleCard Create(const std::string& title, bool expanded, std::function<void(bool expanded)> onExpandChanged = {},
                                        std::function<std::vector<UIMenuItem>()> menuItems = {}, const std::string& menuTooltip = "Options");
    };

    // Rows of live settings in a panel: each row reads its value through a getter every frame and writes through a setter, so a value
    // changed elsewhere (a preset, a console command, a script) shows at once -- the panel is built once, not every frame. A row can
    // be shown only while a condition holds (ShowWhen) and greyed out with the reason next to it while another does not (EnableWhen).
    // Each row follows its value through the UI's per-element sync (UIElement::OnSync): nothing to run by hand, nothing left running
    // after the rows are gone. Rows go into `body` (a vertical UIStackBox); Section and Group return builders for nested bodies.
    //
    // Theme tokens: PropertyLabel (row labels), PropertySubheader (Header), PropertyReason (why a row is greyed out).
    class UIPropertyRows
    {
    public:
        using Cond = std::function<bool()>;

        UIPropertyRows(Ref<UIStackBox> body, UIContext* context);

        // A collapsible card (its open state kept in `expanded` by title, so a rebuild keeps it); its rows go into the returned builder.
        UIPropertyRows Section(const std::string& title, bool openByDefault, std::unordered_map<std::string, bool>* expanded,
                               const std::string& tooltip = {});
        // Rows indented under the last one (a setting's details), in a box of their own the conditions can show or hide at once.
        UIPropertyRows Group();

        // A label and any editor as one row (UIPropertyRow); the label's tooltip is the whole label and `tooltip`.
        Ref<UIElement> Row(const std::string& label, const Ref<UIElement>& editor, const std::string& tooltip = {});

        Ref<UIElement> Check(const std::string& label, std::function<bool()> get, std::function<void(bool)> set, const std::string& tooltip = {});
        // A field with a fill bar between min and max (drag across it or click to type).
        Ref<UIElement> Slider(const std::string& label, std::function<float()> get, std::function<void(float)> set, float min, float max,
                              const char* format = "{:.2f}", const std::string& tooltip = {});
        Ref<UIElement> Drag(const std::string& label, std::function<float()> get, std::function<void(float)> set, float speed,
                            float min = 0.0f, float max = 0.0f, const char* format = "{:.2f}", const std::string& tooltip = {});
        // applyOnRelease: `set` runs once when a drag is let go or a value typed, not on every step (something costly to apply).
        Ref<UIElement> Int(const std::string& label, std::function<int()> get, std::function<void(int)> set, int min, int max,
                           const std::string& tooltip = {}, bool applyOnRelease = false);
        Ref<UIElement> Choice(const std::string& label, std::vector<std::string> items, std::function<int()> get, std::function<void(int)> set,
                              const std::string& tooltip = {});
        // A colour or a text over a value whose address stays put (settings, a component edited in place).
        Ref<UIElement> Color3(const std::string& label, glm::vec3* target, std::function<void()> onChange = {}, const std::string& tooltip = {});
        Ref<UIElement> Color4(const std::string& label, glm::vec4* target, std::function<void()> onChange = {}, const std::string& tooltip = {});
        Ref<UIElement> String(const std::string& label, std::string* target, const std::string& placeholder = {}, std::function<void()> onChange = {},
                              const std::string& tooltip = {});
        // Text that follows a value (stats); `color` the text's.
        Ref<UIElement> Text(std::function<std::string()> text, const glm::vec4& color = { 0.60f, 0.66f, 0.76f, 1.0f });
        Ref<UIElement> Note(const std::string& text, const glm::vec4& color = { 0.60f, 0.66f, 0.76f, 1.0f });
        Ref<UIElement> Button(const std::string& label, std::function<void()> onClick, const std::string& tooltip = {});
        Ref<UIElement> Header(const std::string& title, const std::string& tooltip = {});
        Ref<UIElement> Separator();

        // The element is shown only while `when` holds.
        void ShowWhen(const Ref<UIElement>& element, Cond when);
        // The element is greyed out while `when` does not hold; `reason` (when given) says why, after the row's editor.
        void EnableWhen(const Ref<UIElement>& element, Cond when, std::function<std::string()> reason = {});

        // A 0..1 (or min..max) value as one field: its fill bar is the slider (drag across it), a click types the exact value.
        static Ref<UIDragScalar> SliderField(float value, float min, float max, std::function<void(float)> onChanged);

        const Ref<UIStackBox>& GetBody() const { return m_Body; }
        UIContext* GetContext() const { return m_Context; }

    private:
        Ref<UIStackBox> m_Body;
        UIContext* m_Context = nullptr;
    };
}
