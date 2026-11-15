#include "UIPropertyGrid.h"

#include <algorithm>
#include <memory>

#include "NoxCore/UI/Widgets/UIButton.h"
#include "NoxCore/UI/Widgets/UICheckBox.h"
#include "NoxCore/UI/Widgets/UIDragScalar.h"
#include "NoxCore/UI/Widgets/UILabel.h"
#include "NoxCore/UI/Widgets/UISlider.h"
#include "NoxCore/UI/Widgets/UITextInput.h"

namespace Nox::UI
{
    UIPropertyGrid::UIPropertyGrid()
    {
        StyleClass = "PropertyGrid";
        ChildSpacing = 6.0f;
    }

    void UIPropertyGrid::ClearProperties()
    {
        ClearChildren();
    }

    Ref<UIStackBox> UIPropertyGrid::AddSection(const std::string& title, bool expanded)
    {
        Ref<UIStackBox> sectionContainer(new UIStackBox(UIOrientation::Vertical, 4.0f));
        sectionContainer->WidthMode = UISizeMode::Fill;

        Ref<UIStackBox> body(new UIStackBox(UIOrientation::Vertical, 6.0f));
        body->WidthMode = UISizeMode::Fill;
        body->Padding = { 6.0f, 4.0f, 2.0f, 6.0f };
        body->Visible = expanded;

        const std::string prefix = expanded ? "v  " : ">  ";
        Ref<UIButton> header(new UIButton(prefix + title));
        header->StyleClass = "SectionHeader";
        header->WidthMode = UISizeMode::Fill;
        header->HeightMode = UISizeMode::Fixed;
        header->FixedSize.y = 26.0f;

        UIButton* headerPtr = header.get();
        UIStackBox* bodyPtr = body.get();
        header->OnClick = [headerPtr, bodyPtr, title]()
        {
            bodyPtr->Visible = !bodyPtr->Visible;
            headerPtr->SetText((bodyPtr->Visible ? "v  " : ">  ") + title);
        };

        sectionContainer->AddChild(header);
        sectionContainer->AddChild(body);
        AddChild(sectionContainer);
        return body;
    }

    void UIPropertyGrid::AddRow(const std::string& label, const Ref<UIElement>& editor, const Ref<UIStackBox>& section)
    {
        Ref<UIStackBox> row(new UIStackBox(UIOrientation::Horizontal, 8.0f));
        row->WidthMode = UISizeMode::Fill;
        row->VAlign = UIAlignment::Center;

        Ref<UILabel> nameLabel(new UILabel(label, 13.5f));
        nameLabel->WidthMode = UISizeMode::Fill;
        nameLabel->FlexWeight = std::clamp(LabelRatio, 0.1f, 0.8f);
        nameLabel->VAlign = UIAlignment::Center;
        nameLabel->UseColorOverride = true;
        nameLabel->ColorOverride = { 0.72f, 0.77f, 0.86f, 1.0f };

        if (editor)
        {
            editor->WidthMode = UISizeMode::Fill;
            editor->FlexWeight = std::max(0.2f, 1.0f - LabelRatio);
            editor->VAlign = UIAlignment::Center;
        }

        row->AddChild(nameLabel);
        if (editor)
            row->AddChild(editor);

        if (section)
            section->AddChild(row);
        else
            AddChild(row);
    }

    void UIPropertyGrid::AddBool(const std::string& label, bool value, std::function<void(bool)> onChanged,
                                 const Ref<UIStackBox>& section)
    {
        Ref<UICheckBox> cb(new UICheckBox({}, value, std::move(onChanged)));
        AddRow(label, cb, section);
    }

    void UIPropertyGrid::AddFloatSlider(const std::string& label, float value, float min, float max,
                                        std::function<void(float)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UISlider> slider(new UISlider(value, min, max, std::move(onChanged)));
        AddRow(label, slider, section);
    }

    void UIPropertyGrid::AddDragFloat(const std::string& label, float value, float speed,
                                      std::function<void(float)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UIDragScalar> drag(new UIDragScalar(value, speed, std::move(onChanged)));
        AddRow(label, drag, section);
    }

    void UIPropertyGrid::AddVec3(const std::string& label, glm::vec3 value, float speed,
                                 std::function<void(const glm::vec3&)> onChanged, const Ref<UIStackBox>& section)
    {
        auto state = std::make_shared<glm::vec3>(value);
        Ref<UIStackBox> vecRow(new UIStackBox(UIOrientation::Horizontal, 4.0f));

        auto addAxis = [&](const char* axis, const UIColor& color, float* comp)
        {
            Ref<UIDragScalar> drag(new UIDragScalar(*comp, speed, [state, comp, onChanged](float v)
            {
                *comp = v;
                if (onChanged)
                    onChanged(*state);
            }));
            drag->Prefix = axis;
            drag->PrefixColor = color;
            drag->WidthMode = UISizeMode::Fill;
            vecRow->AddChild(drag);
        };

        addAxis("X", { 0.80f, 0.26f, 0.26f, 1.0f }, &state->x);
        addAxis("Y", { 0.25f, 0.68f, 0.34f, 1.0f }, &state->y);
        addAxis("Z", { 0.24f, 0.52f, 0.88f, 1.0f }, &state->z);

        AddRow(label, vecRow, section);
    }

    void UIPropertyGrid::AddString(const std::string& label, const std::string& value, const std::string& placeholder,
                                   std::function<void(const std::string&)> onChanged, const Ref<UIStackBox>& section)
    {
        Ref<UITextInput> input(new UITextInput(value, placeholder));
        input->OnChanged = std::move(onChanged);
        AddRow(label, input, section);
    }
}
